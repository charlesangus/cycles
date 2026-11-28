/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/usd/sdf/path.h>

#include <array>
#include <unordered_map>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesRenderPass final : public PXR_NS::HdRenderPass {
 public:
  HdCyclesRenderPass(PXR_NS::HdRenderIndex *index,
                     const PXR_NS::HdRprimCollection &collection,
                     HdCyclesSession *renderParam);
  ~HdCyclesRenderPass() override;

  bool IsConverged() const override;

 private:
  void ResetConverged();

  void _Execute(const PXR_NS::HdRenderPassStateSharedPtr &renderPassState,
                const PXR_NS::TfTokenVector &renderTags) override;

  void _MarkCollectionDirty() override;

  void _Sync() override;

  /* The frame-relative interval rprims sample motion over: closed without motion blur. */
  PXR_NS::GfVec2f MotionShutter(const PXR_NS::HdCamera *camera) const;
  /* Turns the integrator's motion blur on or off for rendering through the camera, and returns
   * whether it is on. */
  bool SetMotionBlur(CCL_NS::Scene *scene, const PXR_NS::HdCamera *camera) const;
  /* Marks the sampled rprim data dirty when the interval changed, and returns whether it did. */
  bool SetMotionShutter(const PXR_NS::GfVec2f &shutter);

  using PixelSizes = std::array<float, 9>;
  void UpdatePixelDicing(CCL_NS::Scene *scene, CCL_NS::Camera *dicingCamera);
  static bool MeasurePixelSizes(CCL_NS::Camera *dicingCamera,
                                const CCL_NS::Mesh &mesh,
                                PixelSizes &sizes);

  HdCyclesSession *_renderParam;
  unsigned int _lastSettingsVersion = 0;
  unsigned int _lastSubdivisionVersion = 0;
  PXR_NS::SdfPath _warnedDicingCamera;
  PXR_NS::SdfPath _primaryCamera;
  PXR_NS::SdfPath _renderCamera;
  bool _followPrimaryCamera = true;
  /* What pixel-space dicing last saw of the dicing camera: its matrix, view plane, full size,
   * type, field of view and near clip. */
  std::array<float, 21> _lastDicingView = {};
  /* The pixel sizes each mesh diced in pixel space was last diced at. */
  std::unordered_map<const CCL_NS::Mesh *, PixelSizes> _dicedPixelSizes;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
