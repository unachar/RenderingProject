#pragma once
#include "virtualgeometryasset.h"
#include <memory>
#include <unordered_map>

namespace VirtualGeometryRuntime
{
    struct RuntimeAsset;
}

struct VirtualGeometryGPUCluster
{
    uint32_t VertexOffset = 0;
    uint32_t VertexCount = 0;
    uint32_t TriangleOffset = 0;
    uint32_t TriangleCount = 0;
    uint32_t Level = 0;
    uint32_t GroupIndex = UINT32_MAX;
    uint32_t GeneratingGroupIndex = UINT32_MAX;
    uint32_t MaterialId = 0;
    std::array<float, 4> Bounds{}, Cone{};
    float LodError = 0;
    uint32_t PageIndex = 0;
    uint32_t Flags = 0;
    uint32_t Padding = 0;
    std::array<float, 4> ConeApex{};
};

struct VirtualGeometryGPUGroup
{
    uint32_t FirstChild = 0;
    uint32_t ChildCount = 0;
    uint32_t FirstParent = 0;
    uint32_t ParentCount = 0;
    std::array<float, 4> Bounds{};
    float MinError = 0;
    float MaxError = 0;
    uint32_t PageIndex = 0;
    uint32_t Flags = 0;
    uint32_t Level = 0;
    uint32_t Padding[3]{};
};

struct VirtualGeometryGPUAsset
{
    uint32_t ClusterOffset = 0;
    uint32_t ClusterCount = 0;
    uint32_t GroupOffset = 0;
    uint32_t GroupCount = 0;
    uint32_t RootOffset = 0;
    uint32_t RootCount = 0;
    uint32_t VertexOffset = 0;
    uint32_t TriangleOffset = 0;
    uint32_t MaterialBase = 0;
    uint32_t Flags = 0;
    uint32_t HierarchyIndexOffset = 0;
    uint32_t HierarchyIndexCount = 0;
};

struct VisibleVirtualCluster
{
    // Phase 6 では Flags にアセット表のインデックスを格納する。ラスタライズ用フラグは後で扱う。
    uint32_t InstanceIndex = 0;
    uint32_t ClusterIndex = 0;
    uint32_t MaterialIndex = 0;
    uint32_t Flags = 0;
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
    // 確保はトランザクションとして行い、すべての参照をアセット相対のまま保つ。
    bool Append(const PackedAsset& asset, PackedAsset& global, VirtualGeometryGPUAsset& table, std::string& error);

    class AssetRegistry
    {
      public:
        uint32_t Register(std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset> asset, std::string& error);

        const PackedAsset& Geometry() const
        {
            return m_Geometry;
        }

        const std::vector<VirtualGeometryGPUAsset>& Table() const
        {
            return m_Table;
        }

      private:
        PackedAsset m_Geometry;
        std::vector<VirtualGeometryGPUAsset> m_Table;
        std::vector<std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset>> m_Owners;
        std::unordered_map<const VirtualGeometryRuntime::RuntimeAsset*, uint32_t> m_Ids;
    };
} // 名前空間 VirtualGeometryGPU の終わり。
