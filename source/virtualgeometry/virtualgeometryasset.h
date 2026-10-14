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
    string DebugName;
    vector<VirtualGeometryVertex> Vertices;
    vector<uint8_t> TriangleIndices;
    vector<VirtualGeometryCluster> Clusters;
    vector<VirtualGeometryClusterGroup> Groups;
    vector<uint32_t> HierarchyIndices;
    vector<uint32_t> RootClusters;
    vector<VirtualGeometryLevelStats> Levels;
    VirtualGeometryBuildStats Stats;
};

struct VirtualGeometryBuildResult
{
    VirtualGeometryAsset Asset;
    string Error;
    explicit operator bool() const { return Error.empty(); }
};
