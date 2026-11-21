/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/session.h"
#include "hydra/material.h"
#include "scene/object.h"
#include "scene/pass.h"
#include "scene/shader.h"
// Have to include shader.h before background.h so that 'set_shader' uses the correct 'set'
// overload taking a 'Node *', rather than the one taking a 'bool'
#include "scene/background.h"
#include "scene/light.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "session/session.h"

#include <pxr/imaging/hd/renderBuffer.h>

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

const std::string kCyclesPrefix = "cycles:";
const std::string kLightgroupPrefix = "cycles:lightgroup:";
const std::string kPrimvarsPrefix = "primvars:";

/* combined, depth, normal and object_id are the standard color, depth, normal and primId AOVs.
 * Of the passes not offered, Hydra syncs no data for motion, material_id and shadow_catcher, the
 * denoising, sample-count and volume-guiding passes are Cycles' own, and cryptomatte needs a
 * manifest. */
const std::pair<const char *, PassType> kCyclesPasses[] = {
    {"emission", PASS_EMISSION},
    {"background", PASS_BACKGROUND},
    {"ao", PASS_AO},
    {"diffuse", PASS_DIFFUSE},
    {"diffuse_direct", PASS_DIFFUSE_DIRECT},
    {"diffuse_indirect", PASS_DIFFUSE_INDIRECT},
    {"diffuse_color", PASS_DIFFUSE_COLOR},
    {"glossy", PASS_GLOSSY},
    {"glossy_direct", PASS_GLOSSY_DIRECT},
    {"glossy_indirect", PASS_GLOSSY_INDIRECT},
    {"glossy_color", PASS_GLOSSY_COLOR},
    {"transmission", PASS_TRANSMISSION},
    {"transmission_direct", PASS_TRANSMISSION_DIRECT},
    {"transmission_indirect", PASS_TRANSMISSION_INDIRECT},
    {"transmission_color", PASS_TRANSMISSION_COLOR},
    {"volume", PASS_VOLUME},
    {"volume_direct", PASS_VOLUME_DIRECT},
    {"volume_indirect", PASS_VOLUME_INDIRECT},
    {"position", PASS_POSITION},
    {"roughness", PASS_ROUGHNESS},
    {"uv", PASS_UV},
    {"mist", PASS_MIST},
};

bool StartsWith(const std::string &name, const std::string &prefix)
{
  return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
}

unique_ptr<ShaderGraph> DefaultSurfaceGraph(const std::vector<ustring> &primvarAovs)
{
  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

  ObjectInfoNode *objectNode = graph->create_node<ObjectInfoNode>();

  DiffuseBsdfNode *diffuseNode = graph->create_node<DiffuseBsdfNode>();

  graph->connect(objectNode->output("Color"), diffuseNode->input("Color"));
  graph->connect(diffuseNode->output("BSDF"), graph->output()->input("Surface"));

  HdCyclesCreateAovOutputs(graph.get(), primvarAovs);

  return graph;
}

}  // namespace

HdCyclesAovPass HdCyclesPassForAov(const TfToken &aovName)
{
  HdCyclesAovPass pass;
  const std::string &name = aovName.GetString();

  if (aovName == HdAovTokens->color) {
    pass.type = PASS_COMBINED;
  }
  else if (aovName == HdAovTokens->depth) {
    pass.type = PASS_DEPTH;
  }
  else if (aovName == HdAovTokens->normal) {
    pass.type = PASS_NORMAL;
  }
  else if (aovName == HdAovTokens->primId) {
    pass.type = PASS_OBJECT_ID;
  }
  else if (aovName == HdAovTokens->instanceId) {
    pass.type = PASS_AOV_VALUE;
  }
  else if (StartsWith(name, kLightgroupPrefix)) {
    pass.type = PASS_COMBINED;
    pass.lightgroup = ustring(name.substr(kLightgroupPrefix.size()));
  }
  else if (StartsWith(name, kCyclesPrefix)) {
    const std::string passName = name.substr(kCyclesPrefix.size());
    for (const auto &entry : kCyclesPasses) {
      if (passName == entry.first) {
        pass.type = entry.second;
        break;
      }
    }
  }
  else if (StartsWith(name, kPrimvarsPrefix)) {
    pass.type = PASS_AOV_COLOR;
    pass.primvar = ustring(name.substr(kPrimvarsPrefix.size()));
  }

  if (pass.type != PASS_NONE) {
    pass.channels = Pass::get_info(pass.type, PassMode::DENOISED, false, !pass.lightgroup.empty())
                        .num_components;
  }
  // A primvar renders to Cycles' four-component AOV colour, of which only the colour is filled
  if (!pass.primvar.empty()) {
    pass.channels = 3;
  }

  return pass;
}

