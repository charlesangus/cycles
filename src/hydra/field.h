/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "scene/image.h"

#include <pxr/imaging/hd/field.h>
#include <pxr/usd/sdf/path.h>

#include <mutex>
#include <set>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesField final : public PXR_NS::HdField {
 public:
  HdCyclesField(const PXR_NS::SdfPath &bprimId, const PXR_NS::TfToken &typeId);
  ~HdCyclesField() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  CCL_NS::ImageHandle GetImageHandle() const
  {
    return _handle;
  }

  /* Called by a volume that binds this field when it syncs, so that a new image for this field
   * re-syncs it. Volumes sync in parallel. */
  void AddVolume(const PXR_NS::SdfPath &volumeId);

 private:
  void MarkVolumesDirty(PXR_NS::HdSceneDelegate *sceneDelegate);

  CCL_NS::ImageHandle _handle;
  std::mutex _volumesMutex;
  std::set<PXR_NS::SdfPath> _volumes;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
