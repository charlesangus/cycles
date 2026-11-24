/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/imaging/hd/rprim.h>

#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

template<typename Base, typename CyclesBase> class HdCyclesGeometry : public Base {
 public:
  HdCyclesGeometry(const PXR_NS::SdfPath &rprimId);

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits,
            const PXR_NS::TfToken &reprToken) override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 protected:
  void _InitRepr(const PXR_NS::TfToken &reprToken, PXR_NS::HdDirtyBits *dirtyBits) override;

  PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const override;

  virtual void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                        PXR_NS::HdDirtyBits dirtyBits,
                        bool &rebuild) = 0;

  CyclesBase *_geom = nullptr;
  std::vector<CCL_NS::Object *> _instances;

  /* The shutter interval this sync samples motion over, from the render param. */
  float _shutterOpen = 0.0f;
  float _shutterClose = 0.0f;

 private:
  void Initialize(PXR_NS::HdRenderParam *renderParam);

  void InitializeInstance(const int index);

  PXR_NS::GfMatrix4d _geomTransform;
  /* The transform at evenly spaced times over the shutter, empty when it does not move. */
  std::vector<PXR_NS::GfMatrix4d> _geomMotion;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
