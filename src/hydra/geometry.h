/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "kernel/types.h"
#include "util/param.h"

#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneIndex.h>

PXR_NAMESPACE_OPEN_SCOPE
class HdRenderIndex;
PXR_NAMESPACE_CLOSE_SCOPE

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Geometry whose exported primvars the requested AOVs decide. */
class HdCyclesPrimvarSync {
 public:
  virtual ~HdCyclesPrimvarSync() = default;

  /* Exports the primvars again from the render index's scene, after the primvars the session's
   * AOVs need changed. The caller holds the scene lock. */
  virtual void SyncAovPrimvars(const PXR_NS::HdRenderIndex &renderIndex) = 0;
};

template<typename Base, typename CyclesBase>
class HdCyclesGeometry : public Base, public HdCyclesPrimvarSync {
 public:
  HdCyclesGeometry(const PXR_NS::SdfPath &rprimId);

  void SyncAovPrimvars(const PXR_NS::HdRenderIndex &renderIndex) override;

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

  virtual void PopulatePrimvars(const PXR_NS::HdSceneIndexPrim & /*prim*/) {}

  /* Whether a primvar is exported: a shader of the geometry reads it, or a requested AOV does. */
  bool NeedsPrimvar(CCL_NS::ustring name, CCL_NS::AttributeStandard std) const;

  CyclesBase *_geom = nullptr;
  std::vector<CCL_NS::Object *> _instances;

 private:
  void Initialize(PXR_NS::HdRenderParam *renderParam);

  void InitializeInstance(const int index);

  void PopulateLightgroup(const PXR_NS::HdContainerDataSourceHandle &primDs);

  const HdCyclesSession *_session = nullptr;
  PXR_NS::GfMatrix4d _geomTransform;
  CCL_NS::ustring _lightgroup;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
