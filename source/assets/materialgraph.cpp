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
          {"AO", Type::Scalar, {1}}}},
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
        {Kind::Reroute, "Reroute", Type::Numeric, {{"In", Type::Numeric}}}};

    int Dimensions(Type type)
    {
        return type == Type::Numeric ? 1 : static_cast<int>(type);
    }

    bool Compatible(Type source, Type target)
    {
        return target == Type::Numeric || source == Type::Scalar || source == target ||
               (static_cast<int>(target) > 1 && static_cast<int>(source) > static_cast<int>(target));
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
    MaterialGraph graph;
    const auto output = graph.AddNode(Kind::Output, 580, 80);
    const auto addConstant = [&](Kind kind, const char* name, std::array<float, 4> value, size_t input, float y) {
        const auto id = graph.AddNode(kind, 40, y);
        auto* node = graph.FindNode(id);
        node->Name = name;
        node->Value = value;
        std::string error;
        graph.Connect(node->OutputPin, graph.FindNode(output)->InputPins[input], error);
    };
    addConstant(Kind::ColorParameter, "Base Color", material.BaseColor, 0, 20);
    addConstant(Kind::ScalarParameter, "Metallic", {material.Metallic}, 1, 170);
    addConstant(Kind::ScalarParameter, "Roughness", {material.Roughness}, 2, 280);
    addConstant(Kind::VectorParameter, "Emissive",
                {material.EmissiveColor[0] * material.EmissiveIntensity,
                 material.EmissiveColor[1] * material.EmissiveIntensity,
                 material.EmissiveColor[2] * material.EmissiveIntensity, 0},
                4, 390);
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
    if (!loaded.Validate(error))
    {
        return false;
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
