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
    float PositionError = 0;        // 0 では元の境界から誤差を自動設定する。
    float LODError = 0;             // 0 では製品用ビルダーの既定値を使う。
    uint32_t StreamingPriority = 1; // 0 は低品質、1 は標準、2 は高品質。
    VirtualGeometryBuildConfig Custom = VGSettings::Production();
    VirtualGeometryBuildConfig BuildConfig() const;
    VGCompressionSettings Compression(float sourceExtent) const;
};

namespace VirtualGeometryImport
{
    bool Load(const std::filesystem::path& source, VGImportSettings& settings, std::string& error);
    bool Save(const std::filesystem::path& source, const VGImportSettings& settings, std::string& error);
    uint64_t SettingsKey(const VGImportSettings& settings);
} // 名前空間 VirtualGeometryImport の終わり。
