/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/material.h"
#include "hydra/node_util.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_set>

#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Normalize a material network node name to a full SdfPath. The schema may
 * provide either a full path or a bare identifier. */
static SdfPath MaterialNodeNameToSdfPath(const TfToken &nodeName)
{
  const std::string &s = nodeName.GetString();
  if (s.empty()) {
    return SdfPath::EmptyPath();
  }
  if (s[0] == '/' && SdfPath::IsValidPathString(s)) {
    return SdfPath(s);
  }
  return SdfPath::AbsoluteRootPath().AppendChild(nodeName);
}

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(CyclesMaterialTokens,
    (cycles)
    ((cyclesSurface, "cycles:surface"))
    ((cyclesDisplacement, "cycles:displacement"))
    ((cyclesVolume, "cycles:volume"))
    (UsdPreviewSurface)
    (UsdUVTexture)
    (UsdPrimvarReader_float)
    (UsdPrimvarReader_float2)
    (UsdPrimvarReader_float3)
    (UsdPrimvarReader_float4)
    (UsdPrimvarReader_int)
    (UsdTransform2d)
    (a)
    (rgb)
    (r)
    (g)
    (b)
    (result)
    (st)
    (wrapS)
    (wrapT)
    (scale)
    (bias)
    (opacity)
    (opacityThreshold)
    (periodic)
);
// clang-format on

/* Simple class to handle remapping of USDPreviewSurface nodes and parameters to Cycles
 * equivalents. */
class UsdToCyclesMapping {
  using ParamMap = std::unordered_map<TfToken, ustring, TfToken::HashFunctor>;
  using IgnoredSet = std::unordered_set<TfToken, TfToken::HashFunctor>;

 public:
  UsdToCyclesMapping(const char *nodeType, ParamMap paramMap, IgnoredSet ignored = {})
      : _nodeType(nodeType), _paramMap(std::move(paramMap)), _ignored(std::move(ignored))
  {
  }

  /* Inputs with no Cycles counterpart, dropped without a warning. */
  bool isIgnored(const TfToken &name) const
  {
    return _ignored.count(name) != 0;
  }

  ustring nodeType() const
  {
    return _nodeType;
  }

  virtual std::string parameterName(const TfToken &name,
                                    const ShaderInput *inputConnection,
                                    VtValue * /*value*/ = nullptr) const
  {
    /* UsdNode.name -> Node.input. These all follow a simple pattern that we can just
     * remap based on the name or 'Node.input' type. */
    if (inputConnection) {
      if (name == CyclesMaterialTokens->a) {
        return "alpha";
      }
      if (name == CyclesMaterialTokens->rgb) {
        return "color";
      }
      /* TODO: Is there a better mapping than 'color'? */
      if (name == CyclesMaterialTokens->r || name == CyclesMaterialTokens->g ||
          name == CyclesMaterialTokens->b)
      {
        return "color";
      }

      if (name == CyclesMaterialTokens->result) {
        switch (inputConnection->socket_type.type) {
          case SocketType::BOOLEAN:
          case SocketType::FLOAT:
          case SocketType::INT:
          case SocketType::UINT:
            return "alpha";
          case SocketType::COLOR:
          case SocketType::VECTOR:
          case SocketType::POINT:
          case SocketType::NORMAL:
          default:
            return "color";
        }
      }
    }

    /* Simple mapping case */
    const auto it = _paramMap.find(name);
    return it != _paramMap.end() ? it->second.string() : name.GetString();
  }

 private:
  const ustring _nodeType;
  ParamMap _paramMap;
  IgnoredSet _ignored;
};

class UsdToCyclesTexture : public UsdToCyclesMapping {
 public:
  using UsdToCyclesMapping::UsdToCyclesMapping;