const TfTokenVector &HdCyclesAovNames()
{
  static const TfTokenVector names = [] {
    TfTokenVector result = {HdAovTokens->color,
                            HdAovTokens->depth,
                            HdAovTokens->normal,
                            HdAovTokens->primId,
                            HdAovTokens->instanceId};
    for (const auto &entry : kCyclesPasses) {
      result.emplace_back(kCyclesPrefix + entry.first);
    }
    return result;
  }();

  return names;
}

SceneLock::SceneLock(const HdRenderParam *renderParam)
    : scene(static_cast<const HdCyclesSession *>(renderParam)->session->scene.get()),
      sceneLock(scene->mutex)
{
}

SceneLock::~SceneLock() = default;

HdCyclesSession::HdCyclesSession(Session *session_, const bool keep_nodes)
    : session(session_), keep_nodes(keep_nodes), _ownCyclesSession(false)
{
}

HdCyclesSession::HdCyclesSession(const SessionParams &params)
    : session(new Session(params, SceneParams())), keep_nodes(false), _ownCyclesSession(true)
{
  Scene *const scene = session->scene.get();

  // Create background with ambient light
  {
    unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

    BackgroundNode *bgNode = graph->create_node<BackgroundNode>();
    bgNode->set_color(one_float3());

    graph->connect(bgNode->output("Background"), graph->output()->input("Surface"));

    scene->default_background->set_graph(std::move(graph));
    scene->default_background->tag_update(scene);
  }

  scene->default_surface->set_graph(DefaultSurfaceGraph(_primvarAovs));
  scene->default_surface->tag_update(scene);
}

HdCyclesSession::~HdCyclesSession()
{
  if (_ownCyclesSession) {
    delete session;
  }
}

void HdCyclesSession::UpdateScene()
{
  Scene *const scene = session->scene.get();

  // Update background depending on presence of a background light
  if (scene->light_manager->need_update()) {
    Light *background_light = nullptr;
    ustring background_lightgroup;
    bool have_lights = false;
    for (Object *object : scene->objects) {
      if (!object->get_geometry()->is_light()) {
        continue;
      }

      have_lights = true;

      Light *light = static_cast<Light *>(object->get_geometry());
      if (light->is_background_light()) {
        background_light = light;
        background_lightgroup = object->get_lightgroup();
        break;
      }
    }

    scene->background->set_lightgroup(_backgroundLightgroup.value_or(background_lightgroup));

    if (!background_light) {
      scene->background->set_shader(scene->default_background);
      _defaultBackgroundTransparent = true;

      /* Set background color depending to non-zero value if there are no
       * lights in the scene, to match behavior of other renderers. */
      for (ShaderNode *node : scene->default_background->graph->nodes) {
        if (node->is_a(BackgroundNode::get_node_type())) {
          BackgroundNode *bgNode = static_cast<BackgroundNode *>(node);
          bgNode->set_color((have_lights) ? zero_float3() : make_float3(0.5f));
        }
      }
    }
    else {
      scene->background->set_shader(background_light->get_shader());
      _defaultBackgroundTransparent = false;
    }

    scene->background->set_transparent(
        _backgroundTransparent.value_or(_defaultBackgroundTransparent));
    scene->background->tag_update(scene);
  }
}

