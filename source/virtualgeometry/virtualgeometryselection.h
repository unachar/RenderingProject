#pragma once
#include "virtualgeometryasset.h"
#include <span>

namespace VirtualGeometrySelection
{
struct View
{
    std::array<float, 3> Camera{};   // Object-space camera.
    float WorldScale = 1;            // Conservative maximum scale of the object transform.
    float MinimumWorldScale = 1;     // Conservative lower bound on its smallest singular value.
    float ProjectionScalePixels = 1; // 0.5 * viewport height * projection._22.
};

struct Settings
{
    float TargetErrorPixels = 1;
    int ForceHierarchyLevel = -1;
};

struct Result
{
    std::vector<uint32_t> Clusters;
    std::string Error;
};

// CPU reference for validation; runtime selection is performed on the GPU.
Result Select(const VirtualGeometryAsset& asset, const View& view, const Settings& settings);

struct ResidentResult : Result
{
    std::vector<uint32_t> RequestedPages;
};

struct TemporalResult : ResidentResult
{
    std::vector<uint8_t> DesiredRefine;
};

TemporalResult SelectResidentTemporal(const VirtualGeometryAsset& asset, const View& view, const Settings& settings,
                                      std::span<const uint32_t> clusterPages, std::span<const uint8_t> residentPages,
                                      std::span<const uint8_t> previousRefine, bool historyValid, float fraction);
ResidentResult SelectResident(const VirtualGeometryAsset& asset, const View& view, const Settings& settings,
                              std::span<const uint32_t> clusterPages, std::span<const uint8_t> residentPages);
} // namespace VirtualGeometrySelection
