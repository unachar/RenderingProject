#pragma once
#include "virtualgeometryasset.h"
#include <memory>
#include <unordered_map>

namespace VirtualGeometryRuntime { struct RuntimeAsset; }

struct VirtualGeometryGPUCluster
{
    uint32_t VertexOffset = 0, VertexCount = 0, TriangleOffset = 0, TriangleCount = 0;
    uint32_t Level = 0, GroupIndex = UINT32_MAX, GeneratingGroupIndex = UINT32_MAX, MaterialId = 0;
    std::array<float, 4> Bounds{}, Cone{};
    float LodError = 0;
    uint32_t PageIndex = 0, Flags = 0, Padding = 0;
    std::array<float, 4> ConeApex{};
};
struct VirtualGeometryGPUGroup
{
    uint32_t FirstChild = 0, ChildCount = 0, FirstParent = 0, ParentCount = 0;
    std::array<float, 4> Bounds{};
    float MinError = 0, MaxError = 0;
    uint32_t PageIndex = 0, Flags = 0;
    uint32_t Level = 0, Padding[3]{};
};
struct VirtualGeometryGPUAsset
{
    uint32_t ClusterOffset = 0, ClusterCount = 0, GroupOffset = 0, GroupCount = 0;
    uint32_t RootOffset = 0, RootCount = 0, VertexOffset = 0, TriangleOffset = 0;
    uint32_t MaterialBase = 0, Flags = 0, HierarchyIndexOffset = 0, HierarchyIndexCount = 0;
};
struct VisibleVirtualCluster
{
    // Flags carries the asset-table index in Phase 6; raster flags are deferred.
    uint32_t InstanceIndex = 0, ClusterIndex = 0, MaterialIndex = 0, Flags = 0;
};
static_assert(sizeof(VirtualGeometryGPUCluster) == 96);
static_assert(offsetof(VirtualGeometryGPUCluster, ConeApex) == 80);
static_assert(sizeof(VirtualGeometryGPUGroup) == 64);
static_assert(offsetof(VirtualGeometryGPUGroup, Level) == 48);
static_assert(sizeof(VirtualGeometryGPUAsset) == 48);
static_assert(sizeof(VisibleVirtualCluster) == 16);

namespace VirtualGeometryGPU
{
struct PackedAsset
{
    std::vector<VirtualGeometryGPUCluster> Clusters;
    std::vector<VirtualGeometryGPUGroup> Groups;
    std::vector<uint32_t> HierarchyIndices, Roots;
    std::vector<VirtualGeometryVertex> Vertices;
    std::vector<uint8_t> Triangles;
};
bool Pack(const VirtualGeometryAsset& asset, PackedAsset& output, std::string& error);
// Allocation is transactional; all references remain asset relative.
bool Append(const PackedAsset& asset, PackedAsset& global, VirtualGeometryGPUAsset& table, std::string& error);
class AssetRegistry
{
public:
    uint32_t Register(std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset> asset, std::string& error);
    const PackedAsset& Geometry() const { return m_Geometry; }
    const std::vector<VirtualGeometryGPUAsset>& Table() const { return m_Table; }
private:
    PackedAsset m_Geometry;
    std::vector<VirtualGeometryGPUAsset> m_Table;
    std::vector<std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset>> m_Owners;
    std::unordered_map<const VirtualGeometryRuntime::RuntimeAsset*, uint32_t> m_Ids;
};
}
