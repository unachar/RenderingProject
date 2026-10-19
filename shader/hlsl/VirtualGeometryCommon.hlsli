#ifndef VIRTUAL_GEOMETRY_COMMON
#define VIRTUAL_GEOMETRY_COMMON

struct VirtualGeometryGPUCluster
{
    uint VertexOffset, VertexCount, TriangleOffset, TriangleCount;
    uint Level, GroupIndex, GeneratingGroupIndex, MaterialId;
    float4 Bounds, Cone;
    float LodError;
    uint PageIndex, Flags, Padding;
    float4 ConeApex;
};

struct VirtualGeometryGPUGroup
{
    uint FirstChild, ChildCount, FirstParent, ParentCount;
    float4 Bounds;
    float MinError, MaxError;
    uint PageIndex, Flags;
    uint Level;
    uint3 Padding;
};

struct VirtualGeometryGPUAsset
{
    uint ClusterOffset, ClusterCount, GroupOffset, GroupCount;
    uint RootOffset, RootCount, VertexOffset, TriangleOffset;
    uint MaterialBase, Flags, HierarchyIndexOffset, HierarchyIndexCount;
};

struct VisibleVirtualCluster
{
    // ラスタービンがない間、Flags はアセットテーブルのインデックスを保持する。
    uint InstanceIndex, ClusterIndex, MaterialIndex, Flags;
};

uint VirtualGeometryByte(ByteAddressBuffer triangles, uint address)
{
    return (triangles.Load(address & ~3u) >> ((address & 3u) * 8u)) & 255u;
}

float3 VirtualGeometryLevelColor(uint level)
{
    if (level == 0)
    {
        return float3(0.1, 0.9, 0.2);
    }
    if (level == 1)
    {
        return float3(0.1, 0.9, 0.9);
    }
    if (level == 2)
    {
        return float3(0.1, 0.3, 1.0);
    }
    if (level == 3)
    {
        return float3(1.0, 0.9, 0.1);
    }
    if (level == 4)
    {
        return float3(1.0, 0.45, 0.05);
    }
    return (level & 1u) != 0 ? float3(0.9, 0.1, 0.15) : float3(0.7, 0.1, 0.9);
}
#endif
