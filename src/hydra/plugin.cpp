/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/plugin.h"
#include "hydra/dependency_scene_index.h"
#include "hydra/render_delegate.h"
#include "util/log.h"
#include "util/path.h"

#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/thisPlugin.h>
#include <pxr/base/tf/envSetting.h>
#include <pxr/imaging/hd/dependencyForwardingSceneIndex.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_ENV_SETTING(CYCLES_LOGGING, false, "Enable Cycles logging")
TF_DEFINE_ENV_SETTING(CYCLES_LOGGING_LEVEL, "warning", "Cycles logging level")

HdCyclesPlugin::HdCyclesPlugin()
{
  const PlugPluginPtr plugin = PLUG_THIS_PLUGIN;
  // Initialize Cycles paths relative to the plugin resource path
  const std::string rootPath = PXR_NS::ArchAbsPath(plugin->GetResourcePath());
  CCL_NS::path_init(std::move(rootPath));

  if (TfGetEnvSetting(CYCLES_LOGGING)) {
    CCL_NS::log_level_set(TfGetEnvSetting(CYCLES_LOGGING_LEVEL));
  }
}

HdCyclesPlugin::~HdCyclesPlugin() {}

#if PXR_VERSION < 2302
bool HdCyclesPlugin::IsSupported() const
{
  return true;
}
#else
#  if PXR_VERSION >= 2511
bool HdCyclesPlugin::IsSupported(HdRendererCreateArgs const & /*rendererCreateArgs*/,
                                 std::string * /*reasonWhyNot*/) const
{
  return true;
}
#  endif
bool HdCyclesPlugin::IsSupported(bool /*gpuEnabled*/) const
{
  return true;
}
#endif

HdRenderDelegate *HdCyclesPlugin::CreateRenderDelegate()
{
  return CreateRenderDelegate({});
}

HdRenderDelegate *HdCyclesPlugin::CreateRenderDelegate(const HdRenderSettingsMap &settingsMap)
{
  return new HD_CYCLES_NS::HdCyclesDelegate(settingsMap);
}

void HdCyclesPlugin::DeleteRenderDelegate(HdRenderDelegate *renderDelegate)
{
  delete renderDelegate;
}

// USD's type system accounts for namespace, so we'd have to register our name as
// HdCycles::HdCyclesPlugin in plugInfo.json, which isn't all that bad for JSON,
// but those colons may cause issues for any USD specific tooling. So just put our
// plugin class in the pxr namespace (which USD's type system will elide).
TF_REGISTRY_FUNCTION(TfType)
{
  HdRendererPluginRegistry::Define<PXR_NS::HdCyclesPlugin>();
}

/* Hydra leaves it to each renderer to declare and forward the dependencies between its prims.
 * Without them a field's change never reaches the volumes bound to it, and under a scene index
 * the change tracker rejects a prim dirtying another one itself. The forwarding scene index has
 * to sit downstream of the one declaring the dependencies. */
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
  static const std::string displayName = "Cycles";
  HdSceneIndexPluginRegistry &registry = HdSceneIndexPluginRegistry::GetInstance();

  registry.RegisterSceneIndexForRenderer(
      displayName,
      [](const std::string & /*renderInstanceId*/,
         const HdSceneIndexBaseRefPtr &inputScene,
         const HdContainerDataSourceHandle & /*inputArgs*/) {
        return HD_CYCLES_NS::NewDependencySceneIndex(inputScene);
      },
      nullptr,
      900,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);

  registry.RegisterSceneIndexForRenderer(
      displayName,
      [](const std::string & /*renderInstanceId*/,
         const HdSceneIndexBaseRefPtr &inputScene,
         const HdContainerDataSourceHandle & /*inputArgs*/) -> HdSceneIndexBaseRefPtr {
        return HdDependencyForwardingSceneIndex::New(inputScene);
      },
      nullptr,
      1000,
      HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

PXR_NAMESPACE_CLOSE_SCOPE
