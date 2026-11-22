/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_pass.h"
#include "hydra/camera.h"
#include "hydra/mesh.h"
#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/render_delegate.h"
#include "hydra/session.h"

#ifdef WITH_HYDRA_DISPLAY_DRIVER
#  include "hydra/display_driver.h"
#endif

#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/scene.h"

#include "session/session.h"

#include <pxr/imaging/hd/renderPassState.h>

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

    scene->camera->set_full_width(framing.dataWindow.GetWidth());
    scene->camera->set_full_height(framing.dataWindow.GetHeight());

    if (const auto *const camera = static_cast<const HdCyclesCamera *>(
            renderPassState->GetCamera()))
    {
      camera->ApplyCameraSettings(_renderParam, scene->camera);
    }
    else {
      HdCyclesCamera::ApplyCameraSettings(_renderParam,
                                          renderPassState->GetWorldToViewMatrix(),
                                          renderPassState->GetProjectionMatrix(),
                                          renderPassState->GetClipPlanes(),
                                          scene->camera);
    }

    // Meshes are diced as seen from the dicing camera, which is the render camera unless the
    // dicing camera setting names another
    Camera *const dicingCamera = scene->dicing_camera;
    dicingCamera->set_full_width(framing.dataWindow.GetWidth());
    dicingCamera->set_full_height(framing.dataWindow.GetHeight());
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
      namedDicingCamera->ApplyCameraSettings(_renderParam, dicingCamera);
    }
    else if (const auto *const camera = static_cast<const HdCyclesCamera *>(
                 renderPassState->GetCamera()))
    {
      camera->ApplyCameraSettings(_renderParam, dicingCamera);
    }
    else {
      HdCyclesCamera::ApplyCameraSettings(_renderParam,
                                          renderPassState->GetWorldToViewMatrix(),
                                          renderPassState->GetProjectionMatrix(),
                                          renderPassState->GetClipPlanes(),
                                          dicingCamera);
    }

    // Reset session if the session, scene, camera or AOV bindings changed
    if (scene->need_reset() || settingsVersion != _lastSettingsVersion) {
      _lastSettingsVersion = settingsVersion;

      // Reset convergence state of all render buffers
      ResetConverged();

      BufferParams buffer_params;
      buffer_params.full_x = static_cast<int>(framing.displayWindow.GetMin()[0]);
      buffer_params.full_y = static_cast<int>(framing.displayWindow.GetMin()[1]);
      buffer_params.full_width = static_cast<int>(framing.displayWindow.GetSize()[0]);
      buffer_params.full_height = static_cast<int>(framing.displayWindow.GetSize()[1]);

      buffer_params.window_x = framing.dataWindow.GetMinX() - buffer_params.full_x;
      buffer_params.window_y = framing.dataWindow.GetMinY() - buffer_params.full_y;
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

void HdCyclesRenderPass::_MarkCollectionDirty() {}

// Hydra syncs render passes before rprims, so meshes marked dirty here sync this frame
void HdCyclesRenderPass::_Sync()
{
  if (_renderParam->subdivisionVersion == _lastSubdivisionVersion) {
    return;
  }
  _lastSubdivisionVersion = _renderParam->subdivisionVersion;

  HdRenderIndex *const index = GetRenderIndex();
  HdChangeTracker &tracker = index->GetChangeTracker();
  for (const SdfPath &id : index->GetRprimIds()) {
    if (dynamic_cast<const HdCyclesMesh *>(index->GetRprim(id))) {
      tracker.MarkRprimDirty(id, HdChangeTracker::DirtyTopology);
    }
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
