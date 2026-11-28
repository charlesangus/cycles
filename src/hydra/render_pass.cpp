/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_pass.h"
#include "hydra/camera.h"
#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/render_delegate.h"
#include "hydra/session.h"

#ifdef WITH_HYDRA_DISPLAY_DRIVER
#  include "hydra/display_driver.h"
#endif

#include "scene/camera.h"
#include "scene/geometry.h"
#include "scene/integrator.h"
#include "scene/mesh.h"
#include "scene/scene.h"

#include "session/session.h"

#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/sceneGlobalsSchema.h>

#include <cmath>
#include <cstring>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesRenderPass::HdCyclesRenderPass(HdRenderIndex *index,
                                       const HdRprimCollection &collection,
                                       HdCyclesSession *renderParam)
    : HdRenderPass(index, collection), _renderParam(renderParam)
{
  Session *const session = _renderParam->session;
  // Reset cancel state so session thread can continue rendering
  session->progress.reset();

  session->set_output_driver(make_unique<HdCyclesOutputDriver>(renderParam));

  const auto *const renderDelegate = static_cast<const HdCyclesDelegate *>(
      GetRenderIndex()->GetRenderDelegate());
  if (renderDelegate->IsDisplaySupported()) {
#ifdef WITH_HYDRA_DISPLAY_DRIVER
    session->set_display_driver(
        make_unique<HdCyclesDisplayDriver>(renderParam, renderDelegate->GetHgi()));
#endif
  }
}

HdCyclesRenderPass::~HdCyclesRenderPass()
{
  Session *const session = _renderParam->session;
  session->cancel(true);
}

bool HdCyclesRenderPass::IsConverged() const
{
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (aovBinding.renderBuffer && !aovBinding.renderBuffer->IsConverged()) {
      return false;
    }
  }

  return true;
}

void HdCyclesRenderPass::ResetConverged()
{
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      renderBuffer->SetConverged(false);
    }
  }
}

