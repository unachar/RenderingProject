#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

using AssetHandle = uint64_t;
enum class AssetType : uint32_t { Unknown, Material, Texture, Mesh };
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

enum class MaterialSurface : uint32_t { Opaque, Masked, Transparent };
enum class MaterialShadowMode : uint32_t { None, Opaque, AlphaTest, Transmittance, Auto };
enum class MaterialCull : uint32_t { Back, None, Front };
enum class MaterialTextureSlot : uint32_t { BaseColor, Normal, MetallicRoughness, AO, Emissive, Count };

struct MaterialAssetData
{
    std::array<float, 4> BaseColor{1, 1, 1, 1};
    float Metallic = 0.0f;
    float Roughness = 0.5f;
    float NormalScale = 1.0f;
    float AOStrength = 1.0f;
    std::array<float, 3> EmissiveColor{0, 0, 0};
    float EmissiveIntensity = 0.0f;
    std::array<AssetHandle, 5> Textures{};
    MaterialSurface Surface = MaterialSurface::Opaque;
    MaterialCull Cull = MaterialCull::Back;
    bool DepthWrite = true;
    float AlphaCutoff = 0.5f;
    MaterialShadowMode ShadowMode = MaterialShadowMode::Auto;
    float ShadowCutoff = 0.5f;
    float IOR = 1.5f;
    float Transmission = 0.0f;
    float Thickness = 0.1f;
    std::array<float, 3> AbsorptionCoefficient{0, 0, 0};
    bool operator==(const MaterialAssetData&) const = default;
};

class MaterialSerializer
{
public:
    static bool Validate(const MaterialAssetData& material, std::string& error);
    static bool Load(const std::filesystem::path& path, MaterialAssetData& material, std::string& error);
    static bool Save(const std::filesystem::path& path, const MaterialAssetData& material, std::string& error);
};

// Atomically replaces a file in the same directory; an unsuccessful write leaves its destination intact.
bool WriteAssetFile(const std::filesystem::path& path, const std::string& contents, std::string& error);