  std::string parameterName(const TfToken &name,
                            const ShaderInput *inputConnection,
                            VtValue *value) const override
  {
    if (value) {
      /* Remap UsdUVTexture.wrapS and UsdUVTexture.wrapT to cycles_image_texture.extension. */
      if (name == CyclesMaterialTokens->wrapS || name == CyclesMaterialTokens->wrapT) {
        const std::string valueString = VtValue::Cast<std::string>(*value).Get<std::string>();

        /* A value of 'repeat' in USD is equivalent to 'periodic' in Cycles. */
        if (valueString == "repeat") {
          *value = VtValue(CyclesMaterialTokens->periodic);
        }

        return "extension";
      }
    }

    return UsdToCyclesMapping::parameterName(name, inputConnection, value);
  }
};

namespace {

bool IsIdentityScaleBias(const float *scale, const float *bias, const int first, const int count)
{
  for (int i = first; i < first + count; i++) {
    if (scale[i] != 1.0f || bias[i] != 0.0f) {
      return false;
    }
  }
  return true;
}

/* Insert `output * scale + bias` after a texture output; float outputs are the alpha channel. */
ShaderOutput *ApplyScaleBias(ShaderGraph *graph,
                             ShaderOutput *output,
                             const float *scale,
                             const float *bias)
{
  if (output->socket_type.type == SocketType::FLOAT) {
    if (IsIdentityScaleBias(scale, bias, 3, 1)) {
      return output;
    }
    MathNode *node = graph->create_node<MathNode>();
    node->set_math_type(NODE_MATH_MULTIPLY_ADD);
    node->set_value2(scale[3]);
    node->set_value3(bias[3]);
    graph->connect(output, node->input("Value1"));
    return node->output("Value");
  }

  if (IsIdentityScaleBias(scale, bias, 0, 3)) {
    return output;
  }
  VectorMathNode *node = graph->create_node<VectorMathNode>();
  node->set_math_type(NODE_VECTOR_MATH_MULTIPLY_ADD);
  node->set_vector2(make_float3(scale[0], scale[1], scale[2]));
  node->set_vector3(make_float3(bias[0], bias[1], bias[2]));
  graph->connect(output, node->input("Vector1"));
  return node->output("Vector");
}

class UsdToCycles {
  const UsdToCyclesMapping UsdPreviewSurface = {
      "principled_bsdf",
      {
          {TfToken("diffuseColor"), ustring("base_color")},
          {TfToken("emissiveColor"), ustring("emission_color")},
          {TfToken("clearcoat"), ustring("coat_weight")},
          {TfToken("clearcoatColor"), ustring("coat_tint")},
          {TfToken("clearcoatRoughness"), ustring("coat_roughness")},
          {TfToken("opacity"), ustring("alpha")},
      },
      /* Principled has no F0 input to drive a specular workflow with, a path tracer computes
       * occlusion itself, and a displacement input would need tessellation to displace. */
      {TfToken("useSpecularWorkflow"),
       TfToken("specularColor"),
       TfToken("occlusion"),
       TfToken("displacement")}};
  const UsdToCyclesTexture UsdUVTexture = {
      "image_texture",
      {
          {CyclesMaterialTokens->st, ustring("vector")},
          {CyclesMaterialTokens->wrapS, ustring("extension")},
          {CyclesMaterialTokens->wrapT, ustring("extension")},
          {TfToken("file"), ustring("filename")},
          {TfToken("sourceColorSpace"), ustring("colorspace")},
      },
      {TfToken("fallback")}};
  const UsdToCyclesMapping UsdPrimvarReader = {
      "attribute", {{TfToken("varname"), ustring("attribute")}}, {TfToken("fallback")}};

 public:
  const UsdToCyclesMapping *findUsd(const TfToken &usdNodeType)
  {
    if (usdNodeType == CyclesMaterialTokens->UsdPreviewSurface) {
      return &UsdPreviewSurface;
    }
    if (usdNodeType == CyclesMaterialTokens->UsdUVTexture) {
      return &UsdUVTexture;
    }
    if (usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float2 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float3 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float4 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_int)
    {
      return &UsdPrimvarReader;
    }

    return nullptr;
  }
  const UsdToCyclesMapping *findCycles(const ustring & /*cyclesNodeType*/)
  {
    return nullptr;
  }
};
TfStaticData<UsdToCycles> sUsdToCyles;

}  // namespace

