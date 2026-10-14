#pragma once
#include "virtualgeometryasset.h"
#include "rendering/meshlets/meshletbuilder.h"
#include <filesystem>
#include <memory>
#include <span>

namespace VirtualGeometryRuntime
{
struct RuntimeAsset
{
    VirtualGeometryAsset Hierarchy;
    Meshlets::Asset Meshlets;
    uint64_t Key = 0;
    bool CacheHit = false;
    double BuildMilliseconds = 0;
    filesystem::path CachePath;
    string Error;
    string HierarchyWarning;
};
struct Statistics
{
    uint64_t CacheHits = 0, Builds = 0, SharedHits = 0, Failures = 0;
    double BuildMilliseconds = 0;
};
shared_ptr<const RuntimeAsset> Acquire(
    span<const VirtualGeometryVertex> vertices, span<const uint32_t> indices, uint32_t materialId,
    float materialPartId, const filesystem::path &cacheDirectory = "asset/virtualgeometry-cache");
Statistics GetStatistics();
vector<shared_ptr<const RuntimeAsset>> LiveAssets();
} // namespace VirtualGeometryRuntime
