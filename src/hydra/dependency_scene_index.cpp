/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/dependency_scene_index.h"

#include <pxr/base/tf/staticTokens.h>
#include <pxr/imaging/hd/dependenciesSchema.h>
#include <pxr/imaging/hd/dependencySchema.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/mapContainerDataSource.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/volumeFieldBindingSchema.h>
#include <pxr/imaging/hd/volumeFieldSchema.h>

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
  (cycles_volumeFieldBindingToDependency)
);
// clang-format on

namespace {

HdDataSourceBaseHandle FieldDependency(const HdDataSourceBaseHandle &pathDs)
{
  static const HdLocatorDataSourceHandle dependedOn =
      HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(
          HdVolumeFieldSchema::GetDefaultLocator());
  static const HdLocatorDataSourceHandle affected =
      HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(
          HdVolumeFieldBindingSchema::GetDefaultLocator());

  return HdDependencySchema::Builder()
      .SetDependedOnPrimPath(HdPathDataSource::Cast(pathDs))
      .SetDependedOnDataSourceLocator(dependedOn)
      .SetAffectedDataSourceLocator(affected)
      .Build();
}

/* The forwarding scene index only recomputes a prim's dependencies when its __dependencies are
 * dirtied, so rebinding a volume to other fields has to dirty them. */
HdContainerDataSourceHandle BindingDependency()
{
  static const HdLocatorDataSourceHandle dependedOn =
      HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(
          HdVolumeFieldBindingSchema::GetDefaultLocator());
  static const HdLocatorDataSourceHandle affected =
      HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(
          HdDependenciesSchema::GetDefaultLocator());
  static const HdContainerDataSourceHandle dependency = HdRetainedContainerDataSource::New(
      _tokens->cycles_volumeFieldBindingToDependency,
      HdDependencySchema::Builder()
          .SetDependedOnDataSourceLocator(dependedOn)
          .SetAffectedDataSourceLocator(affected)
          .Build());
  return dependency;
}

class VolumePrimDataSource final : public HdContainerDataSource {
 public:
  HD_DECLARE_DATASOURCE(VolumePrimDataSource);

  TfTokenVector GetNames() override
  {
    TfTokenVector names = _input->GetNames();
    if (std::find(names.begin(), names.end(), HdDependenciesSchema::GetSchemaToken()) ==
        names.end())
    {
      names.push_back(HdDependenciesSchema::GetSchemaToken());
    }
    return names;
  }

  HdDataSourceBaseHandle Get(const TfToken &name) override
  {
    if (name != HdDependenciesSchema::GetSchemaToken()) {
      return _input->Get(name);
    }

    const HdContainerDataSourceHandle fields = HdMapContainerDataSource::New(
        FieldDependency, HdVolumeFieldBindingSchema::GetFromParent(_input).GetContainer());
    return HdOverlayContainerDataSource::New(
        HdDependenciesSchema::GetFromParent(_input).GetContainer(), fields, BindingDependency());
  }

 private:
  VolumePrimDataSource(const HdContainerDataSourceHandle &input) : _input(input) {}

  const HdContainerDataSourceHandle _input;
};

TF_DECLARE_REF_PTRS(DependencySceneIndex);

class DependencySceneIndex final : public HdSingleInputFilteringSceneIndexBase {
 public:
  static DependencySceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &input)
  {
    return TfCreateRefPtr(new DependencySceneIndex(input));
  }

  HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override
  {
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    if (prim.dataSource && prim.primType == HdPrimTypeTokens->volume) {
      prim.dataSource = VolumePrimDataSource::New(prim.dataSource);
    }
    return prim;
  }

  SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override
  {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
  }

 private:
  DependencySceneIndex(const HdSceneIndexBaseRefPtr &input)
      : HdSingleInputFilteringSceneIndexBase(input)
  {
    SetDisplayName("HdCycles: declare Cycles dependencies");
  }

  void _PrimsAdded(const HdSceneIndexBase & /*sender*/,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override
  {
    _SendPrimsAdded(entries);
  }

  void _PrimsRemoved(const HdSceneIndexBase & /*sender*/,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override
  {
    _SendPrimsRemoved(entries);
  }

  void _PrimsDirtied(const HdSceneIndexBase & /*sender*/,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override
  {
    _SendPrimsDirtied(entries);
  }
};

}  // namespace

HdSceneIndexBaseRefPtr NewDependencySceneIndex(const HdSceneIndexBaseRefPtr &inputSceneIndex)
{
  return DependencySceneIndex::New(inputSceneIndex);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
