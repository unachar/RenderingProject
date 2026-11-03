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
    Group,
    // ---- 以下は UE のマテリアルノードに合わせて追加（シリアライズは名前で行うので末尾に追加する）
    Constant2Vector,
    Constant3Vector,
    Constant4Vector,
    TextureCoordinate,
    Panner,
    Rotator,
    // 単項演算（Abs〜DDY は連続していること）
    Abs,
    Ceil,
    Floor,
    Frac,
    Round,
    Truncate,
    Sign,
    OneMinus,
    Saturate,
    SquareRoot,
    Sine,
    Cosine,
    Tangent,
    Arcsine,
    Arccosine,
    Arctangent,
    Exponential,
    Logarithm2,
    DDX,
    DDY,
    // 二項演算（Power〜Arctangent2 は連続していること）
    Power,
    Min,
    Max,
    Fmod,
    Step,
    Arctangent2,
    ConstantBiasScale,
    CrossProduct,
    Distance,
    Length,
    AppendVector,
    ComponentMask,
    ReflectionVector,
    Desaturation,
    If,
    SphereMask,
    CheapContrast,
    Noise,
    Reroute,
    // ---- 2026-11 追加（UE のマテリアルノード / マテリアル関数）。シリアライズは名前なので末尾に追加する
    StaticSwitch,
    Pi,
    RemapValueRange,
    Posterize,
    HueShift,
    Blend,
    LinearGradient,
    RadialGradient,
    Voronoi,
    NormalFromHeight,
    BlendAngleCorrectedNormals,
    FlattenNormal,
    BumpOffset,
    RotateAboutAxis,
    BlackBody
};

// Material Output の入力ピン（順番はシリアライズ済みグラフのピン順なので変更しない。追加は末尾）
enum class MaterialOutputPin : size_t
{
    BaseColor,
    Metallic,
    Roughness,
    Normal,
    Emissive,
    Opacity,
    OpacityMask,
    AO,
    Height,          // POM の高さ（白 = 元の面、黒 = 最深部）
    NormalIntensity, // 法線マップの強さ（NormalScale）
    ParallaxDepth,   // POM の深さ（UV 単位）
    ParallaxSteps,   // POM の最大ステップ数
    IOR,
    Transmission,
    TransmissionRoughness,
    Thickness,
    Absorption,
    ShadingStrength,
    Count
};

// Texture Sample のサンプラー種別（UE の Sampler Type）。node.Value[0] に入れる
enum class MaterialSamplerType : int
{
    Color = 0,  // sRGB → 線形
    Normal = 1, // 格納値 * 2 - 1（BC5 の Z 再構成込み）
    Linear = 2  // 格納値そのまま（Masks / Linear Color）
};

// Blend ノードのモード（node.Value[0]）
enum class MaterialBlendMode : int
{
    Normal,
    Multiply,
    Screen,
    Overlay,
    SoftLight,
    HardLight,
    ColorDodge,
    ColorBurn,
    LinearDodge,
    LinearBurn,
    Difference,
    Darken,
    Lighten,
    Subtract,
    Count
};

// グラフ全体の設定（UE のマテリアルの Details に相当）。-1 / 負値は「マテリアルアセットの設定を使う」
struct MaterialGraphSettings
{
    int Surface = -1;          // 0 Opaque / 1 Masked / 2 Transparent
    int Cull = -1;             // 0 Back / 1 None（両面）/ 2 Front
    int ShadingModel = -1;     // 0 標準 / 1 クリアコート / 2 布 / 3 肌 / 4 髪 / 5 異方性
    int ShadowMode = -1;       // 0 None / 1 Opaque / 2 AlphaTest / 3 Transmittance / 4 Auto
    float AlphaCutoff = -1.0f; // Opacity Mask Clip Value（負値 = UE 既定 0.3333）
    int BakeResolution = 512;  // ピクセルごとの式をテクスチャへ焼くときの解像度
    float BakeTime = 0.0f;     // Time ノードを焼くときの時刻（秒）
    bool AutoBake = true;      // グラフ変更後に自動でテクスチャを焼き直す
    bool operator==(const MaterialGraphSettings&) const = default;
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
    float Width = 0.0f;  // ノード本体の幅（0 = 既定）。右端 / 右下のグリップで変更する
    float Height = 0.0f; // ノード本体の高さ（0 = 内容に合わせる）。下端 / 右下のグリップで変更する
    std::string Comment; // ノードの上に表示するコメント（UE のノードコメント）
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
    MaterialGraphSettings Settings;
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
    // ノード出力の型（Numeric の演算は入力から決まる。Append / Component Mask は成分数から決まる）
    MaterialGraphType ResolveOutputType(uint64_t node) const;
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

