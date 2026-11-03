#include "pch.h"
#include "materialgraph.h"
#include <json/json.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace
{
    using Type = MaterialGraphType;
    using Kind = MaterialNodeKind;
    constexpr size_t MaxNodes = 2048;
    constexpr size_t MaxLinks = 8192;
    constexpr uint64_t MaxIdentity = (uint64_t{1} << 53) - 1;
    const std::vector<MaterialNodeDefinition> NodeDefinitions = {
        {Kind::Constant, "Constant", Type::Scalar, {}},
        {Kind::ScalarParameter, "Scalar Parameter", Type::Scalar, {}},
        {Kind::VectorParameter, "Vector Parameter", Type::Vector3, {}},
        {Kind::ColorParameter, "Color Parameter", Type::Vector4, {}},
        {Kind::TextureSample, "Texture Sample", Type::Vector4, {{"UV", Type::Vector2}}},
        {Kind::Add, "Add", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::Subtract, "Subtract", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::Multiply,
         "Multiply",
         Type::Numeric,
         {{"A", Type::Numeric, {1, 1, 1, 1}}, {"B", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Divide, "Divide", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Lerp,
         "Lerp",
         Type::Numeric,
         {{"A", Type::Numeric}, {"B", Type::Numeric, {1, 1, 1, 1}}, {"Alpha", Type::Scalar, {0.5f}}}},
        {Kind::Clamp,
         "Clamp",
         Type::Numeric,
         {{"Value", Type::Numeric}, {"Min", Type::Numeric}, {"Max", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Smoothstep,
         "Smoothstep",
         Type::Numeric,
         {{"Min", Type::Numeric}, {"Max", Type::Numeric, {1, 1, 1, 1}}, {"Value", Type::Numeric}}},
        {Kind::Normalize, "Normalize", Type::Vector3, {{"Vector", Type::Vector3, {0, 0, 1}}}},
        {Kind::Dot, "Dot Product", Type::Scalar, {{"A", Type::Vector3, {0, 0, 1}}, {"B", Type::Vector3, {0, 0, 1}}}},
        {Kind::Fresnel, "Fresnel", Type::Scalar, {{"Exponent", Type::Scalar, {5}}}},
        {Kind::UV, "UV", Type::Vector2, {}},
        {Kind::WorldPosition, "World Position", Type::Vector3, {}},
        {Kind::WorldNormal, "World Normal", Type::Vector3, {}},
        {Kind::CameraVector, "Camera Vector", Type::Vector3, {}},
        {Kind::Time, "Time", Type::Scalar, {}},
        {Kind::Output,
         "Material Output",
         Type::Numeric,
         {{"Base Color", Type::Vector4, {1, 1, 1, 1}},
          {"Metallic", Type::Scalar},
          {"Roughness", Type::Scalar, {0.5f}},
          {"Normal", Type::Vector3, {0, 0, 1}},
          {"Emissive", Type::Vector3},
          {"Opacity", Type::Scalar, {1}},
          {"Opacity Mask", Type::Scalar, {1}},
          {"AO", Type::Scalar, {1}},
          // ---- エンジンのマテリアルパラメーター（2026-11 追加。未接続ならマテリアルアセットの値を保持する）
          {"Height", Type::Scalar, {1}},
          {"Normal Intensity", Type::Scalar, {1}},
          {"Parallax Depth", Type::Scalar, {0.05f}},
          {"Parallax Steps", Type::Scalar, {32}},
          {"IOR", Type::Scalar, {1.5f}},
          {"Transmission", Type::Scalar},
          {"Transmission Roughness", Type::Scalar, {0.02f}},
          {"Thickness", Type::Scalar, {0.1f}},
          {"Absorption", Type::Vector3},
          {"Shading Model Strength", Type::Scalar, {1}}}},
        {Kind::Group, "Comment Group", Type::Numeric, {}},
        {Kind::Constant2Vector, "Constant2Vector", Type::Vector2, {}},
        {Kind::Constant3Vector, "Constant3Vector", Type::Vector3, {}},
        {Kind::Constant4Vector, "Constant4Vector", Type::Vector4, {}},
        {Kind::TextureCoordinate, "TexCoord", Type::Vector2, {}},
        {Kind::Panner,
         "Panner",
         Type::Vector2,
         {{"Coordinate", Type::Vector2}, {"Time", Type::Scalar}, {"Speed", Type::Vector2, {0.1f, 0.0f}}}},
        {Kind::Rotator,
         "Rotator",
         Type::Vector2,
         {{"Coordinate", Type::Vector2},
          {"Time", Type::Scalar},
          {"Speed", Type::Scalar, {0.25f}},
          {"Center", Type::Vector2, {0.5f, 0.5f}}}},
        {Kind::Abs, "Abs", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Ceil, "Ceil", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Floor, "Floor", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Frac, "Frac", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Round, "Round", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Truncate, "Truncate", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Sign, "Sign", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::OneMinus, "OneMinus", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Saturate, "Saturate", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::SquareRoot, "SquareRoot", Type::Numeric, {{"Input", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Sine, "Sine", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Cosine, "Cosine", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Tangent, "Tangent", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Arcsine, "Arcsine", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Arccosine, "Arccosine", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Arctangent, "Arctangent", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Exponential, "Exponential", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Logarithm2, "Logarithm2", Type::Numeric, {{"Input", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::DDX, "DDX", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::DDY, "DDY", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::Power, "Power", Type::Numeric, {{"Base", Type::Numeric, {1, 1, 1, 1}}, {"Exp", Type::Scalar, {2}}}},
        {Kind::Min, "Min", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::Max, "Max", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::Fmod, "Fmod", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::Step, "Step", Type::Numeric, {{"Y", Type::Numeric, {0.5f, 0.5f, 0.5f, 0.5f}}, {"X", Type::Numeric}}},
        {Kind::Arctangent2, "Arctangent2", Type::Numeric, {{"Y", Type::Numeric}, {"X", Type::Numeric, {1, 1, 1, 1}}}},
        {Kind::ConstantBiasScale,
         "ConstantBiasScale",
         Type::Numeric,
         {{"Input", Type::Numeric}, {"Bias", Type::Scalar, {1}}, {"Scale", Type::Scalar, {0.5f}}}},
        {Kind::CrossProduct,
         "Cross Product",
         Type::Vector3,
         {{"A", Type::Vector3, {1, 0, 0}}, {"B", Type::Vector3, {0, 1, 0}}}},
        {Kind::Distance, "Distance", Type::Scalar, {{"A", Type::Vector3}, {"B", Type::Vector3}}},
        {Kind::Length, "Length", Type::Scalar, {{"Input", Type::Numeric}}},
        {Kind::AppendVector, "Append", Type::Numeric, {{"A", Type::Numeric}, {"B", Type::Numeric}}},
        {Kind::ComponentMask, "Component Mask", Type::Numeric, {{"Input", Type::Numeric}}},
        {Kind::ReflectionVector, "Reflection Vector", Type::Vector3, {}},
        {Kind::Desaturation,
         "Desaturation",
         Type::Vector3,
         {{"Input", Type::Vector3, {1, 1, 1}}, {"Fraction", Type::Scalar, {1}}}},
        {Kind::If,
         "If",
         Type::Numeric,
         {{"A", Type::Scalar},
          {"B", Type::Scalar},
          {"A > B", Type::Numeric, {1, 1, 1, 1}},
          {"A == B", Type::Numeric},
          {"A < B", Type::Numeric}}},
        {Kind::SphereMask,
         "Sphere Mask",
         Type::Scalar,
         {{"A", Type::Vector3}, {"B", Type::Vector3}, {"Radius", Type::Scalar, {0.5f}}, {"Hardness", Type::Scalar, {0.5f}}}},
        {Kind::CheapContrast, "Cheap Contrast", Type::Scalar, {{"In", Type::Scalar}, {"Contrast", Type::Scalar, {1}}}},
        {Kind::Noise, "Noise", Type::Scalar, {{"Position", Type::Vector3}, {"Scale", Type::Scalar, {4}}}},
        {Kind::Reroute, "Reroute", Type::Numeric, {{"In", Type::Numeric}}},
        // ---- 2026-11 追加（UE のノード / マテリアル関数）
        {Kind::StaticSwitch, "Static Switch", Type::Numeric, {{"True", Type::Numeric, {1, 1, 1, 1}}, {"False", Type::Numeric}}},
        {Kind::Pi, "Pi", Type::Scalar, {}},
        {Kind::RemapValueRange,
         "Remap Value Range",
         Type::Numeric,
         {{"Input", Type::Numeric},
          {"Input Low", Type::Scalar},
          {"Input High", Type::Scalar, {1}},
          {"Target Low", Type::Scalar},
          {"Target High", Type::Scalar, {1}}}},
        {Kind::Posterize, "Posterize", Type::Numeric, {{"Input", Type::Numeric}, {"Steps", Type::Scalar, {4}}}},
        {Kind::HueShift, "Hue Shift", Type::Vector3, {{"Color", Type::Vector3, {1, 0, 0}}, {"Shift", Type::Scalar}}},
        {Kind::Blend,
         "Blend",
         Type::Vector3,
         {{"Base", Type::Vector3, {0.5f, 0.5f, 0.5f}}, {"Blend", Type::Vector3, {0.5f, 0.5f, 0.5f}}, {"Opacity", Type::Scalar, {1}}}},
        {Kind::LinearGradient, "Linear Gradient", Type::Scalar, {{"UV", Type::Vector2}}},
        {Kind::RadialGradient,
         "Radial Gradient",
         Type::Scalar,
         {{"UV", Type::Vector2}, {"Center", Type::Vector2, {0.5f, 0.5f}}, {"Radius", Type::Scalar, {0.5f}}, {"Density", Type::Scalar, {2}}}},
        {Kind::Voronoi,
         "Voronoi",
         Type::Scalar,
         {{"UV", Type::Vector2}, {"Scale", Type::Scalar, {8}}, {"Jitter", Type::Scalar, {1}}}},
        {Kind::NormalFromHeight,
         "Normal From Height",
         Type::Vector3,
         {{"Height", Type::Scalar, {0.5f}}, {"Strength", Type::Scalar, {0.05f}}}},
        {Kind::BlendAngleCorrectedNormals,
         "Blend Angle Corrected Normals",
         Type::Vector3,
         {{"Base Normal", Type::Vector3, {0, 0, 1}}, {"Additional Normal", Type::Vector3, {0, 0, 1}}}},
        {Kind::FlattenNormal, "Flatten Normal", Type::Vector3, {{"Normal", Type::Vector3, {0, 0, 1}}, {"Flatness", Type::Scalar}}},
        {Kind::BumpOffset,
         "Bump Offset",
         Type::Vector2,
         {{"Height", Type::Scalar, {0.5f}},
          {"Height Ratio", Type::Scalar, {0.05f}},
          {"Reference Plane", Type::Scalar, {0.5f}},
          {"Coordinate", Type::Vector2}}},
        {Kind::RotateAboutAxis,
         "Rotate About Axis",
         Type::Vector3,
         {{"Rotation Axis", Type::Vector3, {0, 0, 1}},
          {"Rotation Angle", Type::Scalar},
          {"Pivot Point", Type::Vector3},
          {"Position", Type::Vector3}}},
        {Kind::BlackBody, "Black Body", Type::Vector3, {{"Temp", Type::Scalar, {6500}}}}};

    int Dimensions(Type type)
    {
        return type == Type::Numeric ? 1 : static_cast<int>(type);
    }

    bool Compatible(Type source, Type target)
    {
        // Vector3 → Vector4 はアルファ 1 を足して受け付ける（UE の Base Color に float3 を繋ぐのと同じ）
        return target == Type::Numeric || source == Type::Scalar || source == target ||
               (static_cast<int>(target) > 1 && static_cast<int>(source) > static_cast<int>(target)) ||
               (source == Type::Vector3 && target == Type::Vector4);
    }
}

std::span<const MaterialNodeDefinition> MaterialGraph::Definitions()
{
    return NodeDefinitions;
}

const MaterialNodeDefinition& MaterialGraph::Definition(MaterialNodeKind kind)
{
    for (const auto& definition : NodeDefinitions)
    {
        if (definition.Kind == kind)
        {
            return definition;
        }
    }
    return NodeDefinitions.front();
}

MaterialGraph MaterialGraph::FromMaterial(const MaterialAssetData& material)
{
    // マテリアルアセットの現在の値・テクスチャから、同じ見た目になるグラフを組み立てる
    // （テクスチャ / POM / 法線強度も含む。エンジン側はテクスチャを直接使うのでベイクは発生しない）
    MaterialGraph graph;
    const auto output = graph.AddNode(Kind::Output, 720, 40);
    std::string error;
    const auto pinOf = [&](MaterialOutputPin pin) {
        return graph.FindNode(output)->InputPins[static_cast<size_t>(pin)];
    };
    const auto add = [&](Kind kind, const char* name, std::array<float, 4> value, float x, float y) {
        const auto id = graph.AddNode(kind, x, y);
        if (auto* node = graph.FindNode(id))
        {
            if (name)
            {
                node->Name = name;
            }
            if (kind != Kind::TextureSample && kind != Kind::ComponentMask)
            {
                node->Value = value;
            }
        }
        return id;
    };
    const auto outputOf = [&](uint64_t node) {
        const auto* found = graph.FindNode(node);
        return found ? found->OutputPin : 0;
    };
    const auto inputOf = [&](uint64_t node, size_t index) {
        const auto* found = graph.FindNode(node);
        return found && index < found->InputPins.size() ? found->InputPins[index] : 0;
    };
    const auto link = [&](uint64_t sourceNode, uint64_t targetPin) {
        graph.Connect(outputOf(sourceNode), targetPin, error);
    };
    const auto texture = [&](MaterialTextureSlot slot, MaterialSamplerType sampler, const char* name, float y) -> uint64_t {
        const AssetHandle handle = material.Textures[static_cast<size_t>(slot)];
        if (handle == 0)
        {
            return 0;
        }
        const auto id = add(Kind::TextureSample, name, {}, 40, y);
        if (auto* node = graph.FindNode(id))
        {
            node->Texture = handle;
            node->Value = {static_cast<float>(sampler), 0, 0, 0};
        }
        return id;
    };
    const auto mask = [&](uint64_t source, int channel, float y) {
        const auto id = add(Kind::ComponentMask, nullptr, {}, 300, y);
        if (auto* node = graph.FindNode(id))
        {
            node->Value = {channel == 0 ? 1.0f : 0.0f, channel == 1 ? 1.0f : 0.0f, channel == 2 ? 1.0f : 0.0f, 0.0f};
        }
        graph.Connect(outputOf(source), inputOf(id, 0), error);
        return id;
    };
    const auto multiply = [&](uint64_t a, uint64_t b, float y) {
        const auto id = add(Kind::Multiply, nullptr, {}, 480, y);
        graph.Connect(outputOf(a), inputOf(id, 0), error);
        graph.Connect(outputOf(b), inputOf(id, 1), error);
        return id;
    };

    float y = 0.0f;
    // Base Color（テクスチャ × 色）
    {
        const auto base = texture(MaterialTextureSlot::BaseColor, MaterialSamplerType::Color, "Base Color Texture", y);
        const auto color = add(Kind::ColorParameter, "Base Color", material.BaseColor, base ? 300.0f : 40.0f, base ? y + 120 : y);
        link(base ? multiply(base, color, y + 40) : color, pinOf(MaterialOutputPin::BaseColor));
        y += base ? 260.0f : 150.0f;
    }
    // Metallic / Roughness（エンジンの MR テクスチャは R = Metallic, G = Roughness）
    {
        const auto mr = texture(MaterialTextureSlot::MetallicRoughness, MaterialSamplerType::Linear, "Metallic Roughness", y);
        const auto metallic = add(Kind::ScalarParameter, "Metallic", {material.Metallic}, mr ? 300.0f : 40.0f, y + (mr ? 180.0f : 0.0f));
        const auto roughness = add(Kind::ScalarParameter, "Roughness", {material.Roughness}, mr ? 300.0f : 40.0f, y + (mr ? 260.0f : 100.0f));
        if (mr)
        {
            link(multiply(mask(mr, 0, y), metallic, y), pinOf(MaterialOutputPin::Metallic));
            link(multiply(mask(mr, 1, y + 90), roughness, y + 120), pinOf(MaterialOutputPin::Roughness));
            y += 340.0f;
        }
        else
        {
            link(metallic, pinOf(MaterialOutputPin::Metallic));
            link(roughness, pinOf(MaterialOutputPin::Roughness));
            y += 200.0f;
        }
    }
    // Normal（法線マップ）と強さ
    if (const auto normal = texture(MaterialTextureSlot::Normal, MaterialSamplerType::Normal, "Normal Map", y))
    {
        link(normal, pinOf(MaterialOutputPin::Normal));
        y += 200.0f;
    }
    if (material.NormalScale != 1.0f)
    {
        link(add(Kind::ScalarParameter, "Normal Intensity", {material.NormalScale}, 40, y), pinOf(MaterialOutputPin::NormalIntensity));
        y += 100.0f;
    }
    // Emissive（テクスチャ × 色 × 強度）
    {
        const auto emissiveTexture = texture(MaterialTextureSlot::Emissive, MaterialSamplerType::Color, "Emissive Texture", y);
        const std::array<float, 4> emissive = {material.EmissiveColor[0] * material.EmissiveIntensity,
                                               material.EmissiveColor[1] * material.EmissiveIntensity,
                                               material.EmissiveColor[2] * material.EmissiveIntensity, 0};
        const auto color = add(Kind::VectorParameter, "Emissive", emissive, emissiveTexture ? 300.0f : 40.0f,
                               emissiveTexture ? y + 120 : y);
        if (emissiveTexture)
        {
            const auto rgb = add(Kind::ComponentMask, nullptr, {}, 300, y);
            if (auto* node = graph.FindNode(rgb))
            {
                node->Value = {1, 1, 1, 0};
            }
            graph.Connect(outputOf(emissiveTexture), inputOf(rgb, 0), error);
            link(multiply(rgb, color, y + 40), pinOf(MaterialOutputPin::Emissive));
            y += 260.0f;
        }
        else
        {
            link(color, pinOf(MaterialOutputPin::Emissive));
            y += 120.0f;
        }
    }
    // AO
    if (const auto ao = texture(MaterialTextureSlot::AO, MaterialSamplerType::Linear, "Ambient Occlusion", y))
    {
        link(mask(ao, 0, y), pinOf(MaterialOutputPin::AO));
        y += 200.0f;
    }
    // Height（POM）と深さ / ステップ
    if (const auto height = texture(MaterialTextureSlot::Height, MaterialSamplerType::Linear, "Height", y))
    {
        link(mask(height, 0, y), pinOf(MaterialOutputPin::Height));
        y += 200.0f;
    }
    if (material.ParallaxDepth > 0.0f || material.Textures[static_cast<size_t>(MaterialTextureSlot::Height)] != 0)
    {
        link(add(Kind::ScalarParameter, "Parallax Depth", {material.ParallaxDepth}, 40, y), pinOf(MaterialOutputPin::ParallaxDepth));
        link(add(Kind::ScalarParameter, "Parallax Steps", {material.ParallaxSteps}, 40, y + 100), pinOf(MaterialOutputPin::ParallaxSteps));
    }
    graph.Revision = 0;
    return graph;
}

uint64_t MaterialGraph::AddNode(MaterialNodeKind kind, float x, float y)
{
    if (Nodes.size() >= MaxNodes || m_NextId + 32 >= MaxIdentity || !std::isfinite(x) || !std::isfinite(y))
    {
        return 0;
    }
    if (kind == Kind::Output && std::any_of(Nodes.begin(), Nodes.end(), [](const auto& node) {
            return node.Kind == Kind::Output;
        }))
    {
        return 0;
    }
    const auto& definition = Definition(kind);
    MaterialGraphNode node;
    node.Id = m_NextId++;
    node.Kind = kind;
    node.Name = definition.Name;
    node.Position = {x, y};
    switch (kind)
    {
        case Kind::TextureCoordinate:
            node.Value = {1, 1, 0, 0}; // UTiling, VTiling
            break;
        case Kind::ComponentMask:
            node.Value = {1, 1, 0, 0}; // R, G
            break;
        case Kind::Constant4Vector:
            node.Value = {0, 0, 0, 1};
            break;
        case Kind::Group:
            node.Name = "Comment";
            node.Value = {0.30f, 0.42f, 0.60f, 1.0f}; // コメント色
            break;
        case Kind::StaticSwitch:
            node.Value = {1, 0, 0, 0}; // 既定は True
            break;
        case Kind::Pi:
            node.Value = {1, 0, 0, 0}; // Multiplier
            break;
        case Kind::Blend:
            node.Value = {static_cast<float>(MaterialBlendMode::Overlay), 0, 0, 0};
            break;
        default:
            break;
    }
    for (size_t i = 0; i < definition.Inputs.size(); ++i)
    {
        node.InputPins.push_back(m_NextId++);
    }
    if (kind != Kind::Output && kind != Kind::Group)
    {
        node.OutputPin = m_NextId++;
    }
    const auto id = node.Id;
    Nodes.push_back(std::move(node));
    ++Revision;
    return id;
}

MaterialGraphNode* MaterialGraph::FindNode(uint64_t id)
{
    for (auto& node : Nodes)
    {
        if (node.Id == id)
        {
            return &node;
        }
    }
    return nullptr;
}

const MaterialGraphNode* MaterialGraph::FindNode(uint64_t id) const
{
    for (const auto& node : Nodes)
    {
        if (node.Id == id)
        {
            return &node;
        }
    }
    return nullptr;
}

const MaterialGraphNode* MaterialGraph::FindPinNode(uint64_t pin, bool* input, size_t* index) const
{
    if (pin == 0)
    {
        return nullptr;
    }
    for (const auto& node : Nodes)
    {
        if (node.OutputPin == pin)
        {
            if (input)
            {
                *input = false;
            }
            if (index)
            {
                *index = 0;
            }
            return &node;
        }
        for (size_t i = 0; i < node.InputPins.size(); ++i)
        {
            if (node.InputPins[i] == pin)
            {
                if (input)
                {
                    *input = true;
                }
                if (index)
                {
                    *index = i;
                }
                return &node;
            }
        }
    }
    return nullptr;
}

const MaterialGraphLink* MaterialGraph::InputLink(uint64_t pin) const
{
    for (const auto& link : Links)
    {
        if (link.TargetPin == pin)
        {
            return &link;
        }
    }
    return nullptr;
}

bool MaterialGraph::RemoveNode(uint64_t id)
{
    const auto* node = FindNode(id);
    if (!node || node->Kind == Kind::Output)
    {
        return false;
    }
    std::unordered_set<uint64_t> pins(node->InputPins.begin(), node->InputPins.end());
    pins.insert(node->OutputPin);
    std::erase_if(Links, [&](const auto& link) {
        return pins.contains(link.SourcePin) || pins.contains(link.TargetPin);
    });
    std::erase_if(Nodes, [&](const auto& candidate) {
        return candidate.Id == id;
    });
    ++Revision;
    return true;
}

bool MaterialGraph::RemoveLink(uint64_t id)
{
    if (std::erase_if(Links, [&](const auto& link) {
            return link.Id == id;
        }) == 0)
    {
        return false;
    }
    ++Revision;
    return true;
}

bool MaterialGraph::Reaches(uint64_t start, uint64_t target) const
{
    std::vector<uint64_t> pending{start};
    std::unordered_set<uint64_t> visited;
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        if (current == target)
        {
            return true;
        }
        if (!visited.insert(current).second)
        {
            continue;
        }
        const auto* node = FindNode(current);
        if (!node)
        {
            continue;
        }
        for (const auto& link : Links)
        {
            if (link.SourcePin == node->OutputPin)
            {
                const auto* downstream = FindPinNode(link.TargetPin);
                if (downstream)
                {
                    pending.push_back(downstream->Id);
                }
            }
        }
    }
    return false;
}

MaterialGraphType MaterialGraph::OutputType(uint64_t id, std::vector<uint64_t>& visiting) const
{
    const auto* node = FindNode(id);
    if (!node || std::find(visiting.begin(), visiting.end(), id) != visiting.end())
    {
        return Type::Numeric;
    }
    const auto& definition = Definition(node->Kind);
    if (definition.OutputType != Type::Numeric)
    {
        return definition.OutputType;
    }
    auto linkedType = [&](size_t input, Type fallback) {
        const auto* link = input < node->InputPins.size() ? InputLink(node->InputPins[input]) : nullptr;
        const auto* source = link ? FindPinNode(link->SourcePin) : nullptr;
        return source ? OutputType(source->Id, visiting) : fallback;
    };
    if (node->Kind == Kind::StaticSwitch)
    {
        visiting.push_back(id);
        const size_t selected = node->Value[0] > 0.5f ? 0 : 1;
        const Type result = linkedType(selected, Type::Scalar);
        visiting.pop_back();
        return result;
    }
    if (node->Kind == Kind::AppendVector)
    {
        visiting.push_back(id);
        const int count = Dimensions(linkedType(0, Type::Scalar)) + Dimensions(linkedType(1, Type::Scalar));
        visiting.pop_back();
        return static_cast<Type>(std::clamp(count, 1, 4));
    }
    if (node->Kind == Kind::ComponentMask)
    {
        int count = 0;
        for (float flag : node->Value)
        {
            count += flag > 0.5f ? 1 : 0;
        }
        return static_cast<Type>(std::clamp(count, 1, 4));
    }
    visiting.push_back(id);
    Type result = Type::Scalar;
    for (const auto pin : node->InputPins)
    {
        const auto* link = InputLink(pin);
        const auto* source = link ? FindPinNode(link->SourcePin) : nullptr;
        if (source)
        {
            result = static_cast<Type>(
                (std::max)(static_cast<int>(result), static_cast<int>(OutputType(source->Id, visiting))));
        }
    }
    visiting.pop_back();
    return result;
}

MaterialGraphType MaterialGraph::ResolveOutputType(uint64_t node) const
{
    std::vector<uint64_t> visiting;
    return OutputType(node, visiting);
}

bool MaterialGraph::CanConnect(uint64_t source, uint64_t target, std::string& error) const
{
    error.clear();
    bool sourceInput = false;
    bool targetInput = false;
    size_t inputIndex = 0;
    const auto* sourceNode = FindPinNode(source, &sourceInput);
    const auto* targetNode = FindPinNode(target, &targetInput, &inputIndex);
    if (!sourceNode || !targetNode || sourceInput || !targetInput)
    {
        error = "Connect an output pin to an input pin.";
        return false;
    }
    if (sourceNode->Id == targetNode->Id || Reaches(targetNode->Id, sourceNode->Id))
    {
        error = "This connection would create a cycle.";
        return false;
    }
    std::vector<uint64_t> visiting;
    if (!Compatible(OutputType(sourceNode->Id, visiting), Definition(targetNode->Kind).Inputs[inputIndex].Type))
    {
        error = "Pin types are incompatible. Scalars may broadcast; vectors must have matching dimensions.";
        return false;
    }
    return true;
}

bool MaterialGraph::Connect(uint64_t source, uint64_t target, std::string& error)
{
    if (Links.size() >= MaxLinks || m_NextId >= MaxIdentity || !CanConnect(source, target, error))
    {
        return false;
    }
    MaterialGraph candidate = *this;
    std::erase_if(candidate.Links, [&](const auto& link) {
        return link.TargetPin == target;
    });
    candidate.Links.push_back({candidate.m_NextId++, source, target});
    if (!candidate.Validate(error))
    {
        return false;
    }
    ++candidate.Revision;
    *this = std::move(candidate);
    return true;
}

bool MaterialGraph::Validate(std::string& error) const
{
    error.clear();
    std::unordered_set<uint64_t> ids;
    size_t outputs = 0;
    const auto unique = [&](uint64_t id) {
        return id > 0 && id <= MaxIdentity && ids.insert(id).second;
    };
    for (const auto& node : Nodes)
    {
        const auto& definition = Definition(node.Kind);
        if (!unique(node.Id) || node.InputPins.size() != definition.Inputs.size() || definition.Kind != node.Kind)
        {
            error = "Invalid node identity, kind or pin count.";
            return false;
        }
        if (node.Kind == Kind::Output)
        {
            ++outputs;
        }
        if ((node.Kind == Kind::Output || node.Kind == Kind::Group) && node.OutputPin != 0)
        {
            error = "Material Output and comment groups cannot have output pins.";
            return false;
        }
        for (const auto pin : node.InputPins)
        {
            if (!unique(pin))
            {
                error = "Duplicate input pin identity.";
                return false;
            }
        }
        if (node.Kind != Kind::Output && node.Kind != Kind::Group && !unique(node.OutputPin))
        {
            error = "Invalid output pin identity.";
            return false;
        }
        for (const float value : node.Value)
        {
            if (!std::isfinite(value))
            {
                error = "Non-finite node value.";
                return false;
            }
        }
        for (const float value : node.Position)
        {
            if (!std::isfinite(value))
            {
                error = "Non-finite node position.";
                return false;
            }
        }
        for (const float value : node.GroupSize)
        {
            if (!std::isfinite(value) || value <= 0)
            {
                error = "Invalid group size.";
                return false;
            }
        }
    }
    if (outputs != 1)
    {
        error = "A material graph must have exactly one Material Output.";
        return false;
    }
    std::unordered_set<uint64_t> targets;
    for (const auto& node : Nodes)
    {
        if (node.Kind == Kind::AppendVector)
        {
            std::vector<uint64_t> visiting;
            int count = 0;
            for (const auto pin : node.InputPins)
            {
                const auto* link = InputLink(pin);
                const auto* source = link ? FindPinNode(link->SourcePin) : nullptr;
                count += source ? Dimensions(OutputType(source->Id, visiting)) : 1;
            }
            if (count > 4)
            {
                error = "Append: the combined vector exceeds 4 components.";
                return false;
            }
        }
    }
    for (const auto& link : Links)
    {
        if (!unique(link.Id) || !targets.insert(link.TargetPin).second ||
            !CanConnect(link.SourcePin, link.TargetPin, error))
        {
            if (error.empty())
            {
                error = "Duplicate link identity or multiple connections to one input.";
            }
            return false;
        }
    }
    for (const auto& node : Nodes)
    {
        if (Definition(node.Kind).OutputType != Type::Numeric || node.Kind == Kind::Output ||
            node.Kind == Kind::AppendVector || node.Kind == Kind::ComponentMask)
        {
            continue;
        }
        Type vectorType = Type::Scalar;
        for (const auto pin : node.InputPins)
        {
            const auto* link = InputLink(pin);
            const auto* source = link ? FindPinNode(link->SourcePin) : nullptr;
            if (!source)
            {
                continue;
            }
            std::vector<uint64_t> visiting;
            const auto type = OutputType(source->Id, visiting);
            if (type != Type::Scalar)
            {
                if (vectorType != Type::Scalar && vectorType != type)
                {
                    error = "Arithmetic vector inputs must have matching dimensions.";
                    return false;
                }
                vectorType = type;
            }
        }
    }
    return true;
}

std::string MaterialGraph::Serialize() const
{
    Json::Value document(Json::objectValue);
    document["version"] = 1;
    document["nodes"] = Json::Value(Json::arrayValue);
    document["links"] = Json::Value(Json::arrayValue);
    for (const auto& node : Nodes)
    {
        Json::Value value(Json::objectValue);
        value["id"] = Json::UInt64(node.Id);
        value["kind"] = Definition(node.Kind).Name;
        value["name"] = node.Name;
        value["texture"] = FormatAssetHandle(node.Texture);
        value["output"] = Json::UInt64(node.OutputPin);
        value["inputs"] = Json::Value(Json::arrayValue);
        for (const auto pin : node.InputPins)
        {
            value["inputs"].append(Json::UInt64(pin));
        }
        for (const float component : node.Value)
        {
            value["value"].append(component);
        }
        for (const float component : node.Position)
        {
            value["position"].append(component);
        }
        for (const float component : node.GroupSize)
        {
            value["size"].append(component);
        }
        if (node.Width > 0.0f)
        {
            value["width"] = node.Width;
        }
        if (node.Height > 0.0f)
        {
            value["height"] = node.Height;
        }
        if (!node.Comment.empty())
        {
            value["comment"] = node.Comment;
        }
        document["nodes"].append(std::move(value));
    }
    for (const auto& link : Links)
    {
        Json::Value value(Json::objectValue);
        value["id"] = Json::UInt64(link.Id);
        value["source"] = Json::UInt64(link.SourcePin);
        value["target"] = Json::UInt64(link.TargetPin);
        document["links"].append(std::move(value));
    }
    // グラフ設定（既定値と同じなら書かない：旧バージョンとの差分を最小にする）
    if (!(Settings == MaterialGraphSettings{}))
    {
        Json::Value settings(Json::objectValue);
        settings["surface"] = Settings.Surface;
        settings["cull"] = Settings.Cull;
        settings["shadingModel"] = Settings.ShadingModel;
        settings["shadowMode"] = Settings.ShadowMode;
        settings["alphaCutoff"] = Settings.AlphaCutoff;
        settings["bakeResolution"] = Settings.BakeResolution;
        settings["bakeTime"] = Settings.BakeTime;
        settings["autoBake"] = Settings.AutoBake;
        document["settings"] = std::move(settings);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, document);
}

bool MaterialGraph::Deserialize(const std::string& contents, MaterialGraph& graph, std::string& error)
{
    Json::Value document;
    Json::CharReaderBuilder reader;
    std::istringstream input(contents);
    if (contents.size() > 8 * 1024 * 1024 || !Json::parseFromStream(reader, input, &document, &error) ||
        !document.isObject() || !document["version"].isInt() || document["version"].asInt() != 1 ||
        !document["nodes"].isArray() || document["nodes"].size() > MaxNodes || !document["links"].isArray() ||
        document["links"].size() > MaxLinks)
    {
        error = "Invalid or unsupported material graph document. " + error;
        return false;
    }
    MaterialGraph loaded;
    const auto readArray = [&](const Json::Value& source, auto& destination) {
        if (!source.isArray() || source.size() != destination.size())
        {
            return false;
        }
        for (Json::ArrayIndex i = 0; i < source.size(); ++i)
        {
            if (!source[i].isNumeric())
            {
                return false;
            }
            destination[i] = source[i].asFloat();
        }
        return true;
    };
    for (const auto& value : document["nodes"])
    {
        if (!value.isObject() || !value["id"].isUInt64() || !value["kind"].isString() || !value["name"].isString() ||
            !value["texture"].isString() || !value["output"].isUInt64() || !value["inputs"].isArray())
        {
            error = "Malformed material graph node.";
            return false;
        }
        const auto definition = std::find_if(NodeDefinitions.begin(), NodeDefinitions.end(), [&](const auto& item) {
            return value["kind"].asString() == item.Name;
        });
        if (definition == NodeDefinitions.end())
        {
            error = "Unknown node kind.";
            return false;
        }
        MaterialGraphNode node;
        node.Id = value["id"].asUInt64();
        node.Kind = definition->Kind;
        node.Name = value["name"].asString();
        node.Texture = ParseAssetHandle(value["texture"].asString());
        node.OutputPin = value["output"].asUInt64();
        if (!readArray(value["value"], node.Value) || !readArray(value["position"], node.Position) ||
            !readArray(value["size"], node.GroupSize))
        {
            error = "Malformed node values or position.";
            return false;
        }
        // 追加項目（旧バージョンのファイルには無い）
        if (value.isMember("width") && value["width"].isNumeric())
        {
            const float width = value["width"].asFloat();
            node.Width = std::isfinite(width) ? std::clamp(width, 0.0f, 1024.0f) : 0.0f;
        }
        if (value.isMember("height") && value["height"].isNumeric())
        {
            const float height = value["height"].asFloat();
            node.Height = std::isfinite(height) ? std::clamp(height, 0.0f, 2048.0f) : 0.0f;
        }
        if (value.isMember("comment") && value["comment"].isString())
        {
            node.Comment = value["comment"].asString();
        }
        for (const auto& pin : value["inputs"])
        {
            if (!pin.isUInt64())
            {
                error = "Malformed pin identity.";
                return false;
            }
            node.InputPins.push_back(pin.asUInt64());
        }
        loaded.Nodes.push_back(std::move(node));
    }
    for (const auto& value : document["links"])
    {
        if (!value.isObject() || !value["id"].isUInt64() || !value["source"].isUInt64() || !value["target"].isUInt64())
        {
            error = "Malformed material graph link.";
            return false;
        }
        loaded.Links.push_back({value["id"].asUInt64(), value["source"].asUInt64(), value["target"].asUInt64()});
    }
    if (document.isMember("settings") && document["settings"].isObject())
    {
        const auto& settings = document["settings"];
        const auto readInt = [&](const char* name, int& target, int minimum, int maximum) {
            if (settings[name].isInt())
            {
                target = std::clamp(settings[name].asInt(), minimum, maximum);
            }
        };
        const auto readFloat = [&](const char* name, float& target, float minimum, float maximum) {
            if (settings[name].isNumeric())
            {
                const float value = settings[name].asFloat();
                target = std::isfinite(value) ? std::clamp(value, minimum, maximum) : target;
            }
        };
        readInt("surface", loaded.Settings.Surface, -1, 2);
        readInt("cull", loaded.Settings.Cull, -1, 2);
        readInt("shadingModel", loaded.Settings.ShadingModel, -1, 5);
        readInt("shadowMode", loaded.Settings.ShadowMode, -1, 4);
        readFloat("alphaCutoff", loaded.Settings.AlphaCutoff, -1.0f, 1.0f);
        readInt("bakeResolution", loaded.Settings.BakeResolution, 64, 2048);
        readFloat("bakeTime", loaded.Settings.BakeTime, 0.0f, 3600.0f);
        if (settings["autoBake"].isBool())
        {
            loaded.Settings.AutoBake = settings["autoBake"].asBool();
        }
    }
    uint64_t highest = 0;
    for (const auto& node : loaded.Nodes)
    {
        highest = (std::max)({highest, node.Id, node.OutputPin});
        for (const auto pin : node.InputPins)
        {
            highest = (std::max)(highest, pin);
        }
    }
    for (const auto& link : loaded.Links)
    {
        highest = (std::max)(highest, link.Id);
    }
    // 旧バージョンのグラフ：後から入力ピンが増えたノード（Material Output の Height / POM など）にピンを足す
    for (auto& node : loaded.Nodes)
    {
        const auto& definition = Definition(node.Kind);
        while (node.InputPins.size() < definition.Inputs.size() && highest + 1 < MaxIdentity)
        {
            node.InputPins.push_back(++highest);
        }
    }
    if (!loaded.Validate(error))
    {
        return false;
    }
    loaded.m_NextId = highest + 1;
    graph = std::move(loaded);
    return true;
}

bool MaterialGraph::Save(const std::filesystem::path& path, std::string& error) const
{
    return Validate(error) && WriteAssetFile(path, Serialize(), error);
}

bool MaterialGraph::Load(const std::filesystem::path& path, MaterialGraph& graph, std::string& error)
{
    std::error_code filesystemError;
    const auto size = std::filesystem::file_size(path, filesystemError);
    if (filesystemError || size > 8 * 1024 * 1024)
    {
        error = "Cannot read material graph: " + path.generic_string();
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input)
    {
        error = "Failed to read material graph: " + path.generic_string();
        return false;
    }
    return Deserialize(contents.str(), graph, error);
}