HdCyclesMaterial::HdCyclesMaterial(const SdfPath &sprimId) : HdMaterial(sprimId) {}

HdCyclesMaterial::~HdCyclesMaterial() = default;

HdDirtyBits HdCyclesMaterial::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyResource | DirtyBits::DirtyParams;
}

void HdCyclesMaterial::Sync(HdSceneDelegate *sceneDelegate,
                            HdRenderParam *renderParam,
                            HdDirtyBits *dirtyBits)
{
  if (*dirtyBits == DirtyBits::Clean) {
    return;
  }

  Initialize(renderParam);

  const SceneLock lock(renderParam);

  const bool dirtyParams = (*dirtyBits & DirtyBits::DirtyParams);
  const bool dirtyResource = (*dirtyBits & DirtyBits::DirtyResource);

  const SdfPath &id = GetId();

  if (dirtyResource || dirtyParams) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
    const HdContainerDataSourceHandle &primDs = prim.dataSource;

    HdMaterialSchema matSchema = HdMaterialSchema::GetFromParent(primDs);
    /* Prefer cycles network if it exists, otherwise use universal network. */
    HdMaterialNetworkSchema network = matSchema.GetMaterialNetwork(CyclesMaterialTokens->cycles);
    if (!network) {
      network = matSchema.GetMaterialNetwork();
    }

    if (network) {
      if (!_nodes.empty() && !dirtyResource) {
        if (UpdateParameters(network)) {
          PopulateShaderGraph(network);
          _shader->tag_modified();
        }
        else {
          _shader->tag_modified();
        }
      }
      else {
        PopulateShaderGraph(network);
        _shader->tag_modified();
      }
    }
    else {
      TF_RUNTIME_ERROR("Could not get a material network for %s.", id.GetText());
      PopulateFallbackShaderGraph();
    }
  }

  if (_shader->is_modified()) {
    _shader->tag_update(lock.scene);
  }

  *dirtyBits = DirtyBits::Clean;
}

bool HdCyclesMaterial::UpdateParameters(NodeDesc &nodeDesc,
                                        HdMaterialNodeParameterContainerSchema params,
                                        const SdfPath &nodePath)
{
  bool rebuild = false;
  const bool isTexture = nodeDesc.mapping && nodeDesc.mapping->nodeType() == "image_texture";
  const bool isSurface = nodeDesc.mapping && nodeDesc.mapping->nodeType() == "principled_bsdf";

  /* Read first: opacity may be visited before the threshold that decides how it is applied. */
  if (isSurface) {
    float threshold = 0.0f;
    if (auto thresholdDs = params.Get(CyclesMaterialTokens->opacityThreshold).GetValue()) {
      VtValue v = VtValue::Cast<float>(thresholdDs->GetValue(0.0f));
      if (v.IsHolding<float>()) {
        threshold = v.UncheckedGet<float>();
      }
    }
    rebuild |= nodeDesc.opacityThreshold != threshold;
    nodeDesc.opacityThreshold = threshold;
  }

  for (const TfToken &paramName : params.GetNames()) {
    auto valueDs = params.Get(paramName).GetValue();
    if (!valueDs) {
      continue;
    }
    VtValue value = valueDs->GetValue(0.0f);

    if (isSurface && paramName == CyclesMaterialTokens->opacityThreshold) {
      continue;
    }

    /* A cutout is fully opaque where opacity reaches the threshold, and absent elsewhere. */
    if (isSurface && paramName == CyclesMaterialTokens->opacity &&
        nodeDesc.opacityThreshold > 0.0f)
    {
      value = VtValue::Cast<float>(value);
      if (value.IsHolding<float>()) {
        value = VtValue(value.UncheckedGet<float>() >= nodeDesc.opacityThreshold ? 1.0f : 0.0f);
      }
    }

    if (isTexture &&
        (paramName == CyclesMaterialTokens->scale || paramName == CyclesMaterialTokens->bias))
    {
      value = VtValue::Cast<GfVec4f>(value);
      if (value.IsHolding<GfVec4f>()) {
        float *dst = paramName == CyclesMaterialTokens->scale ? nodeDesc.scale : nodeDesc.bias;
        const GfVec4f &v = value.UncheckedGet<GfVec4f>();
        for (int i = 0; i < 4; i++) {
          rebuild |= dst[i] != v[i];
          dst[i] = v[i];
        }
      }
      continue;
    }

    /* See if the parameter name is in USDPreviewSurface terms, and needs to be converted .*/
    const UsdToCyclesMapping *inputMapping = nodeDesc.mapping;
    const std::string inputName = inputMapping ?
                                      inputMapping->parameterName(paramName, nullptr, &value) :
                                      paramName.GetString();

    /* Find the input to write the parameter value to. */
    const SocketType *input = nullptr;
    for (const SocketType &socket : nodeDesc.node->type->inputs) {
      if (string_iequals(socket.name.string(), inputName) || socket.ui_name == inputName) {
        input = &socket;
        break;
      }
    }

    if (!input) {
      if (inputMapping && inputMapping->isIgnored(paramName)) {
        continue;
      }
      TF_WARN("Could not find parameter '%s' on node '%s' ('%s')",
              paramName.GetText(),
              nodePath.GetText(),
              nodeDesc.node->name.c_str());
      continue;
    }

    SetNodeValue(nodeDesc.node, *input, value);
  }

  return rebuild;
}

