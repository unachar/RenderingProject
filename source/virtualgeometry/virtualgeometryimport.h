#pragma once
#include "virtualgeometryconfig.h"
#include "virtualgeometrycompression.h"
#include <filesystem>

enum class VGQuality : uint32_t
{
    Low,
    Medium,
    High,
    Production,
    Custom
};

struct VGImportSettings
{
    bool Enabled = true;
    bool BackgroundBuild = true;
    VGQuality Quality = VGQuality::Production;
    float PositionError = 0;        // Zero selects automatic error from source bounds.
    float LODError = 0;             // Zero selects the production builder default.
    uint32_t StreamingPriority = 1; // 0 low, 1 normal, 2 high.
    VirtualGeometryBuildConfig Custom = VGSettings::Production();
    VirtualGeometryBuildConfig BuildConfig() const;
    VGCompressionSettings Compression(float sourceExtent) const;
};

namespace VirtualGeometryImport
{
bool Load(const std::filesystem::path& source, VGImportSettings& settings, std::string& error);
bool Save(const std::filesystem::path& source, const VGImportSettings& settings, std::string& error);
uint64_t SettingsKey(const VGImportSettings& settings);
} // namespace VirtualGeometryImport
