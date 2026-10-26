#pragma once
#include "procedural/proceduralstudio.h"
#include "procedural/proceduraltextureparams.h"
#include "assetregistry.h"

// Versioned procedural definition: no image dimensions or pixel data.
class NoiseSerializer
{
  public:
    static bool Validate(const ProcStudio::TextureParams& params, std::string& error);
    static std::string Encode(const ProcStudio::TextureParams& params);
    static bool Load(const std::filesystem::path& path, ProcStudio::TextureParams& params, std::string& error);
    static bool Save(const std::filesystem::path& path, const ProcStudio::TextureParams& params, std::string& error);
};

// CPU-only binding cache, shared by material runtime and regression tests.
class MaterialNoiseSources
{
  public:
    std::array<ProcStudio::TextureGpuParams, 3> Update(const AssetRegistry& registry, const MaterialAssetData& material,
                                                       std::string& error);

  private:
    struct Source
    {
        AssetHandle Handle = 0;
        std::filesystem::path Path;
        std::filesystem::file_time_type Time{};
        ProcStudio::TextureGpuParams Parameters{};
    };

    std::array<Source, 3> m_Sources{};
};