bool HdCyclesMaterial::UpdateParameters(HdMaterialNetworkSchema network)
{
  bool rebuild = false;
  HdMaterialNodeContainerSchema nodes = network.GetNodes();
  for (const TfToken &nodeName : nodes.GetNames()) {
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);

    const auto nodeIt = _nodes.find(nodePath);
    if (nodeIt == _nodes.end()) {
      TF_RUNTIME_ERROR("Could not update parameters on missing node '%s'", nodePath.GetText());
      continue;
    }

    rebuild |= UpdateParameters(nodeIt->second, nodes.Get(nodeName).GetParameters(), nodePath);
  }

  return rebuild;
}

void HdCyclesMaterial::UpdateConnections(NodeDesc &nodeDesc,
                                         HdMaterialNodeSchema nodeSchema,
                                         const SdfPath &nodePath,
                                         ShaderGraph *shaderGraph)
{
  HdMaterialConnectionVectorContainerSchema conns = nodeSchema.GetInputConnections();
  for (const TfToken &dstSocketName : conns.GetNames()) {
    HdMaterialConnectionVectorSchema connVec = conns.Get(dstSocketName);
    const size_t count = connVec.GetNumElements();
    if (count == 0) {
      continue;
    }

    const UsdToCyclesMapping *inputMapping = nodeDesc.mapping;
    const std::string inputName = inputMapping ?
                                      inputMapping->parameterName(dstSocketName, nullptr) :
                                      dstSocketName.GetString();

    /* Find the input to connect to on the passed in node. */
    ShaderInput *input = nullptr;
    for (ShaderInput *in : nodeDesc.node->inputs) {
      if (string_iequals(in->socket_type.name.string(), inputName)) {
        input = in;
        break;
      }
    }

    if (!input) {
      if (inputMapping && inputMapping->isIgnored(dstSocketName)) {
        continue;
      }
      TF_WARN("Ignoring connection on '%s.%s', input '%s' was not found",
              nodePath.GetText(),
              dstSocketName.GetText(),
              dstSocketName.GetText());
      continue;
    }

    /* USD allows N connections per input (MaterialX <switch>, <combine>, struct
     * inputs etc). Cycles inputs are single-connection, and the right lowering
     * depends on the node type, so just take the first and warn. */
    if (count > 1) {
      TF_WARN(
          "Ignoring multiple connections to '%s.%s'", nodePath.GetText(), dstSocketName.GetText());
    }

    HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
    const SdfPath upstreamNodePath =
        connSchema.GetUpstreamNodePath() ?
            MaterialNodeNameToSdfPath(connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
            SdfPath();
    const TfToken upstreamOutputName = connSchema.GetUpstreamNodeOutputName() ?
                                           connSchema.GetUpstreamNodeOutputName()->GetTypedValue(
                                               0.0f) :
                                           TfToken();

    const auto srcNodeIt = _nodes.find(upstreamNodePath);
    if (srcNodeIt == _nodes.end()) {
      TF_WARN("Ignoring connection from '%s.%s' to '%s.%s', node '%s' was not found",
              upstreamNodePath.GetText(),
              upstreamOutputName.GetText(),
              nodePath.GetText(),
              dstSocketName.GetText(),
              upstreamNodePath.GetText());
      continue;
    }

    const UsdToCyclesMapping *outputMapping = srcNodeIt->second.mapping;
    const std::string outputName = outputMapping ?
                                       outputMapping->parameterName(upstreamOutputName, input) :
                                       upstreamOutputName.GetString();

    ShaderOutput *output = nullptr;
    for (ShaderOutput *out : srcNodeIt->second.node->outputs) {
      if (string_iequals(out->socket_type.name.string(), outputName)) {
        output = out;
        break;
      }
    }

    if (!output) {
      TF_WARN("Ignoring connection from '%s.%s' to '%s.%s', output '%s' was not found",
              upstreamNodePath.GetText(),
              upstreamOutputName.GetText(),
              nodePath.GetText(),
              dstSocketName.GetText(),
              upstreamOutputName.GetText());
      continue;
    }

    ShaderOutput *source = output;
    float scale[4];
    float bias[4];
    std::copy(std::begin(srcNodeIt->second.scale), std::end(srcNodeIt->second.scale), scale);
    std::copy(std::begin(srcNodeIt->second.bias), std::end(srcNodeIt->second.bias), bias);

    /* UsdPreviewSurface.normal is a tangent-space vector in -1..1, while Cycles' normal map node
     * decodes a 0..1 colour itself. */
    const bool isNormalMap = outputMapping && outputMapping->nodeType() == "image_texture" &&
                             input->socket_type.type == SocketType::NORMAL &&
                             output->socket_type.type != SocketType::FLOAT;
    if (isNormalMap) {
      for (int i = 0; i < 3; i++) {
        scale[i] *= 0.5f;
        bias[i] = bias[i] * 0.5f + 0.5f;
      }
    }

    if (outputMapping && outputMapping->nodeType() == "image_texture") {
      source = ApplyScaleBias(shaderGraph, source, scale, bias);
    }

    if (isNormalMap) {
      NormalMapNode *normalMap = shaderGraph->create_node<NormalMapNode>();
      shaderGraph->connect(source, normalMap->input("Color"));
      source = normalMap->output("Normal");
    }

    if (nodeDesc.opacityThreshold > 0.0f && input->socket_type.name == ustring("alpha")) {
      /* The largest float below the threshold, so that a greater-than test keeps opacity equal to
       * it. */
      MathNode *cutout = shaderGraph->create_node<MathNode>();
      cutout->set_math_type(NODE_MATH_GREATER_THAN);
      cutout->set_value2(std::nextafter(nodeDesc.opacityThreshold, 0.0f));
      shaderGraph->connect(source, cutout->input("Value1"));
      source = cutout->output("Value");
    }

    shaderGraph->connect(source, input);
  }
}

static void CreateInstanceIdAov(ShaderGraph *graph)
{
  const ustring instanceId(HdAovTokens->instanceId.GetString());

  OutputAOVNode *aovNode = graph->create_node<OutputAOVNode>();
  aovNode->set_name(instanceId);

  AttributeNode *instanceIdNode = graph->create_node<AttributeNode>();
  instanceIdNode->set_attribute(instanceId);

  graph->connect(instanceIdNode->output("Fac"), aovNode->input("Value"));
}

void HdCyclesMaterial::PopulateShaderGraph(HdMaterialNetworkSchema network)
{
  _nodes.clear();

  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

  HdMaterialNodeContainerSchema nodes = network.GetNodes();

  /* Iterate all the nodes first and build a complete but unconnected graph with parameters set. */
  for (const TfToken &nodeName : nodes.GetNames()) {
    HdMaterialNodeSchema nodeSchema = nodes.Get(nodeName);
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);

    NodeDesc nodeDesc = {};

    const auto nodeIt = _nodes.find(nodePath);
    /* Create new node only if it does not exist yet. */
    if (nodeIt != _nodes.end()) {
      nodeDesc = nodeIt->second;
    }
    else {
      /* E.g. cycles_principled_bsdf or UsdPreviewSurface. */
      const TfToken nodeTypeIdToken = nodeSchema.GetNodeIdentifier() ?
                                          nodeSchema.GetNodeIdentifier()->GetTypedValue(0.0f) :
                                          TfToken();
      const std::string &nodeTypeId = nodeTypeIdToken.GetString();

      ustring cyclesType(nodeTypeId);
      if (nodeTypeId.starts_with("cycles_") || nodeTypeId.starts_with("cycles:")) {
        /* Native Cycles note embedded in USDShade. */
        cyclesType = nodeTypeId.substr(strlen("cycles_"));
        nodeDesc.mapping = sUsdToCyles->findCycles(cyclesType);
      }
      else {
        /* Check if any remapping is needed (e.g. for USDPreviewSurface to Cycles nodes). */
        nodeDesc.mapping = sUsdToCyles->findUsd(nodeTypeIdToken);
        if (nodeDesc.mapping) {
          cyclesType = nodeDesc.mapping->nodeType();
        }
      }

      /* If it's a native Cycles' node-type, just do the lookup now. */
      if (const NodeType *nodeType = NodeType::find(cyclesType)) {
        nodeDesc.node = graph->create_node(nodeType);
        if (nodeDesc.mapping && cyclesType == "principled_bsdf") {
          /* Cycles emits white at strength 0; UsdPreviewSurface emits its emissiveColor, black
           * unless authored. */
          PrincipledBsdfNode *surface = static_cast<PrincipledBsdfNode *>(nodeDesc.node);
          surface->set_emission_color(zero_float3());
          surface->set_emission_strength(1.0f);
        }
        _nodes.emplace(nodePath, nodeDesc);
      }
      else {
        TF_RUNTIME_ERROR("Could not create node '%s'", nodePath.GetText());
        continue;
      }
    }

    UpdateParameters(_nodes.at(nodePath), nodeSchema.GetParameters(), nodePath);
  }

  /* Now that all nodes have been constructed, iterate the network again and build up any
   * connections between nodes. */
  for (const TfToken &nodeName : nodes.GetNames()) {
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);

    const auto nodeIt = _nodes.find(nodePath);
    if (nodeIt == _nodes.end()) {
      TF_RUNTIME_ERROR("Could not find node '%s' to connect", nodePath.GetText());
      continue;
    }

    UpdateConnections(nodeIt->second, nodes.Get(nodeName), nodePath, graph.get());
  }

  /* Finally connect the terminals to the graph output (Surface, Volume, Displacement). */
  HdMaterialConnectionContainerSchema terminals = network.GetTerminals();
  for (const TfToken &terminalName : terminals.GetNames()) {
    HdMaterialConnectionSchema termSchema = terminals.Get(terminalName);
    const SdfPath upstreamNodePath =
        termSchema.GetUpstreamNodePath() ?
            MaterialNodeNameToSdfPath(termSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
            SdfPath();
    const TfToken upstreamOutputName = termSchema.GetUpstreamNodeOutputName() ?
                                           termSchema.GetUpstreamNodeOutputName()->GetTypedValue(
                                               0.0f) :
                                           TfToken();

    const auto nodeIt = _nodes.find(upstreamNodePath);
    if (nodeIt == _nodes.end()) {
      TF_RUNTIME_ERROR("Could not find terminal node '%s'", upstreamNodePath.GetText());
      continue;
    }

    ShaderNode *const node = nodeIt->second.node;

    const char *inputName = nullptr;
    const char *outputName = nullptr;
    if (terminalName == HdMaterialTerminalTokens->surface ||
        terminalName == CyclesMaterialTokens->cyclesSurface)
    {
      inputName = "Surface";
      /* Find default output name based on the node if none is provided. */
      if (node->type->name == "add_closure" || node->type->name == "mix_closure") {
        outputName = "Closure";
      }
      else if (node->type->name == "emission") {
        outputName = "Emission";
      }
      else {
        outputName = "BSDF";
      }
    }
    else if (terminalName == HdMaterialTerminalTokens->displacement ||
             terminalName == CyclesMaterialTokens->cyclesDisplacement)
    {
      inputName = outputName = "Displacement";
    }
    else if (terminalName == HdMaterialTerminalTokens->volume ||
             terminalName == CyclesMaterialTokens->cyclesVolume)
    {
      inputName = outputName = "Volume";
    }

    /* For native Cycles nodes we use the upstream output name as is, for
     * mapping from e.g. UsdPreviewSurface we need to use the default output
     * name that is known to exist. */
    if (!upstreamOutputName.IsEmpty() && nodeIt->second.mapping == nullptr) {
      outputName = upstreamOutputName.GetText();
    }

    ShaderInput *const input = inputName ? graph->output()->input(inputName) : nullptr;
    if (!input) {
      TF_RUNTIME_ERROR("Could not find terminal input '%s.%s'",
                       upstreamNodePath.GetText(),
                       inputName ? inputName : "<null>");
      continue;
    }

    ShaderOutput *const output = outputName ? node->output(outputName) : nullptr;
    if (!output) {
      TF_RUNTIME_ERROR("Could not find terminal output '%s.%s'",
                       upstreamNodePath.GetText(),
                       outputName ? outputName : "<null>");
      continue;
    }

    graph->connect(output, input);
  }

  CreateInstanceIdAov(graph.get());

  _shader->set_graph(std::move(graph));
}

