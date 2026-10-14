#include "assets/materialasset.h"
#include "assets/assetregistry.h"
#include <fstream>
#include <windows.h>
#include <iostream>
#include <stdexcept>

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        const auto root = std::filesystem::path("build/material-tests/assets-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        MaterialAssetData authored;
        Check(authored.Roughness == 0.5f && authored.NormalScale == 1.0f, "PBR defaults");
        authored.BaseColor = {0.2f, 0.3f, 0.4f, 0.35f};
        authored.Textures[1] = 0xfedcba9876543210ull;
        authored.EmissiveIntensity = 3.0f;
        authored.Surface = MaterialSurface::Transparent;
        authored.Cull = MaterialCull::None;
        authored.DepthWrite = false;
        authored.AlphaCutoff = 0.3f;
        authored.ShadowMode = MaterialShadowMode::AlphaTest;
        authored.ShadowCutoff = 0.4f;
        authored.IOR = 1.45f;
        authored.Transmission = 0.95f;
        authored.Thickness = 0.2f;
        authored.AbsorptionCoefficient = {0.1f, 0.2f, 0.3f};
        std::string error;
        const auto path = root / "Brick.material";
        Check(MaterialSerializer::Save(path, authored, error), error.c_str());
        MaterialAssetData loaded;
        Check(MaterialSerializer::Load(path, loaded, error), error.c_str());
        Check(loaded == authored, "full material roundtrip including 64-bit texture handles");
        for (auto mode : {MaterialShadowMode::Auto, MaterialShadowMode::Transmittance, MaterialShadowMode::Opaque})
        {
            authored.ShadowMode = mode;
            Check(MaterialSerializer::Save(path, authored, error) && MaterialSerializer::Load(path, loaded, error) && loaded == authored,
                "optical shadow modes roundtrip");
        }
        auto invalidOptics = authored;
        invalidOptics.AbsorptionCoefficient[0] = -0.01f;
        Check(!MaterialSerializer::Validate(invalidOptics, error), "negative absorption cannot amplify light");
        invalidOptics = authored;
        invalidOptics.IOR = 0.5f;
        Check(!MaterialSerializer::Validate(invalidOptics, error), "invalid refractive index rejected");
        auto invalidShadow = authored;
        invalidShadow.ShadowMode = static_cast<MaterialShadowMode>(99);
        Check(!MaterialSerializer::Validate(invalidShadow, error), "reject unsupported shadow mode");
        invalidShadow = authored;
        invalidShadow.ShadowCutoff = -0.1f;
        Check(!MaterialSerializer::Validate(invalidShadow, error), "reject invalid shadow cutoff");
        Check(ParseAssetHandle(FormatAssetHandle(authored.Textures[1])) == authored.Textures[1], "handle precision");
        auto invalid = authored;
        invalid.Roughness = -1.0f;
        Check(!MaterialSerializer::Save(path, invalid, error), "reject invalid authored values");
        Check(MaterialSerializer::Load(path, loaded, error) && loaded == authored, "failed save preserves file");
        const auto malformed = root / "Bad.material";
        std::ofstream(malformed) << "{\"version\":1,\"parameters\":{\"roughness\":\"bad\"}}";
        Check(!MaterialSerializer::Load(malformed, loaded, error), "reject wrong field type");
        Check(loaded == authored, "failed load preserves caller data");
        std::ofstream(malformed) << "{\"version\":999}";
        Check(!MaterialSerializer::Load(malformed, loaded, error), "reject future schema");
        AssetRegistry registry(root);
        const auto handle = registry.Register(path, AssetType::Material, error);
        Check(handle != 0, error.c_str());
        const auto moved = root / "Environment" / "Wall.material";
        Check(registry.Move(handle, moved, error), error.c_str());
        registry.Refresh(error);
        Check(registry.Resolve(handle) == std::filesystem::absolute(moved).lexically_normal(), "move preserves identity");
        AssetRegistry reopened(root);
        Check(reopened.Refresh(error), error.c_str());
        Check(reopened.Resolve(handle) == registry.Resolve(handle), "identity survives registry restart");
        const auto copy = root / "Wall_Copy.material";
        const auto copyHandle = registry.Duplicate(handle, copy, error);
        Check(copyHandle != 0 && copyHandle != handle, "duplicate gets new handle");
        Check(MaterialSerializer::Load(copy, loaded, error) && loaded == authored, "duplicate preserves contents");
        const auto collision = root / "Collision.material";
        std::filesystem::copy_file(moved, collision, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(moved.string() + ".meta", collision.string() + ".meta", std::filesystem::copy_options::overwrite_existing);
        Check(!registry.Refresh(error), "duplicate metadata is reported");
        Check(registry.Resolve(handle).empty(), "ambiguous handles do not silently select an asset");
        // Leave the fixture reusable without recursively removing any directory.
        std::filesystem::remove(collision);
        std::filesystem::remove(collision.string() + ".meta");
        std::cout << "Material asset tests: PASS\n";
        return 0;
    }
    catch (const std::exception& failure)
    {
        std::cerr << "FAIL: " << failure.what() << '\n';
        return 1;
    }
}