void HdCyclesRenderPass::_Execute(const HdRenderPassStateSharedPtr &renderPassState,
                                  const TfTokenVector & /*renderTags*/)
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (session->progress.get_cancel()) {
    return;  // Something went wrong and cannot continue without recreating the session
  }

  if (scene->mutex.try_lock()) {
    auto *const renderDelegate = static_cast<HdCyclesDelegate *>(
        GetRenderIndex()->GetRenderDelegate());

    const unsigned int settingsVersion = renderDelegate->GetRenderSettingsVersion();

    // Update requested AOV bindings
    const HdRenderPassAovBindingVector &aovBindings = renderPassState->GetAovBindings();
    if (_renderParam->GetAovBindings() != aovBindings ||
        // Need to resync passes when denoising is enabled or disabled to update the pass mode
        (settingsVersion != _lastSettingsVersion && scene->integrator->use_denoise_is_modified()))
    {
      _renderParam->SyncAovBindings(aovBindings);

      if (renderDelegate->IsDisplaySupported()) {
        // Update display pass to the first requested color AOV
        const HdRenderPassAovBinding displayAovBinding = !aovBindings.empty() ?
                                                             aovBindings.front() :
                                                             HdRenderPassAovBinding();
        if (displayAovBinding.aovName == HdAovTokens->color && displayAovBinding.renderBuffer) {
          _renderParam->SetDisplayAovBinding(displayAovBinding);
        }
        else {
          _renderParam->SetDisplayAovBinding(HdRenderPassAovBinding());
        }
      }
    }

    // Update camera dimensions to the viewport size
    CameraUtilFraming framing = renderPassState->GetFraming();
    if (!framing.IsValid()) {
      const GfVec4f vp = renderPassState->GetViewport();
      framing = CameraUtilFraming(GfRect2i(GfVec2i(0), int(vp[2]), int(vp[3])));
    }

    const auto *const camera = static_cast<const HdCyclesCamera *>(renderPassState->GetCamera());

    const bool shutterIsOpen = camera && camera->GetShutterOpen() < camera->GetShutterClose();
    const bool motionBlur = SetMotionBlur(scene, camera);
    const GfVec2f shutter = MotionShutter(camera);
    const float shutterOpen = shutter[0];
    const float shutterClose = shutter[1];

    /* Rprims sampled their motion over the shutter _Sync took from the published camera. A client
     * that renders through another camera gets them synced again over this one, and this pass
     * renders nothing, as it would converge on the old motion. */
    if (camera) {
      _renderCamera = camera->GetId();
      if (_renderCamera != _primaryCamera) {
        _followPrimaryCamera = false;
      }
    }
    if (SetMotionShutter(shutter)) {
      ResetConverged();
      scene->mutex.unlock();
      return;
    }

    if (camera) {
      camera->ApplyCameraSettings(
          _renderParam, framing, renderPassState->GetWindowPolicy(), scene->camera);
    }
    else {
      HdCyclesCamera::ApplyCameraSettings(_renderParam,
                                          renderPassState->GetWorldToViewMatrix(),
                                          renderPassState->GetProjectionMatrix(),
                                          renderPassState->GetClipPlanes(),
                                          renderPassState->GetWindowPolicy(),
                                          framing,
                                          scene->camera);
    }

    // Meshes are diced as seen from the dicing camera, which is the render camera unless the
    // dicing camera setting names another
    Camera *const dicingCamera = scene->dicing_camera;
    const SdfPath &dicingCameraPath = _renderParam->subdivision.dicingCamera;
    const auto *const namedDicingCamera =
        dicingCameraPath.IsEmpty() ?
            nullptr :
            static_cast<const HdCyclesCamera *>(GetRenderIndex()->GetSprim(
                HdPrimTypeTokens->camera, dicingCameraPath));
    if (!dicingCameraPath.IsEmpty() && !namedDicingCamera &&
        _warnedDicingCamera != dicingCameraPath)
    {
      _warnedDicingCamera = dicingCameraPath;
      TF_WARN("The dicing camera %s is not a camera in the scene; meshes are diced from the "
              "render camera",
              dicingCameraPath.GetText());
    }
    if (namedDicingCamera) {
      namedDicingCamera->ApplyCameraSettings(
          _renderParam, framing, namedDicingCamera->GetWindowPolicy(), dicingCamera);
    }
    else if (camera) {
      camera->ApplyCameraSettings(
          _renderParam, framing, renderPassState->GetWindowPolicy(), dicingCamera);
    }
    else {
      HdCyclesCamera::ApplyCameraSettings(_renderParam,
                                          renderPassState->GetWorldToViewMatrix(),
                                          renderPassState->GetProjectionMatrix(),
                                          renderPassState->GetClipPlanes(),
                                          renderPassState->GetWindowPolicy(),
                                          framing,
                                          dicingCamera);
    }

    UpdatePixelDicing(scene, dicingCamera);

    if (shutterIsOpen && motionBlur) {
      scene->camera->set_shuttertime(shutterClose - shutterOpen);
      /* Volumes advect by their velocity from the current frame. */
      scene->camera->set_motion_position(MOTION_POSITION_CUSTOM);
      scene->camera->set_motion_frame_time(-shutterOpen / (shutterClose - shutterOpen));
    }

    // Reset session if the session, scene, camera or AOV bindings changed
    if (scene->need_reset() || settingsVersion != _lastSettingsVersion) {
      _lastSettingsVersion = settingsVersion;

      // Reset convergence state of all render buffers
      ResetConverged();

      // The camera's full size is the data window, so the buffers are all of it
      BufferParams buffer_params;
      buffer_params.full_x = 0;
      buffer_params.full_y = 0;
      buffer_params.full_width = framing.dataWindow.GetWidth();
      buffer_params.full_height = framing.dataWindow.GetHeight();

      buffer_params.window_x = 0;
      buffer_params.window_y = 0;
      buffer_params.window_width = framing.dataWindow.GetWidth();
      buffer_params.window_height = framing.dataWindow.GetHeight();

      buffer_params.width = buffer_params.window_width;
      buffer_params.height = buffer_params.window_height;

      session->reset(session->params, buffer_params);
    }

    scene->mutex.unlock();

    // Start Cycles render thread if not already running
    session->start();
  }

  session->draw();
}

