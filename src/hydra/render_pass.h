/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/renderPass.h>
#include <pxr/usd/sdf/path.h>

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

  /* Rebuilds every material's AOV outputs and exports every geometry's primvars again, which
   * Hydra syncs only when the scene changes, not when the requested AOVs do. */
  void SyncAovOutputs(CCL_NS::Scene *scene);

  void _Execute(const PXR_NS::HdRenderPassStateSharedPtr &renderPassState,
                const PXR_NS::TfTokenVector &renderTags) override;

  void _MarkCollectionDirty() override;

  void _Sync() override;

  HdCyclesSession *_renderParam;
  unsigned int _lastSettingsVersion = 0;
  unsigned int _lastSubdivisionVersion = 0;
  PXR_NS::SdfPath _warnedDicingCamera;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
