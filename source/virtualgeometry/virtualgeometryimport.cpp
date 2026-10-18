#include "virtualgeometryimport.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

VirtualGeometryBuildConfig VGImportSettings::BuildConfig() const
{
    auto config = Quality == VGQuality::Custom ? Custom : VGSettings::Production();
    switch (Quality)
    {
    case VGQuality::Low:
        config.TargetRelativeError = 0.1f;
        break;
    case VGQuality::Medium:
        config.TargetRelativeError = 0.05f;
        break;
    case VGQuality::High:
        config.TargetRelativeError = 0.02f;
        break;
    default:
        break;
    }
    if (LODError > 0)
    {
        config.TargetRelativeError = LODError;
    }
    // Every preset must obey the shipping Mesh Shader output limits.
    config.MaxClusterVertices = std::min(config.MaxClusterVertices, 64u);
    config.MaxClusterTriangles = std::min(config.MaxClusterTriangles, 124u);
    return config;
}

VGCompressionSettings VGImportSettings::Compression(float sourceExtent) const
{
    VGCompressionSettings settings;
    settings.MaxPositionError =
        PositionError > 0 ? PositionError : std::clamp(sourceExtent * 0.00001f, 0.000001f, 0.001f);
    return settings;
}

namespace VirtualGeometryImport
{
namespace
{
std::filesystem::path Sidecar(const std::filesystem::path& source)
{
    auto path = source;
    path += ".vgsettings";
    return path;
}

std::string Encode(const VGImportSettings& settings)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(9) << "VGSETTINGS 1\n"
           << settings.Enabled << ' ' << settings.BackgroundBuild << ' ' << static_cast<uint32_t>(settings.Quality)
           << ' ' << settings.PositionError << ' ' << settings.LODError << ' ' << settings.StreamingPriority << '\n';
    const auto& c = settings.Custom;
    output << c.MaxClusterTriangles << ' ' << c.MaxClusterVertices << ' ' << c.TargetClustersPerGroup << ' '
           << c.MaxHierarchyLevels << ' ' << c.SimplificationRatio << ' ' << c.ConeWeight << ' ' << c.NormalWeight
           << ' ' << c.TexCoordWeight << ' ' << c.ColorWeight << ' ' << c.TargetRelativeError << ' '
           << c.BoundaryEpsilon << '\n';
    return output.str();
}

bool Valid(const VGImportSettings& settings, std::string& error)
{
    if (settings.Quality > VGQuality::Custom || settings.StreamingPriority > 2 ||
        !std::isfinite(settings.PositionError) || settings.PositionError < 0 || !std::isfinite(settings.LODError) ||
        settings.LODError < 0)
    {
        error = "Invalid VirtualGeometry import settings";
        return false;
    }
    return ValidateVirtualGeometryConfig(settings.BuildConfig(), error);
}
} // namespace

bool Load(const std::filesystem::path& source, VGImportSettings& settings, std::string& error)
{
    error.clear();
    std::error_code ec;
    const auto path = Sidecar(source);
    if (!std::filesystem::exists(path, ec) && !ec)
    {
        settings = {};
        return true;
    }
    std::ifstream input(path);
    input.imbue(std::locale::classic());
    VGImportSettings parsed;
    std::string magic;
    uint32_t version = 0, quality = 0;
    auto& c = parsed.Custom;
    if (!(input >> magic >> version) || magic != "VGSETTINGS" || version != 1 ||
        !(input >> parsed.Enabled >> parsed.BackgroundBuild >> quality >> parsed.PositionError >> parsed.LODError >>
          parsed.StreamingPriority >> c.MaxClusterTriangles >> c.MaxClusterVertices >> c.TargetClustersPerGroup >>
          c.MaxHierarchyLevels >> c.SimplificationRatio >> c.ConeWeight >> c.NormalWeight >> c.TexCoordWeight >>
          c.ColorWeight >> c.TargetRelativeError >> c.BoundaryEpsilon))
    {
        error = "Invalid or truncated VirtualGeometry settings: " + path.generic_string();
        return false;
    }
    parsed.Quality = static_cast<VGQuality>(quality);
    if (!Valid(parsed, error))
    {
        return false;
    }
    settings = parsed;
    return true;
}

bool Save(const std::filesystem::path& source, const VGImportSettings& settings, std::string& error)
{
    error.clear();
    if (!Valid(settings, error))
    {
        return false;
    }
    const auto target = Sidecar(source);
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << Encode(settings);
        output.flush();
        if (!output)
        {
            error = "Cannot write VirtualGeometry settings";
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "Cannot publish VirtualGeometry settings";
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }
#endif
    return true;
}

uint64_t SettingsKey(const VGImportSettings& settings)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : Encode(settings))
    {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    return hash;
}
} // namespace VirtualGeometryImport
