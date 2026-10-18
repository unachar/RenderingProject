#include "virtualgeometryreport.h"
#include <fstream>
#include <sstream>
#include <locale>

std::string VirtualGeometryReport::Text(const VirtualGeometryAsset& asset)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    const auto& stats = asset.Stats;
    output << "Nanite-like Virtualized Geometry Phase 1\n"
           << "Name: " << asset.DebugName << "\nVersion: " << asset.Version
           << "\nMaterial section: " << asset.SourceMaterialId << "\nMaterialPartId: " << asset.SourceMaterialPartId
           << "\nSource vertices: " << stats.SourceVertices << "\nSource triangles: " << stats.SourceTriangles
           << "\nRemoved degenerate triangles: " << stats.RemovedDegenerateTriangles
           << "\nRemoved duplicate triangles: " << stats.RemovedDuplicateTriangles
           << "\nFinal leaf triangles: " << stats.FinalLeafTriangles << "\nLeaf clusters: " << stats.LeafClusters
           << "\nTotal clusters: " << stats.TotalClusters << "\nTotal groups: " << stats.TotalGroups
           << "\nHierarchy levels: " << stats.HierarchyLevels << "\nRoots: " << asset.RootClusters.size()
           << "\nAverage triangles/cluster: " << stats.AverageTrianglesPerCluster
           << "\nAverage vertices/cluster: " << stats.AverageVerticesPerCluster
           << "\nMaximum triangles/cluster: " << stats.MaximumTrianglesPerCluster
           << "\nMaximum vertices/cluster: " << stats.MaximumVerticesPerCluster
           << "\nSource memory bytes: " << stats.SourceMemoryBytes
           << "\nRaw geometry memory bytes: " << stats.RawMemoryBytes
           << "\nBuild milliseconds (0 after reload): " << stats.BuildMilliseconds << '\n';
    for (size_t level = 0; level < asset.Levels.size(); ++level)
    {
        const auto& data = asset.Levels[level];
        output << "Level " << level << ": clusters=" << data.Clusters << " triangles=" << data.Triangles
               << " vertices=" << data.Vertices << '\n';
    }
    for (size_t index = 0; index < asset.Groups.size(); ++index)
    {
        const auto& group = asset.Groups[index];
        output << "Group " << index << " level=" << group.Level << " errors=" << group.MinError << ',' << group.MaxError
               << " children:";
        for (uint32_t child = 0; child < group.ChildCount; ++child)
        {
            output << ' ' << asset.HierarchyIndices.at(size_t(group.FirstChild) + child);
        }
        output << " parents:";
        for (uint32_t parent = 0; parent < group.ParentCount; ++parent)
        {
            output << ' ' << asset.HierarchyIndices.at(size_t(group.FirstParent) + parent);
        }
        output << '\n';
    }
    output << "Root clusters:";
    for (uint32_t root : asset.RootClusters)
    {
        output << ' ' << root;
    }
    output << '\n';
    return output.str();
}

bool VirtualGeometryReport::Save(const std::filesystem::path& path, const VirtualGeometryAsset& asset,
                                 std::string& error)
{
    error.clear();
    try
    {
        std::ofstream output(path);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << Text(asset);
        output.close();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