void HdCyclesSession::SetBackgroundTransparent(const std::optional<bool> transparent)
{
  _backgroundTransparent = transparent;
  session->scene->background->set_transparent(
      _backgroundTransparent.value_or(_defaultBackgroundTransparent));
}

void HdCyclesSession::SetBackgroundLightgroup(const std::optional<ustring> lightgroup)
{
  Scene *const scene = session->scene.get();
  if (lightgroup) {
    scene->background->set_lightgroup(*lightgroup);
  }
  else if (_backgroundLightgroup) {
    // The dome light's group comes back when the light manager next updates
    scene->light_manager->tag_update(scene, LightManager::LIGHT_MODIFIED);
  }
  _backgroundLightgroup = lightgroup;
}

bool HdCyclesSession::SyncAovBindings(const HdRenderPassAovBindingVector &aovBindings)
{
  Scene *const scene = session->scene.get();

  // Delete all existing passes
  const vector<Pass *> &scene_passes = scene->passes;
  scene->delete_nodes(set<Pass *>(scene_passes.begin(), scene_passes.end()));

  // Update passes with requested AOV bindings
  {
    const thread_scoped_lock lock(_aovMutex);
    _aovBindings = aovBindings;
  }

  std::vector<ustring> primvarAovs;
  bool uvPass = false;
  for (const HdRenderPassAovBinding &aovBinding : aovBindings) {
    const HdCyclesAovPass aovPass = HdCyclesPassForAov(aovBinding.aovName);
    if (aovPass.type == PASS_NONE) {
      TF_WARN("Unknown pass %s", aovBinding.aovName.GetText());
      continue;
    }

    PassType type = aovPass.type;
    if (!aovPass.primvar.empty()) {
      if (std::find(primvarAovs.begin(), primvarAovs.end(), aovPass.primvar) == primvarAovs.end())
      {
        primvarAovs.push_back(aovPass.primvar);
      }
      // A one-channel buffer takes the value AOV, which has no colour to fill
      if (aovBinding.renderBuffer &&
          HdGetComponentCount(aovBinding.renderBuffer->GetFormat()) == 1)
      {
        type = PASS_AOV_VALUE;
      }
    }
    uvPass |= type == PASS_UV;

    Pass *pass = scene->create_node<Pass>();
    pass->set_type(type);
    pass->set_mode(PassMode::DENOISED);
    pass->set_name(ustring(aovBinding.aovName.GetString()));
    pass->set_lightgroup(aovPass.lightgroup);
  }

  const bool changed = primvarAovs != _primvarAovs || uvPass != _uvPass;
  _primvarAovs = std::move(primvarAovs);
  _uvPass = uvPass;

  if (changed && _ownCyclesSession) {
    scene->default_surface->set_graph(DefaultSurfaceGraph(_primvarAovs));
    scene->default_surface->tag_update(scene);
  }

  return changed;
}

bool HdCyclesSession::AovsNeedPrimvar(const ustring name, const AttributeStandard std) const
{
  return (std == ATTR_STD_UV && _uvPass) ||
         std::find(_primvarAovs.begin(), _primvarAovs.end(), name) != _primvarAovs.end();
}

void HdCyclesSession::RemoveAovBinding(HdRenderBuffer *renderBuffer)
{
  const thread_scoped_lock lock(_aovMutex);
  for (HdRenderPassAovBinding &aovBinding : _aovBindings) {
    if (renderBuffer == aovBinding.renderBuffer) {
      aovBinding.renderBuffer = nullptr;
      break;
    }
  }

  if (renderBuffer == _displayAovBinding.renderBuffer) {
    _displayAovBinding.renderBuffer = nullptr;
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
