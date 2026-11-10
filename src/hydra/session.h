/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "util/thread.h"

#include <pxr/imaging/hd/renderDelegate.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

struct SceneLock {
  SceneLock(const PXR_NS::HdRenderParam *renderParam);
  ~SceneLock();

  CCL_NS::Scene *scene;

 private:
  CCL_NS::thread_scoped_lock sceneLock;
};

class HdCyclesSession final : public PXR_NS::HdRenderParam {
 public:
  HdCyclesSession(CCL_NS::Session *session_, const bool keep_nodes);
  HdCyclesSession(const CCL_NS::SessionParams &params);
  ~HdCyclesSession() override;

  void UpdateScene();

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

  void SyncAovBindings(const PXR_NS::HdRenderPassAovBindingVector &aovBindings);

  void RemoveAovBinding(PXR_NS::HdRenderBuffer *renderBuffer);

  CCL_NS::Session *session;
  bool keep_nodes;

 private:
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  mutable CCL_NS::thread_mutex _aovMutex;
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