/* Cycles tessellates only meshes that changed, so a mesh diced in pixel space keeps the
 * tessellation of where the dicing camera was until it is told to dice again. Dicing again costs
 * as much as the first time, so a mesh is diced again only when the camera changed the size of a
 * pixel, which is what the dicing rate is measured in, at its bounds by more than a fifth. */
void HdCyclesRenderPass::UpdatePixelDicing(Scene *scene, Camera *dicingCamera)
{
  std::array<float, 21> dicingView;
  const Transform dicingMatrix = dicingCamera->get_matrix();
  std::memcpy(dicingView.data(), &dicingMatrix, sizeof(float) * 12);
  dicingView[12] = dicingCamera->get_viewplane_left();
  dicingView[13] = dicingCamera->get_viewplane_right();
  dicingView[14] = dicingCamera->get_viewplane_bottom();
  dicingView[15] = dicingCamera->get_viewplane_top();
  dicingView[16] = float(dicingCamera->get_full_width());
  dicingView[17] = float(dicingCamera->get_full_height());
  dicingView[18] = float(dicingCamera->get_camera_type());
  dicingView[19] = dicingCamera->get_fov();
  dicingView[20] = dicingCamera->get_nearclip();
  const bool viewChanged = dicingView != _lastDicingView;
  _lastDicingView = dicingView;

  constexpr float tolerance = 1.2f;
  bool cameraUpdated = false;
  std::unordered_map<const Mesh *, PixelSizes> diced;
  for (Geometry *const geom : scene->geometry) {
    if (!geom->is_mesh()) {
      continue;
    }
    Mesh *const mesh = static_cast<Mesh *>(geom);
    if (mesh->get_subdivision_type() == Mesh::SUBDIVISION_NONE ||
        mesh->get_subd_adaptive_space() != Mesh::SUBDIVISION_ADAPTIVE_SPACE_PIXEL)
    {
      continue;
    }
    const auto previous = _dicedPixelSizes.find(mesh);
    const bool tessellates = mesh->need_tesselation();
    if (!tessellates && !viewChanged && previous != _dicedPixelSizes.end()) {
      diced.emplace(mesh, previous->second);
      continue;
    }

    /* Geometry tessellation updates the dicing camera the same way before it dices. */
    if (!cameraUpdated) {
      dicingCamera->set_screen_size(dicingCamera->get_full_width(),
                                    dicingCamera->get_full_height());
      dicingCamera->update(scene);
      cameraUpdated = true;
    }
    PixelSizes sizes;
    const bool measured = MeasurePixelSizes(dicingCamera, *mesh, sizes);

    /* A mesh with no record and an unchanged view was diced from this view, before its bounds
     * were known. */
    bool dice = false;
    if (!tessellates && viewChanged) {
      dice = !measured || previous == _dicedPixelSizes.end();
      for (size_t i = 0; !dice && i < sizes.size(); i++) {
        const float ratio = sizes[i] / previous->second[i];
        dice = !(ratio < tolerance && ratio > 1.0f / tolerance);
      }
      if (!dice) {
        diced.emplace(mesh, previous->second);
        continue;
      }
    }
    if (dice) {
      mesh->tag_subd_dicing_rate_modified();
      mesh->tag_update(scene, true);
    }
    if (measured) {
      diced.emplace(mesh, sizes);
    }
  }
  _dicedPixelSizes.swap(diced);
}

/* The size of a pixel at the corners and centre of the mesh's bounds. */
bool HdCyclesRenderPass::MeasurePixelSizes(Camera *dicingCamera,
                                           const Mesh &mesh,
                                           PixelSizes &sizes)
{
  if (!mesh.bounds.valid()) {
    return false;
  }
  const Transform &objectToWorld = mesh.get_subd_objecttoworld();
  for (int corner = 0; corner < 8; corner++) {
    const float3 P = make_float3((corner & 1) ? mesh.bounds.max.x : mesh.bounds.min.x,
                                 (corner & 2) ? mesh.bounds.max.y : mesh.bounds.min.y,
                                 (corner & 4) ? mesh.bounds.max.z : mesh.bounds.min.z);
    sizes[corner] = dicingCamera->world_to_raster_size(transform_point(&objectToWorld, P));
  }
  sizes[8] = dicingCamera->world_to_raster_size(
      transform_point(&objectToWorld, mesh.bounds.center()));
  for (const float size : sizes) {
    if (!(size > 0.0f) || !std::isfinite(size)) {
      return false;
    }
  }
  return true;
}

