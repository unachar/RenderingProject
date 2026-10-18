#pragma once
#include "virtualgeometry/virtualgeometryasset.h"
#include <algorithm>

inline VirtualGeometryAsset MakeSharedVirtualGeometryGraph()
{
    VirtualGeometryAsset asset;
    asset.Clusters.resize(6);
    asset.Groups.resize(3);
    asset.RootClusters = {4, 5};
    asset.HierarchyIndices = {0, 1, 2, 3, 2, 4, 3, 5};
    asset.Groups[0].FirstChild = 0;
    asset.Groups[0].ChildCount = 2;
    asset.Groups[0].FirstParent = 2;
    asset.Groups[0].ParentCount = 2;
    asset.Groups[0].MaxError = 2;
    asset.Groups[1].Level = asset.Groups[2].Level = 1;
    asset.Groups[1].FirstChild = 4;
    asset.Groups[1].FirstParent = 5;
    asset.Groups[2].FirstChild = 6;
    asset.Groups[2].FirstParent = 7;
    for (uint32_t group = 1; group < 3; ++group)
    {
        asset.Groups[group].ChildCount = asset.Groups[group].ParentCount = 1;
        asset.Groups[group].MaxError = 4;
    }
    for (uint32_t index = 0; index < 6; ++index)
    {
        asset.Clusters[index].Level = index / 2;
        asset.Clusters[index].LodError = float(index / 2) * 2;
    }
    asset.Clusters[0].GroupIndex = asset.Clusters[1].GroupIndex = 0;
    asset.Clusters[2].GroupIndex = 1;
    asset.Clusters[3].GroupIndex = 2;
    asset.Clusters[2].GeneratingGroupIndex = asset.Clusters[3].GeneratingGroupIndex = 0;
    asset.Clusters[4].GeneratingGroupIndex = 1;
    asset.Clusters[5].GeneratingGroupIndex = 2;
    for (auto& group : asset.Groups)
    {
        group.Bounds.Center = {0, 0, 10};
    }
    asset.Groups[2].Bounds.Center = {0, 0, 100};
    return asset;
}

inline VirtualGeometryAsset MakeBroadVirtualGeometryGraph(uint32_t leafCount)
{
    VirtualGeometryAsset asset;
    asset.Clusters.resize(leafCount);
    std::vector<uint32_t> current;
    for (uint32_t index = 0; index < leafCount; ++index)
    {
        current.push_back(index);
    }
    uint32_t level = 0;
    while (current.size() > 1)
    {
        std::vector<uint32_t> parents;
        for (size_t begin = 0; begin < current.size(); begin += 8)
        {
            uint32_t groupIndex = uint32_t(asset.Groups.size());
            VirtualGeometryClusterGroup group;
            group.Level = level;
            group.Bounds.Center = {0, 0, 10};
            group.MaxError = float(level + 1);
            group.FirstChild = uint32_t(asset.HierarchyIndices.size());
            group.ChildCount = uint32_t(std::min(size_t(8), current.size() - begin));
            for (uint32_t child = 0; child < group.ChildCount; ++child)
            {
                uint32_t index = current[begin + child];
                asset.Clusters[index].GroupIndex = groupIndex;
                asset.HierarchyIndices.push_back(index);
            }
            uint32_t parentIndex = uint32_t(asset.Clusters.size());
            group.FirstParent = uint32_t(asset.HierarchyIndices.size());
            group.ParentCount = 1;
            asset.HierarchyIndices.push_back(parentIndex);
            VirtualGeometryCluster parent;
            parent.Level = level + 1;
            parent.GeneratingGroupIndex = groupIndex;
            parent.LodError = group.MaxError;
            asset.Clusters.push_back(parent);
            asset.Groups.push_back(group);
            parents.push_back(parentIndex);
        }
        current = std::move(parents);
        ++level;
    }
    asset.RootClusters = std::move(current);
    return asset;
}
