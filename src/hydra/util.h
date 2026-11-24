/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "scene/attribute.h"

#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/types.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
class HdSceneDelegate;
class SdfPath;
class TfToken;
PXR_NAMESPACE_CLOSE_SCOPE

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Prim data source from the scene index, or empty if none is reachable. */
PXR_NS::HdSceneIndexPrim GetPrim(PXR_NS::HdSceneDelegate *delegate, const PXR_NS::SdfPath &id);

/* Typed child data source, or null if missing or of a different type. */
template<typename T>
typename PXR_NS::HdTypedSampledDataSource<T>::Handle GetTypedDataSource(
    const PXR_NS::HdContainerDataSourceHandle &container, const PXR_NS::TfToken &name)
{
  if (!container) {
    return nullptr;
  }
  return PXR_NS::HdTypedSampledDataSource<T>::Cast(container->Get(name));
}

/* Typed value at shutter offset 0, or `fallback` if missing or of a different type. */
template<typename T>
T GetTypedValue(const PXR_NS::HdContainerDataSourceHandle &container,
                const PXR_NS::TfToken &name,
                const T &fallback = T())
{
  if (!container) {
    return fallback;
  }
  const auto base = container->Get(name);
  if (const auto typed = PXR_NS::HdTypedSampledDataSource<T>::Cast(base)) {
    return typed->GetTypedValue(0.0f);
  }
  if (const auto sampled = PXR_NS::HdSampledDataSource::Cast(base)) {
    PXR_NS::VtValue v = sampled->GetValue(0.0f);
    if (v.IsHolding<T>()) {
      return v.UncheckedGet<T>();
    }
  }
  return fallback;
}

/* Shutter offsets to sample `ds` at for motion blur, evenly spaced from `shutterOpen` to
 * `shutterClose`, as Cycles spreads its motion steps evenly over the shutter. Their count is odd
 * so that one falls on the shutter's middle, where Cycles keeps a geometry's centre step. Empty
 * when the shutter is closed or `ds` does not vary over it. */
std::vector<float> MotionSampleTimes(const PXR_NS::HdSampledDataSourceHandle &ds,
                                     float shutterOpen,
                                     float shutterClose);

/* The points primvar sampled over the shutter into the motion steps of `attr_P`, the centre step
 * included, with the geometry's motion steps to match. When they do not move over it, or their
 * count changes, `attr_P` loses its motion steps and keeps its centre step. */
void PopulatePointMotion(CCL_NS::Geometry *geom,
                         CCL_NS::Attribute *attr_P,
                         const PXR_NS::HdPrimvarsSchema &primvars,
                         size_t numPoints,
                         float shutterOpen,
                         float shutterClose);

/* The stage's time codes per second, by which time-code shutter offsets become seconds. */
double GetTimeCodesPerSecond(PXR_NS::HdSceneDelegate *delegate);

/* Read a primvar's value at shutter offset 0. */
PXR_NS::VtValue ReadPrimvar(const PXR_NS::HdPrimvarsSchema &primvars, const PXR_NS::TfToken &name);

/* Read a primvar's interpolation. */
PXR_NS::HdInterpolation ReadPrimvarInterpolation(const PXR_NS::HdPrimvarsSchema &primvars,
                                                 const PXR_NS::TfToken &name);

/* Read a primvar's role token. */
PXR_NS::TfToken ReadPrimvarRole(const PXR_NS::HdPrimvarsSchema &primvars,
                                const PXR_NS::TfToken &name);

/* Enumerate primvar names whose interpolation matches. */
PXR_NS::TfTokenVector PrimvarNamesAtInterpolation(const PXR_NS::HdPrimvarsSchema &primvars,
                                                  PXR_NS::HdInterpolation interpolation);

/* Convert a Hydra primvar value to a Cycles attribute. */
void ApplyPrimvars(CCL_NS::AttributeSet &attributes,
                   const CCL_NS::ustring &name,
                   PXR_NS::VtValue value,
                   CCL_NS::AttributeElement elem,
                   CCL_NS::AttributeStandard std);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
