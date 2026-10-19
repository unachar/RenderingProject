#include "virtualgeometry/virtualgeometryselection.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    VirtualGeometryAsset SharedGraph()
    {
        // 葉0と1から同じグループ内の親2と3を作り、それぞれの親から
        // 独立して根4と5を作る。片方の根だけを細分化した場合でも、
        // グループ0の片側だけを置き換えてはならない。
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

    void ExpectCut(const VirtualGeometrySelection::Result& result, std::vector<uint32_t> expected)
    {
        Check(result.Error.empty(), result.Error.c_str());
        auto actual = result.Clusters;
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        Check(actual == expected, "Incorrect hierarchy cut or duplicate coverage");
    }
} // 無名名前空間

void TestVirtualGeometrySelection()
{
    using namespace VirtualGeometrySelection;
    auto graph = SharedGraph();
    View view;
    view.ProjectionScalePixels = 10;
    Settings settings;
    ExpectCut(Select(graph, view, settings), {0, 1}); // 祖先を含めた整合性の維持により、遠方の根も細分化される。
    view.ProjectionScalePixels = 3;
    ExpectCut(Select(graph, view, settings), {2, 5}); // 異なる詳細度を混在させた、有効な選択結果。
    view.ProjectionScalePixels = 1;
    ExpectCut(Select(graph, view, settings), {4, 5});
    settings.ForceHierarchyLevel = 1;
    ExpectCut(Select(graph, view, settings), {2, 3});
    settings.ForceHierarchyLevel = 0;
    ExpectCut(Select(graph, view, settings), {0, 1});
    settings.ForceHierarchyLevel = 31;
    ExpectCut(Select(graph, view, settings), {4, 5});
    settings.ForceHierarchyLevel = -1;
    view.WorldScale = 100;
    ExpectCut(Select(graph, view, settings), {0, 1});
    view.WorldScale = 1;
    view.Camera = {0, 0, 10};
    ExpectCut(Select(graph, view, settings), {0, 1});

    VirtualGeometryAsset terminal;
    terminal.Clusters.resize(1);
    terminal.RootClusters = {0};
    ExpectCut(Select(terminal, view, settings), {0});

    auto corrupt = graph;
    corrupt.HierarchyIndices[0] = 100;
    Check(!Select(corrupt, view, settings).Error.empty(), "Invalid reference accepted");
    corrupt = graph;
    corrupt.Groups[1].Level = 0;
    Check(!Select(corrupt, view, settings).Error.empty(), "Nonascending dependency accepted");
    corrupt = graph;
    corrupt.Groups[0].MaxError = -1;
    Check(!Select(corrupt, view, settings).Error.empty(), "Negative error accepted");
    view.WorldScale = std::nanf("");
    Check(!Select(graph, view, settings).Error.empty(), "Invalid view accepted");
}
