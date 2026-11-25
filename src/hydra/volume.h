/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/geometry.h"

#include <pxr/imaging/hd/volume.h>

#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesVolume final : public HdCyclesGeometry<PXR_NS::HdVolume, CCL_NS::Volume> {
 public:
  HdCyclesVolume(const PXR_NS::SdfPath &rprimId);
  ~HdCyclesVolume() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  bool UsesField(const PXR_NS::SdfPath &fieldId) const;

 private:
  void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                PXR_NS::HdDirtyBits dirtyBits,
                bool &rebuild) override;

  std::vector<PXR_NS::SdfPath> _fieldIds;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