void HdCyclesMaterial::PopulateFallbackShaderGraph()
{
  if (_shader->graph && _nodes.empty()) {
    return;
  }

  _nodes.clear();

  /* Scene code dereferences the graph of every shader, starting with Shader::tag_update, so a
   * material with no network Cycles can read still needs one. This one looks like the default
   * surface, so its meshes render as if they had no material. */
  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

  ObjectInfoNode *objectNode = graph->create_node<ObjectInfoNode>();
  DiffuseBsdfNode *diffuseNode = graph->create_node<DiffuseBsdfNode>();

  graph->connect(objectNode->output("Color"), diffuseNode->input("Color"));
  graph->connect(diffuseNode->output("BSDF"), graph->output()->input("Surface"));

  CreateInstanceIdAov(graph.get());

  _shader->set_graph(std::move(graph));
  _shader->tag_modified();
}

void HdCyclesMaterial::Finalize(HdRenderParam *renderParam)
{
  if (!_shader) {
    return;
  }

  const SceneLock lock(renderParam);
  const bool keep_nodes = static_cast<const HdCyclesSession *>(renderParam)->keep_nodes;

  _nodes.clear();

  if (!keep_nodes) {
    lock.scene->delete_node(_shader);
  }
  _shader = nullptr;
}

void HdCyclesMaterial::Initialize(HdRenderParam *renderParam)
{
  if (_shader) {
    return;
  }

  const SceneLock lock(renderParam);

  _shader = lock.scene->create_node<Shader>();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