void HdCyclesRenderPass::_MarkCollectionDirty() {}

GfVec2f HdCyclesRenderPass::MotionShutter(const HdCamera *camera) const
{
  if (!camera || camera->GetShutterOpen() >= camera->GetShutterClose() ||
      !_renderParam->GetMotionBlur().value_or(true))
  {
    return GfVec2f(0.0f);
  }
  return GfVec2f(float(camera->GetShutterOpen()), float(camera->GetShutterClose()));
}

bool HdCyclesRenderPass::SetMotionBlur(Scene *scene, const HdCamera *camera) const
{
  const bool shutterIsOpen = camera && camera->GetShutterOpen() < camera->GetShutterClose();
  const bool motionBlur = _renderParam->GetMotionBlur().value_or(shutterIsOpen);
  if (scene->integrator->get_motion_blur() != motionBlur) {
    scene->integrator->set_motion_blur(motionBlur);
    scene->integrator->tag_update(scene, Integrator::UPDATE_NONE);
  }
  return motionBlur;
}

/* A volume reads its velocity only with motion blur on, hence its fields too. */
bool HdCyclesRenderPass::SetMotionShutter(const GfVec2f &shutter)
{
  if (!_renderParam->SetMotionShutter(shutter[0], shutter[1])) {
    return false;
  }
  GetRenderIndex()->GetChangeTracker().MarkAllRprimsDirty(HdChangeTracker::DirtyTransform |
                                                          HdChangeTracker::DirtyPoints |
                                                          HdChangeTracker::DirtyVolumeField);
  return true;
}

// Hydra syncs render passes after sprims and before rprims, so rprims marked dirty here sync
// this frame
void HdCyclesRenderPass::_Sync()
{
  HdRenderIndex *const index = GetRenderIndex();

  /* The render pass learns its camera only after rprims sync, but the camera the client
   * published as primary has already synced, so rprims sample over its shutter the first time.
   * After a client rendered through another camera, that camera is followed until the published
   * one changes, so the two never alternate and resync every frame. */
  SdfPath primaryCamera;
  if (HdSceneIndexBaseRefPtr sceneIndex = index->GetTerminalSceneIndex()) {
    if (HdPathDataSourceHandle ds =
            HdSceneGlobalsSchema::GetFromSceneIndex(sceneIndex).GetPrimaryCameraPrim())
    {
      primaryCamera = ds->GetTypedValue(0.0f);
    }
  }
  if (primaryCamera != _primaryCamera) {
    _primaryCamera = primaryCamera;
    _followPrimaryCamera = true;
  }
  const SdfPath &shutterCamera = _followPrimaryCamera && !primaryCamera.IsEmpty() ?
                                     primaryCamera :
                                     _renderCamera;
  if (!shutterCamera.IsEmpty()) {
    const auto *const camera = dynamic_cast<const HdCamera *>(
        index->GetSprim(HdPrimTypeTokens->camera, shutterCamera));
    /* A volume adds its velocity field only when it syncs with motion blur already on, and with
     * the shutter unchanged _Execute would not sync it again once it turns motion blur on. */
    {
      const SceneLock lock(_renderParam);
      SetMotionBlur(lock.scene, camera);
    }
    SetMotionShutter(MotionShutter(camera));
  }

  if (_renderParam->subdivisionVersion == _lastSubdivisionVersion) {
    return;
  }
  _lastSubdivisionVersion = _renderParam->subdivisionVersion;

  /* Hydra rejects marking one rprim dirty while it syncs from a scene index, but not marking them
   * all. Only meshes subdivide, so the others merely sync their topology again. */
  index->GetChangeTracker().MarkAllRprimsDirty(HdChangeTracker::DirtyTopology);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
