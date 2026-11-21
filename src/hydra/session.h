/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "kernel/types.h"
#include "util/param.h"
#include "util/thread.h"

#include <pxr/imaging/hd/renderDelegate.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

struct SceneLock {
  SceneLock(const PXR_NS::HdRenderParam *renderParam);
  ~SceneLock();

  CCL_NS::Scene *scene;

 private:
  CCL_NS::thread_scoped_lock sceneLock;
};

/* The Cycles pass an AOV name renders: a standard Hydra AOV, a `cycles:<pass>` pass, a
 * `cycles:lightgroup:<name>` light group, or a `primvars:<name>` primvar written by every
 * material. type is PASS_NONE for a name the delegate does not render. */
struct HdCyclesAovPass {
  CCL_NS::PassType type = CCL_NS::PASS_NONE;
  CCL_NS::ustring lightgroup;
  CCL_NS::ustring primvar;
  int channels = 0;
};

HdCyclesAovPass HdCyclesPassForAov(const PXR_NS::TfToken &aovName);

/* Every standard and `cycles:<pass>` AOV name the delegate renders, in checklist order. Light
 * groups and primvars are named by the scene, so they are not listed. */
const PXR_NS::TfTokenVector &HdCyclesAovNames();

class HdCyclesSession final : public PXR_NS::HdRenderParam {
 public:
  HdCyclesSession(CCL_NS::Session *session_, const bool keep_nodes);
  HdCyclesSession(const CCL_NS::SessionParams &params);
  ~HdCyclesSession() override;

  void UpdateScene();

  /* An explicit background transparency, which wins over the default UpdateScene() picks from
   * the lights; nullopt goes back to that default. The caller holds the scene lock. */
  void SetBackgroundTransparent(std::optional<bool> transparent);

  double GetStageMetersPerUnit() const
  {
    return _stageMetersPerUnit;
  }

  void SetStageMetersPerUnit(const double stageMetersPerUnit)
  {
    _stageMetersPerUnit = stageMetersPerUnit;
  }

  PXR_NS::HdRenderPassAovBinding GetDisplayAovBinding() const
  {
    const CCL_NS::thread_scoped_lock lock(_aovMutex);
    return _displayAovBinding;
  }

  void SetDisplayAovBinding(const PXR_NS::HdRenderPassAovBinding &aovBinding)
  {
    const CCL_NS::thread_scoped_lock lock(_aovMutex);
    _displayAovBinding = aovBinding;
  }

  PXR_NS::HdRenderPassAovBindingVector GetAovBindings() const
  {
    const CCL_NS::thread_scoped_lock lock(_aovMutex);
    return _aovBindings;
  }

  /* The output driver writes into the bound render buffers on the Cycles session thread while
   * Hydra syncs or finalizes them, so it holds this lock for a whole tile write. Nothing may be
   * called back into this session's AOV accessors while it is held, as the mutex is not
   * recursive. */
  CCL_NS::thread_scoped_lock LockAovBindings() const
  {
    return CCL_NS::thread_scoped_lock(_aovMutex);
  }

  const PXR_NS::HdRenderPassAovBindingVector &GetAovBindingsLocked() const
  {
    return _aovBindings;
  }

  const PXR_NS::HdRenderPassAovBinding &GetDisplayAovBindingLocked() const
  {
    return _displayAovBinding;
  }

  /* Returns whether the primvars geometry exports changed, which needs every material and
   * geometry synced again: Hydra syncs them only when the scene changes. */
  bool SyncAovBindings(const PXR_NS::HdRenderPassAovBindingVector &aovBindings);

  /* The primvars the requested AOVs read. The caller holds the scene lock. */
  const std::vector<CCL_NS::ustring> &GetPrimvarAovs() const
  {
    return _primvarAovs;
  }

  /* Whether geometry exports a primvar for the requested AOVs, though no shader reads it: a
   * primvar AOV's, or the texture coordinates of a uv pass. The caller holds the scene lock. */
  bool AovsNeedPrimvar(CCL_NS::ustring name, CCL_NS::AttributeStandard std) const;

  void RemoveAovBinding(PXR_NS::HdRenderBuffer *renderBuffer);

  CCL_NS::Session *session;
  bool keep_nodes;

  /* The latest of the session's resets whose render has reached the render buffers, and the
   * latest whose render finished into them: a render cut short by a reset may still write them,
   * and mark them converged, after the reset was asked for. */
  std::atomic<uint64_t> resetsWritten{0};
  std::atomic<uint64_t> resetsConverged{0};

 private:
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  mutable CCL_NS::thread_mutex _aovMutex;
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
  std::vector<CCL_NS::ustring> _primvarAovs;
  bool _uvPass = false;
  std::optional<bool> _backgroundTransparent;
  bool _defaultBackgroundTransparent = false;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
