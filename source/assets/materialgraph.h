#pragma once
#include "materialasset.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class MaterialGraphType
{
    Scalar = 1,
    Vector2 = 2,
    Vector3 = 3,
    Vector4 = 4,
    Numeric = 0
};
enum class MaterialNodeKind
{
    Constant,
    ScalarParameter,
    VectorParameter,
    ColorParameter,
    TextureSample,
    Add,
    Subtract,
    Multiply,
    Divide,
    Lerp,
    Clamp,
    Smoothstep,
    Normalize,
    Dot,
    Fresnel,
    UV,
    WorldPosition,
    WorldNormal,
    CameraVector,
    Time,
    Output,
    Group
};

struct MaterialGraphInput
{
    const char* Name;
    MaterialGraphType Type;
    std::array<float, 4> Default{};
};

struct MaterialNodeDefinition
{
    MaterialNodeKind Kind;
    const char* Name;
    MaterialGraphType OutputType;
    std::vector<MaterialGraphInput> Inputs;
};

struct MaterialGraphNode
{
    uint64_t Id = 0;
    MaterialNodeKind Kind = MaterialNodeKind::Constant;
    std::string Name;
    std::array<float, 4> Value{};
    AssetHandle Texture = 0;
    std::vector<uint64_t> InputPins;
    uint64_t OutputPin = 0;
    std::array<float, 2> Position{};
    std::array<float, 2> GroupSize{400.0f, 250.0f};
};

struct MaterialGraphLink
{
    uint64_t Id = 0;
    uint64_t SourcePin = 0;
    uint64_t TargetPin = 0;
};

// This is the material document, independent of ImGui/node-editor and GPU resources.
// The UI keeps undo snapshots of this value; shader jobs consume immutable copies.
class MaterialGraph
{
  public:
    std::vector<MaterialGraphNode> Nodes;
    std::vector<MaterialGraphLink> Links;
    uint64_t Revision = 0;
    uint64_t ParameterRevision = 0;

    static std::span<const MaterialNodeDefinition> Definitions();
    static const MaterialNodeDefinition& Definition(MaterialNodeKind kind);
    static MaterialGraph FromMaterial(const MaterialAssetData& material);
    uint64_t AddNode(MaterialNodeKind kind, float x, float y);
    bool RemoveNode(uint64_t node);
    bool CanConnect(uint64_t source, uint64_t target, std::string& error) const;
    bool Connect(uint64_t source, uint64_t target, std::string& error);
    bool RemoveLink(uint64_t link);
    MaterialGraphNode* FindNode(uint64_t node);
    const MaterialGraphNode* FindNode(uint64_t node) const;
    const MaterialGraphNode* FindPinNode(uint64_t pin, bool* input = nullptr, size_t* index = nullptr) const;
    const MaterialGraphLink* InputLink(uint64_t pin) const;
    bool Validate(std::string& error) const;
    std::string Serialize() const;
    static bool Deserialize(const std::string& contents, MaterialGraph& graph, std::string& error);
    bool Save(const std::filesystem::path& path, std::string& error) const;
    static bool Load(const std::filesystem::path& path, MaterialGraph& graph, std::string& error);

  private:
    uint64_t m_NextId = 1;
    bool Reaches(uint64_t startNode, uint64_t targetNode) const;
    MaterialGraphType OutputType(uint64_t node, std::vector<uint64_t>& visiting) const;
};
