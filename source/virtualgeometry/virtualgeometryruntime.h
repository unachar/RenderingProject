#pragma once
#include "virtualgeometryasset.h"
#include "virtualgeometryimport.h"
#include "rendering/meshlets/meshletbuilder.h"
#include <filesystem>
#include <memory>
#include <span>
#include <future>
namespace VirtualGeometryPageCache { struct PageManifest; }

namespace VirtualGeometryRuntime
{
struct RuntimeAsset
{
    VirtualGeometryAsset Hierarchy;
    Meshlets::Asset Meshlets;
    std::shared_ptr<const VirtualGeometryPageCache::PageManifest> Pages;
    uint64_t Key = 0;
    bool CacheHit = false;
    double BuildMilliseconds = 0;
    std::filesystem::path CachePath;
    std::string Error;
    std::string HierarchyWarning;
    VGImportSettings ImportSettings;
    VGCompressionSettings CompressionSettings;
};
struct Statistics
{
    uint64_t CacheHits = 0, Builds = 0, SharedHits = 0, Failures = 0;
    double BuildMilliseconds = 0;
};
std::shared_ptr<const RuntimeAsset> Acquire(
    std::span<const VirtualGeometryVertex> vertices, std::span<const uint32_t> indices, uint32_t materialId,
    float materialPartId, const std::filesystem::path &cacheDirectory = "asset/virtualgeometry-cache",
    const VGImportSettings& settings = {});
// Owns immutable input snapshots. Publication into meshes happens on the render thread.
std::shared_future<std::shared_ptr<const RuntimeAsset>> AcquireAsync(
    std::vector<VirtualGeometryVertex> vertices, std::vector<uint32_t> indices,
    uint32_t materialId, float materialPartId, const VGImportSettings& settings);
Statistics GetStatistics();
std::vector<std::shared_ptr<const RuntimeAsset>> LiveAssets();
} // namespace VirtualGeometryRuntime
