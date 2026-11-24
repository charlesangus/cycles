/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/gf/camera.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>

#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesCamera final : public PXR_NS::HdCamera {
 public:
  HdCyclesCamera(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesCamera() override;

  /* Through this camera, framed by the camera's full size and its own window policy. */
  void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam, CCL_NS::Camera *cam) const;

  /* Through this camera, its window conformed by `windowPolicy` to the framing's display window
   * and pixel aspect, and cropped to its data window, which is the Cycles camera's full size. */
  void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                           const PXR_NS::CameraUtilFraming &framing,
                           PXR_NS::CameraUtilConformWindowPolicy windowPolicy,
                           CCL_NS::Camera *cam) const;

  static void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                                  const PXR_NS::GfCamera &dataUnconformedWindow,
                                  PXR_NS::CameraUtilConformWindowPolicy windowPolicy,
                                  const PXR_NS::CameraUtilFraming &framing,
                                  CCL_NS::Camera *cam);
  static void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                                  const PXR_NS::GfMatrix4d &worldToViewMatrix,
                                  const PXR_NS::GfMatrix4d &projectionMatrix,
                                  const std::vector<PXR_NS::GfVec4d> &clipPlanes,
                                  PXR_NS::CameraUtilConformWindowPolicy windowPolicy,
                                  const PXR_NS::CameraUtilFraming &framing,
                                  CCL_NS::Camera *cam);

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 private:
  PXR_NS::GfCamera _data;
  /* The transform at evenly spaced times over the shutter, empty when it does not move. */
  std::vector<PXR_NS::GfMatrix4d> _transformMotion;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
