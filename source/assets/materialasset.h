#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

using AssetHandle = uint64_t;
enum class AssetType : uint32_t
{
    Unknown,
    Material,
    Texture,
    Mesh,
    Noise
};

struct AssetDragPayload
{
    AssetHandle Handle = 0;
    AssetType Type = AssetType::Unknown;
};

inline constexpr char AssetPayloadType[] = "ASSET_HANDLE";
std::string FormatAssetHandle(AssetHandle handle);
AssetHandle GenerateAssetHandle();
AssetHandle ParseAssetHandle(const std::string& text);
bool SceneHasMaterialAssets();

enum class MaterialSurface : uint32_t
{
    Opaque,
    Masked,
    Transparent
};
enum class MaterialShadowMode : uint32_t
{
    None,
    Opaque,
    AlphaTest,
    Transmittance,
    Auto
};
enum class MaterialCull : uint32_t
{
    Back,
    None,
    Front
};
enum class MaterialTextureSlot : uint32_t
{
    BaseColor,
    Normal,
    MetallicRoughness,
    AO,
    Emissive,
    Height,
    Count
};

struct MaterialAssetData
{
    std::array<float, 4> BaseColor{1, 1, 1, 1};
    float Metallic = 0.0f;
    float Roughness = 0.5f;
    float NormalScale = 1.0f;
    float AOStrength = 1.0f;
    std::array<float, 3> EmissiveColor{0, 0, 0};
    float EmissiveIntensity = 0.0f;
    float ParallaxDepth = 0.0f; // UV units; zero disables POM.
    float ParallaxSteps = 32.0f;
    std::array<AssetHandle, static_cast<size_t>(MaterialTextureSlot::Count)> Textures{};
    MaterialSurface Surface = MaterialSurface::Opaque;
    MaterialCull Cull = MaterialCull::Back;
    bool DepthWrite = true;
    float AlphaCutoff = 0.5f;
    MaterialShadowMode ShadowMode = MaterialShadowMode::Auto;
    float ShadowCutoff = 0.5f;
    float IOR = 1.5f;
    float Transmission = 0.0f;
    float TransmissionRoughness = 0.02f;
    float Thickness = 0.1f;
    std::array<float, 3> AbsorptionCoefficient{0, 0, 0};
    // 拡張シェーディングモデル: 0 標準、1 クリアコート、2 布の光沢、3 肌、4 髪、5 異方性。
    uint32_t ShadingModel = 0;
    float ShadingStrength = 1.0f;
    bool operator==(const MaterialAssetData&) const = default;
};

class MaterialSerializer
{
  public:
    static bool Validate(const MaterialAssetData& material, std::string& error);
    static bool Load(const std::filesystem::path& path, MaterialAssetData& material, std::string& error);
    static bool Save(const std::filesystem::path& path, const MaterialAssetData& material, std::string& error);
};

// 同じディレクトリ内でファイルをアトミックに置換する。書き込み失敗時は置換先を保持する。
bool WriteAssetFile(const std::filesystem::path& path, const std::string& contents, std::string& error);
