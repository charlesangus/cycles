/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "util/thread.h"

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/usd/sdf/path.h>

#include <atomic>
#include <cstdint>
#include <optional>

HDCYCLES_NAMESPACE_OPEN_SCOPE

struct SceneLock {
  SceneLock(const PXR_NS::HdRenderParam *renderParam);
  ~SceneLock();

  CCL_NS::Scene *scene;

 private:
  CCL_NS::thread_scoped_lock sceneLock;
};

/* Whether meshes are subdivided by their subdivision scheme, and how finely. A mesh's constant
 * primvars "cycles:dicing_rate" (a multiplier on dicingRate) and "cycles:max_level" override
 * these per mesh. */
struct HdCyclesSubdivisionSettings {
  bool enabled = false;
  float dicingRate = 1.0f;
  int maxLevel = 12;
  bool objectSpace = false;
  /* Empty for the render camera. */
  PXR_NS::SdfPath dicingCamera;

  bool operator==(const HdCyclesSubdivisionSettings &other) const = default;
};

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

  void SyncAovBindings(const PXR_NS::HdRenderPassAovBindingVector &aovBindings);

  void RemoveAovBinding(PXR_NS::HdRenderBuffer *renderBuffer);

  CCL_NS::Session *session;
  bool keep_nodes;

  /* The latest of the session's resets whose render has reached the render buffers, and the
   * latest whose render finished into them: a render cut short by a reset may still write them,
   * and mark them converged, after the reset was asked for. */
  std::atomic<uint64_t> resetsWritten{0};
  std::atomic<uint64_t> resetsConverged{0};

  /* Set outside Hydra's sync, read by the meshes during it. subdivisionVersion counts the
   * changes that need every mesh's topology synced again. */
  HdCyclesSubdivisionSettings subdivision;
  unsigned int subdivisionVersion = 0;
  /* Scales the lights to the radiance UsdLux gives them, which Cycles' own units overshoot.
   * lightLevelsVersion counts the changes that need every light's parameters synced again. */
  bool matchLightLevels = true;
  unsigned int lightLevelsVersion = 0;

 private:
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  mutable CCL_NS::thread_mutex _aovMutex;
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
  std::optional<bool> _backgroundTransparent;
  bool _defaultBackgroundTransparent = false;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
