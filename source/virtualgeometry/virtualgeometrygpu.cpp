#include "virtualgeometrygpu.h"
#include "virtualgeometryvalidator.h"
#include "virtualgeometryruntime.h"
#include <exception>
#include <limits>

namespace VirtualGeometryGPU
{
uint32_t AssetRegistry::Register(std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset> asset, std::string& error)
{
    if (!asset || !asset->Error.empty())
    {
        error = "VirtualGeometry asset is unavailable";
        return UINT32_MAX;
    }
    const auto existing = m_Ids.find(asset.get());
    if (existing != m_Ids.end())
    {
        error.clear();
        return existing->second;
    }
    PackedAsset packed;
    if (!Pack(asset->Hierarchy, packed, error) || m_Table.size() >= UINT32_MAX)
    {
        return UINT32_MAX;
    }
    try
    {
        uint32_t id = uint32_t(m_Table.size());
        m_Table.reserve(m_Table.size() + 1);
        m_Owners.reserve(m_Owners.size() + 1);
        auto [entry, inserted] = m_Ids.emplace(asset.get(), id);
        VirtualGeometryGPUAsset table;
        if (!Append(packed, m_Geometry, table, error))
        {
            m_Ids.erase(entry);
            return UINT32_MAX;
        }
        m_Table.push_back(table);
        m_Owners.push_back(std::move(asset));
        return id;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return UINT32_MAX;
    }
}
namespace
{
std::array<float, 4> Sphere(const VirtualGeometrySphere& sphere)
{
    return {sphere.Center[0], sphere.Center[1], sphere.Center[2], sphere.Radius};
}
template<class T>
bool Fits(const std::vector<T>& existing, const std::vector<T>& added)
{
    return existing.size() <= UINT32_MAX && added.size() <= UINT32_MAX - existing.size();
}
template<class T>
void ReserveAppend(std::vector<T>& destination, size_t added)
{
    size_t required = destination.size()+added;
    if (required <= destination.capacity())
    {
        return;
    }
    size_t grown = destination.capacity()+destination.capacity()/2;
    destination.reserve(std::max(required,std::min(size_t(UINT32_MAX),grown)));
}
}

bool Pack(const VirtualGeometryAsset& asset, PackedAsset& output, std::string& error)
{
    output = {};
    if (!VirtualGeometryValidator::Validate(asset, error))
    {
        return false;
    }
    try
    {
        PackedAsset result;
        result.Clusters.reserve(asset.Clusters.size());
        for (const auto& cluster : asset.Clusters)
        {
            if (cluster.VertexCount > 64 || cluster.TriangleCount > 124 || asset.Vertices.size() > UINT32_MAX/48)
            {
                error = "Native VirtualGeometry exceeds Mesh Shader limits";
                return false;
            }
            VirtualGeometryGPUCluster record;
            record.VertexOffset = cluster.VertexOffset;
            record.VertexCount = cluster.VertexCount;
            record.TriangleOffset = cluster.TriangleOffset;
            record.TriangleCount = cluster.TriangleCount;
            record.Level = cluster.Level;
            record.GroupIndex = cluster.GroupIndex;
            record.GeneratingGroupIndex = cluster.GeneratingGroupIndex;
            record.MaterialId = cluster.MaterialId;
            record.Bounds = Sphere(cluster.Bounds);
            record.Cone = {cluster.Cone.Axis[0], cluster.Cone.Axis[1], cluster.Cone.Axis[2], cluster.Cone.Cutoff};
            record.ConeApex = {cluster.Cone.Apex[0], cluster.Cone.Apex[1], cluster.Cone.Apex[2], 0};
            record.LodError = cluster.LodError;
            result.Clusters.push_back(record);
        }
        for (const auto& group : asset.Groups)
        {
            VirtualGeometryGPUGroup record;
            record.FirstChild = group.FirstChild;
            record.ChildCount = group.ChildCount;
            record.FirstParent = group.FirstParent;
            record.ParentCount = group.ParentCount;
            record.Bounds = Sphere(group.Bounds);
            record.MinError = group.MinError;
            record.MaxError = group.MaxError;
            record.Level = group.Level;
            result.Groups.push_back(record);
        }
        result.HierarchyIndices = asset.HierarchyIndices;
        result.Roots = asset.RootClusters;
        result.Vertices = asset.Vertices;
        result.Triangles = asset.TriangleIndices;
        result.Triangles.resize((result.Triangles.size() + 3) & ~size_t(3), 0);
        output = std::move(result);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool Append(const PackedAsset& asset, PackedAsset& global, VirtualGeometryGPUAsset& table, std::string& error)
{
    if (!Fits(global.Clusters, asset.Clusters) || !Fits(global.Groups, asset.Groups) ||
        !Fits(global.HierarchyIndices, asset.HierarchyIndices) || !Fits(global.Roots, asset.Roots) ||
        !Fits(global.Vertices, asset.Vertices) || !Fits(global.Triangles, asset.Triangles) ||
        global.Triangles.size() % 4 != 0 || asset.Triangles.size() % 4 != 0 ||
        uint64_t(global.Vertices.size())+asset.Vertices.size() > UINT32_MAX/48)
    {
        error = "Native VirtualGeometry global range overflow/alignment failure";
        return false;
    }
    VirtualGeometryGPUAsset entry;
    entry.ClusterOffset = uint32_t(global.Clusters.size());
    entry.ClusterCount = uint32_t(asset.Clusters.size());
    entry.GroupOffset = uint32_t(global.Groups.size());
    entry.GroupCount = uint32_t(asset.Groups.size());
    entry.RootOffset = uint32_t(global.Roots.size());
    entry.RootCount = uint32_t(asset.Roots.size());
    entry.VertexOffset = uint32_t(global.Vertices.size());
    entry.TriangleOffset = uint32_t(global.Triangles.size());
    entry.HierarchyIndexOffset = uint32_t(global.HierarchyIndices.size());
    entry.HierarchyIndexCount = uint32_t(asset.HierarchyIndices.size());
    uint32_t maximumLevel = 0;
    for (const auto& cluster : asset.Clusters)
    {
        maximumLevel = std::max(maximumLevel,cluster.Level);
    }
    entry.Flags = 1u | (maximumLevel << 8);
    try
    {
        // Reserve every allocation before changing logical contents, so a
        // failed registration never exposes a partially appended asset.
        ReserveAppend(global.Clusters,asset.Clusters.size());
        ReserveAppend(global.Groups,asset.Groups.size());
        ReserveAppend(global.HierarchyIndices,asset.HierarchyIndices.size());
        ReserveAppend(global.Roots,asset.Roots.size());
        ReserveAppend(global.Vertices,asset.Vertices.size());
        ReserveAppend(global.Triangles,asset.Triangles.size());
        global.Clusters.insert(global.Clusters.end(), asset.Clusters.begin(), asset.Clusters.end());
        global.Groups.insert(global.Groups.end(), asset.Groups.begin(), asset.Groups.end());
        global.HierarchyIndices.insert(global.HierarchyIndices.end(), asset.HierarchyIndices.begin(), asset.HierarchyIndices.end());
        global.Roots.insert(global.Roots.end(), asset.Roots.begin(), asset.Roots.end());
        global.Vertices.insert(global.Vertices.end(), asset.Vertices.begin(), asset.Vertices.end());
        global.Triangles.insert(global.Triangles.end(), asset.Triangles.begin(), asset.Triangles.end());
        table = entry;
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
}
