#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <istream>
#include <ostream>
#include <string>
#include <utility>

namespace Visibility
{
inline constexpr uint32_t InvalidID = UINT32_MAX;
inline constexpr uint32_t MaxLODs = 8;
struct VisibleInstanceData
{
    uint32_t InstanceID, SelectedMeshID, SelectedLOD, State;
};
struct GPULODLevel
{
    uint32_t MeshIndex = InvalidID;
    float ScreenSizeThreshold = 0;
    float Hysteresis = 0.1f;
    uint32_t Padding = 0;
};
struct GPULODGroup
{
    uint32_t Offset = 0, Count = 0, Padding[2]{};
};
struct LODMetadata
{
    uint32_t Count = 5;
    std::array<float, MaxLODs> Thresholds{0.30f, 0.15f, 0.05f, 0.01f, 0, 0, 0, 0};
    float Hysteresis = 0.1f;
    std::array<std::string, MaxLODs> MeshPaths{}; // Empty uses the imported mesh's generated LOD.
};
struct Statistics
{
    uint32_t Total = 0, FrustumCulled = 0, FrustumVisible = 0, Tested = 0;
    uint32_t Occluded = 0, FinalVisible = 0, Triangles = 0, IndexVertices = 0;
    std::array<uint32_t, MaxLODs> LODCounts{};
};
static_assert(sizeof(VisibleInstanceData) == 16 && sizeof(GPULODLevel) == 16 && sizeof(GPULODGroup) == 16);
static_assert(sizeof(Statistics) == 64);

// Half-open source coverage. Floor-sized D3D mips need the odd edge folded in.
inline std::pair<uint32_t, uint32_t> ReductionRange(uint32_t pixel, uint32_t source, uint32_t destination)
{
    return {pixel * source / destination, ((pixel + 1) * source + destination - 1) / destination};
}
inline uint32_t SelectLOD(float size, const LODMetadata &metadata, uint32_t previous)
{
    const uint32_t count = std::clamp(metadata.Count, 1u, MaxLODs);
    if (!std::isfinite(size) || size < 0)
    {
        return 0;
    }
    uint32_t selected = count - 1;
    for (uint32_t level = 0; level < count; ++level)
    {
        if (size >= metadata.Thresholds[level])
        {
            selected = level;
            break;
        }
    }
    if (previous < count)
    {
        selected = previous;
        while (selected + 1 < count && size < metadata.Thresholds[selected] * (1 - metadata.Hysteresis))
        {
            ++selected;
        }
        while (selected > 0 && size >= metadata.Thresholds[selected - 1] * (1 + metadata.Hysteresis))
        {
            --selected;
        }
    }
    return selected;
}
inline uint32_t FallbackLOD(uint32_t selected, const std::array<uint32_t, MaxLODs> &meshes)
{
    for (int level = static_cast<int>((std::min)(selected, MaxLODs - 1)); level >= 0; --level)
    {
        if (meshes[level] != InvalidID)
        {
            return static_cast<uint32_t>(level);
        }
    }
    return InvalidID;
}
inline bool ValidLOD(const LODMetadata &value)
{
    if (!value.Count || value.Count > MaxLODs || !std::isfinite(value.Hysteresis) || value.Hysteresis < 0 ||
        value.Hysteresis > 0.5f)
    {
        return false;
    }
    float previous = 1;
    for (uint32_t i = 0; i < value.Count; ++i)
    {
        const float threshold = value.Thresholds[i];
        if (!std::isfinite(threshold) || threshold < 0 || threshold > previous)
        {
            return false;
        }
        previous = threshold;
    }
    return true;
}
inline bool SaveLOD(std::ostream &stream, const LODMetadata &value)
{
    if (!ValidLOD(value))
    {
        return false;
    }
    stream << "LOD 2 " << value.Count << ' ' << value.Hysteresis;
    for (float threshold : value.Thresholds)
    {
        stream << ' ' << threshold;
    }
    for (const auto &path : value.MeshPaths)
    {
        stream << ' ' << std::quoted(path);
    }
    stream << '\n';
    return bool(stream);
}
inline bool LoadLOD(std::istream &stream, LODMetadata &destination)
{
    std::string magic;
    uint32_t version = 0;
    LODMetadata value;
    if (!(stream >> magic >> version >> value.Count >> value.Hysteresis) || magic != "LOD" ||
        (version != 1 && version != 2))
    {
        return false;
    }
    for (float &threshold : value.Thresholds)
    {
        if (!(stream >> threshold))
        {
            return false;
        }
    }
    if (version == 2)
    {
        for (auto &path : value.MeshPaths)
        {
            if (!(stream >> std::quoted(path)))
            {
                return false;
            }
        }
    }
    if (!ValidLOD(value))
    {
        return false;
    }
    destination = value;
    return true;
}
} // namespace Visibility
