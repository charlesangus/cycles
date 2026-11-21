/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "util/param.h"

#include <vector>

#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>

PXR_NAMESPACE_OPEN_SCOPE
class HdRenderIndex;
PXR_NAMESPACE_CLOSE_SCOPE

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Adds the AOV outputs every surface graph has: the instanceId AOV, and one per primvar AOV
 * that writes the primvar under the name `primvars:<name>`. */
void HdCyclesCreateAovOutputs(CCL_NS::ShaderGraph *graph,
                              const std::vector<CCL_NS::ustring> &primvarAovs);

class HdCyclesMaterial final : public PXR_NS::HdMaterial {
 public:
  HdCyclesMaterial(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesMaterial() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

  CCL_NS::Shader *GetCyclesShader() const
  {
    return _shader;
  }

  /* Rebuilds the shader graph from the render index's scene, after the primvar AOVs the session
   * renders changed. The caller holds the scene lock. */
  void SyncAovOutputs(const PXR_NS::HdRenderIndex &renderIndex, CCL_NS::Scene *scene);

 private:
  struct NodeDesc {
    CCL_NS::ShaderNode *node;
    const class UsdToCyclesMapping *mapping;
    /* UsdUVTexture applies rgba * scale + bias to what it samples, which image_texture lacks. */
    float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    /* UsdPreviewSurface turns opacity into a cutout when this is above zero. */
    float opacityThreshold = 0.0f;
  };

  void Initialize(PXR_NS::HdRenderParam *renderParam);

  /* Returns true when a change needs the shader graph rebuilt rather than a socket update. */
  bool UpdateParameters(NodeDesc &nodeDesc,
                        PXR_NS::HdMaterialNodeParameterContainerSchema params,
                        const PXR_NS::SdfPath &nodePath);

  bool UpdateParameters(PXR_NS::HdMaterialNetworkSchema network);

  void UpdateConnections(NodeDesc &nodeDesc,
                         PXR_NS::HdMaterialNodeSchema nodeSchema,
                         const PXR_NS::SdfPath &nodePath,
                         CCL_NS::ShaderGraph *shaderGraph);

  void PopulateShaderGraph(PXR_NS::HdMaterialNetworkSchema network);
  void PopulateFallbackShaderGraph(bool rebuild = false);

  std::vector<CCL_NS::ustring> PrimvarAovs() const;

  const HdCyclesSession *_session = nullptr;
  CCL_NS::Shader *_shader = nullptr;
  std::unordered_map<PXR_NS::SdfPath, NodeDesc, PXR_NS::SdfPath::Hash> _nodes;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
