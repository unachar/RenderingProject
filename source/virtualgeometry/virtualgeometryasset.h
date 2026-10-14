#pragma once
#include "virtualgeometrytypes.h"
#include <vector>
#include <string>

struct VirtualGeometryAsset
{
    uint32_t Version = VirtualGeometryConfig::AssetVersion;
    VirtualGeometryBuildConfig Config;
    uint32_t SourceMaterialId = 0;
    float SourceMaterialPartId = 0;
    std::string DebugName;
    std::vector<VirtualGeometryVertex> Vertices;
    std::vector<uint8_t> TriangleIndices;
    std::vector<VirtualGeometryCluster> Clusters;
    std::vector<VirtualGeometryClusterGroup> Groups;
    std::vector<uint32_t> HierarchyIndices;
    std::vector<uint32_t> RootClusters;
    std::vector<VirtualGeometryLevelStats> Levels;
    VirtualGeometryBuildStats Stats;
};

struct VirtualGeometryBuildResult
{
    VirtualGeometryAsset Asset;
    std::string Error;
    explicit operator bool() const { return Error.empty(); }
};
