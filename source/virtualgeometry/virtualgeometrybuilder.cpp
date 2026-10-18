#include "virtualgeometrybuilder.h"
#include "virtualgeometryinternal.h"
#include "virtualgeometryvalidator.h"
#include <chrono>
#include <algorithm>

VirtualGeometryBuildResult VirtualGeometryBuilder::Build(std::span<const VirtualGeometryVertex> vertices,
                                                         std::span<const uint32_t> indices,
                                                         const VirtualGeometryBuildConfig& config, uint32_t materialId)
{
    VirtualGeometryBuildResult result;
    if (!ValidateVirtualGeometryConfig(config, result.Error))
    {
        return result;
    }
    auto start = std::chrono::steady_clock::now();
    try
    {
        auto& asset = result.Asset;
        asset.Config = config;
        asset.SourceMaterialId = materialId;
        asset.Stats.SourceVertices = vertices.size();
        asset.Stats.SourceTriangles = indices.size() / 3;
        asset.Stats.SourceMemoryBytes = vertices.size_bytes() + indices.size_bytes();
        auto clean = VirtualGeometryInternal::CleanSource(vertices, indices, asset.Stats);
        auto leaves = VirtualGeometryInternal::AppendClusters(asset, vertices, clean, 0, 0);
        asset.Stats.LeafClusters = VirtualGeometryInternal::CheckedIndex(leaves.size());
        // Release source-sized temporary indices before hierarchy group temporaries.
        std::vector<uint32_t>().swap(clean);
        VirtualGeometryInternal::BuildHierarchy(asset, std::move(leaves));
        VirtualGeometryInternal::UpdateStatistics(asset);
        if (!VirtualGeometryValidator::Validate(asset, result.Error))
        {
            result.Asset = {};
            return result;
        }
        asset.Stats.BuildMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    catch (const std::exception& exception)
    {
        result.Asset = {};
        result.Error = exception.what();
    }
    return result;
}

void VirtualGeometryInternal::UpdateStatistics(VirtualGeometryAsset& asset)
{
    auto& stats = asset.Stats;
    stats.TotalClusters = CheckedIndex(asset.Clusters.size());
    stats.TotalGroups = CheckedIndex(asset.Groups.size());
    asset.Levels.clear();
    uint64_t triangles = 0;
    uint64_t vertices = 0;
    for (const auto& cluster : asset.Clusters)
    {
        if (asset.Levels.size() <= cluster.Level)
        {
            asset.Levels.resize(size_t(cluster.Level) + 1);
        }
        auto& level = asset.Levels[cluster.Level];
        ++level.Clusters;
        level.Triangles += cluster.TriangleCount;
        level.Vertices += cluster.VertexCount;
        triangles += cluster.TriangleCount;
        vertices += cluster.VertexCount;
        stats.MaximumTrianglesPerCluster = std::max(stats.MaximumTrianglesPerCluster, cluster.TriangleCount);
        stats.MaximumVerticesPerCluster = std::max(stats.MaximumVerticesPerCluster, cluster.VertexCount);
    }
    stats.HierarchyLevels = CheckedIndex(asset.Levels.size());
    stats.AverageTrianglesPerCluster = double(triangles) / stats.TotalClusters;
    stats.AverageVerticesPerCluster = double(vertices) / stats.TotalClusters;
    stats.RawMemoryBytes = uint64_t(asset.Vertices.size()) * sizeof(VirtualGeometryVertex) +
                           asset.TriangleIndices.size() +
                           uint64_t(asset.Clusters.size()) * sizeof(VirtualGeometryCluster) +
                           uint64_t(asset.Groups.size()) * sizeof(VirtualGeometryClusterGroup) +
                           uint64_t(asset.HierarchyIndices.size() + asset.RootClusters.size()) * sizeof(uint32_t) +
                           uint64_t(asset.Levels.size()) * sizeof(VirtualGeometryLevelStats);
}
