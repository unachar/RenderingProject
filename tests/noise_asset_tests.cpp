#include "assets/noiseasset.h"
#include "assets/assetregistry.h"
#include "procedural/proceduraltextureparams.h"
#include <cmath>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

static void Require(bool value, const char* message)
{
    if (!value)
    {
        throw std::runtime_error(message);
    }
}

int main()
{
    using namespace ProcStudio;
    try
    {
        const auto dir =
            std::filesystem::temp_directory_path() / ("render-noise-" + std::to_string(GenerateAssetHandle()));
        std::filesystem::create_directories(dir);

        struct Cleanup
        {
            std::filesystem::path Path;

            ~Cleanup()
            {
                std::filesystem::remove_all(Path);
            }
        } cleanup{dir};

        std::string error;
        TextureParams original;
        original.Noise.Seed = 4294967295u;
        original.Noise.Direction = -1.4f;
        original.Noise.Phase = 5.2f;
        original.Noise.Anisotropy = 7;
        original.Noise.KernelFrequency = 6;
        original.Time = 23.5f;
        original.ColorA = {0.2f, 0.4f, 0.6f};
        original.Output = TextureOutput::Normal;
        original.Pattern = GeometricPattern::Hexagons;
        const auto path = dir / "sample.noise";
        for (int basis = 0; basis < static_cast<int>(Basis::Count); ++basis)
        {
            original.Noise.BasisType = static_cast<Basis>(basis);
            original.Noise.Fractal = static_cast<FractalType>(basis % static_cast<int>(FractalType::Count));
            Require(NoiseSerializer::Save(path, original, error), error.c_str());
            TextureParams loaded;
            Require(NoiseSerializer::Load(path, loaded, error), error.c_str());
            Require(NoiseSerializer::Encode(loaded) == NoiseSerializer::Encode(original), "all parameters round-trip");
            Require(TextureScalar(loaded, .12345f, .78901f, 0, false) ==
                        TextureScalar(original, .12345f, .78901f, 0, false),
                    "definition preserves scalar field");
            const auto gpu = MakeTextureGpuParams(loaded);
            Require(gpu.Seed == UINT32_MAX && gpu.Direction == original.Noise.Direction && gpu.KernelFrequency == 6 &&
                        gpu.Phase == original.Noise.Phase,
                    "GPU packing retains extended parameters");
        }
        Require(AssetRegistry::Classify("FOO.NOISE") == AssetType::Noise, "classify noise case-insensitively");
        AssetRegistry registry(dir);
        const auto handle = registry.Register(path, AssetType::Noise, error);
        Require(handle != 0 && registry.TypeOf(handle) == AssetType::Noise, "noise GUID registration");
        const auto moved = dir / "moved.noise";
        Require(registry.Move(handle, moved, error) && registry.Resolve(handle) == moved, "noise move keeps GUID");
        // Material sources preserve ordinary texture slots while replacing .noise inputs independently.
        MaterialNoiseSources sources;
        MaterialAssetData material;
        material.Textures[0] = handle;
        material.Textures[1] = handle;
        material.Textures[5] = handle;
        const auto texturePath = dir / "regular.png";
        std::ofstream(texturePath) << "placeholder";
        const auto textureHandle = registry.Register(texturePath, AssetType::Texture, error);
        Require(textureHandle != 0, "ordinary texture registered");
        auto packed = sources.Update(registry, material, error);
        Require(error.empty() && packed[0].Width && packed[1].Width && packed[2].Width,
                "all three noise channels enabled");
        material.Textures[0] = textureHandle;
        material.Textures[1] = 0;
        packed = sources.Update(registry, material, error);
        Require(!packed[0].Width && !packed[1].Width && packed[2].Width, "noise to texture/empty clears stale records");
        auto modified = original;
        modified.Noise.Seed = 42;
        Require(NoiseSerializer::Save(moved, modified, error), "update definition");
        const auto timestamp = std::filesystem::last_write_time(moved);
        std::filesystem::last_write_time(moved, timestamp + std::chrono::seconds(1));
        packed = sources.Update(registry, material, error);
        Require(error.empty() && packed[2].Seed == 42, "refresh reloads modified definition");
        std::ofstream(moved) << "broken";
        std::filesystem::last_write_time(moved, timestamp + std::chrono::seconds(2));
        packed = sources.Update(registry, material, error);
        Require(!error.empty() && !packed[2].Width, "invalid replacement disables affected source");
        Require(NoiseSerializer::Save(moved, original, error), "repair definition");
        packed = sources.Update(registry, material, error);
        Require(error.empty() && packed[2].Width, "repair clears errors and restores source");
        const auto materialPath = dir / "mixed.material";
        Require(MaterialSerializer::Save(materialPath, material, error), "save mixed sources");
        MaterialAssetData reloaded;
        Require(MaterialSerializer::Load(materialPath, reloaded, error) && reloaded == material,
                "mixed GUIDs survive material save/reload");
        std::filesystem::remove(moved);
        Require(registry.Refresh(error), "refresh after deleting .noise");
        packed = sources.Update(registry, material, error);
        Require(!packed[2].Width && !error.empty(), "missing registered source reports an error after refresh");
        Require(NoiseSerializer::Save(moved, original, error) && registry.Refresh(error), "restore missing .noise");
        packed = sources.Update(registry, material, error);
        Require(packed[2].Width && error.empty(), "restored .noise keeps its metadata GUID");
        TextureParams unchanged = original;
        const auto before = NoiseSerializer::Encode(unchanged);
        for (const char* invalid :
             {"{}", "[]", "{\"version\":99}", "{\"version\":1,\"kind\":999}", "{\"version\":1,\"noise\":{\"seed\":-1}}",
              "{\"version\":1,\"kind\":\"foo\"}", "{\"version\":1,\"contrast\":1e300}",
              "{\"version\":1,\"colorA\":[1,2]}", "{\"version\":1,\"version\":1}", "{\"version\":1} garbage"})
        {
            std::ofstream(moved) << invalid;
            Require(!NoiseSerializer::Load(moved, unchanged, error) && !error.empty(), "reject malformed definition");
            Require(NoiseSerializer::Encode(unchanged) == before, "failed load is transactional");
        }
        for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f})
        {
            auto invalid = original;
            invalid.Noise.Frequency = bad;
            Require(!NoiseSerializer::Save(path, invalid, error), "invalid numeric definition rejected before writing");
        }
        for (auto kind : {TextureKind::Mandelbrot, TextureKind::Julia})
        {
            auto invalid = original;
            invalid.Kind = kind;
            Require(!NoiseSerializer::Validate(invalid, error), "CPU escape fractals cannot silently change algorithm");
        }
        static_assert(sizeof(TextureGpuParams) == 208);
        static_assert(offsetof(TextureGpuParams, Direction) == 192);
        std::cout
            << "Noise definitions: round-trip, all bases, transactional errors, validation, GUIDs and GPU layout PASS\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
