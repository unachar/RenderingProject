#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "GpuSceneCommon.hlsli"
#include "VirtualGeometryCommon.hlsli"
#include "VGGeometryDecode.hlsli"
StructuredBuffer<GPUInstanceData> Instances : register(t30);
ByteAddressBuffer Materials : register(t31);
StructuredBuffer<VirtualGeometryGPUCluster> Clusters : register(t33);
StructuredBuffer<VirtualGeometryGPUAsset> Assets : register(t34);
ByteAddressBuffer Triangles : register(t35);
ByteAddressBuffer Vertices : register(t36);
StructuredBuffer<VisibleVirtualCluster> Visible : register(t37);
ByteAddressBuffer Statistics : register(t38);
StructuredBuffer<uint4> Residency : register(t39);
ByteAddressBuffer GeometryPages[16384] : register(t0, space2);

cbuffer MeshDebug : register(b6)
{
    uint LevelColors;
}
#ifdef VIRTUAL_GEOMETRY_BINNED
cbuffer MaterialBin : register(b7)
{
    uint NativeBinOffset, NativeBinCount;
}
#endif
struct PrimitiveOutput
{
    bool Cull : SV_CullPrimitive;
};
#ifdef VIRTUAL_GEOMETRY_SHADOW
struct MeshVertex
{
    float4 Position : SV_POSITION;
};
#elif defined(VIRTUAL_GEOMETRY_VELOCITY)
struct MeshVertex
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
    nointerpolation uint MaterialFlags : TEXCOORD2;
};
#else
#define MeshVertex PSInput3D
#endif
uint3 TriangleIndices(VirtualGeometryGPUAsset asset, VirtualGeometryGPUCluster cluster, uint primitiveIndex)
{
    if ((asset.Flags & 2u) != 0)
    {
        uint slot = Residency[asset.VertexOffset + cluster.PageIndex].x;
        ByteAddressBuffer page = GeometryPages[NonUniformResourceIndex(slot)];
        if ((cluster.Flags & 0x10000u) != 0)
        {
            return VGLoadTriangle(page, cluster.VertexOffset, primitiveIndex);
        }
        uint pageAddress = cluster.TriangleOffset + primitiveIndex * 3;
        return uint3(VirtualGeometryByte(page, pageAddress), VirtualGeometryByte(page, pageAddress + 1),
                     VirtualGeometryByte(page, pageAddress + 2));
    }
    uint address = asset.TriangleOffset + cluster.TriangleOffset + primitiveIndex * 3;
    return uint3(VirtualGeometryByte(Triangles, address), VirtualGeometryByte(Triangles, address + 1),
                 VirtualGeometryByte(Triangles, address + 2));
}

uint VertexAddress(VirtualGeometryGPUAsset asset, VirtualGeometryGPUCluster cluster, uint index)
{
    if ((asset.Flags & 2u) != 0)
    {
        return cluster.VertexOffset + index * 48;
    }
    return (asset.VertexOffset + cluster.VertexOffset + index) * 48;
}

uint3 VertexLoad3(VirtualGeometryGPUAsset asset, VirtualGeometryGPUCluster cluster, uint address)
{
    if ((asset.Flags & 2u) != 0)
    {
        uint slot = Residency[asset.VertexOffset + cluster.PageIndex].x;
        return GeometryPages[NonUniformResourceIndex(slot)].Load3(address);
    }
    return Vertices.Load3(address);
}

float3 NativeNormal(float3 normal, GPUInstanceData instance)
{
    float3 a = instance.World[0].xyz;
    float3 b = instance.World[1].xyz;
    float3 c = instance.World[2].xyz;
    float determinant = dot(a, cross(b, c));
    float3 transformed = normal.x * cross(b, c) + normal.y * cross(c, a) + normal.z * cross(a, b);
    if (abs(determinant) <= 1e-8 || dot(transformed, transformed) <= 1e-12)
    {
        return float3(0, 1, 0);
    }
    return normalize(transformed * sign(determinant));
}

float3 NativePosition(VirtualGeometryGPUAsset asset, VirtualGeometryGPUCluster cluster, uint index)
{
    if ((asset.Flags & 2u) != 0 && (cluster.Flags & 0x10000u) != 0)
    {
        uint slot = Residency[asset.VertexOffset + cluster.PageIndex].x;
        ByteAddressBuffer page = GeometryPages[NonUniformResourceIndex(slot)];
        return VGDecodePosition(page, cluster.VertexOffset, index);
    }
    return asfloat(VertexLoad3(asset, cluster, VertexAddress(asset, cluster, index)));
}

float3 NativeDebugColor(VirtualGeometryGPUAsset asset, VirtualGeometryGPUCluster cluster, VisibleVirtualCluster item,
                        GPUInstanceData instance)
{
    uint id = item.ClusterIndex;
    if (LevelColors == 1)
    {
        return VirtualGeometryLevelColor(cluster.Level);
    }
    if (LevelColors == 3)
    {
        id = cluster.GroupIndex;
    }
    if (LevelColors == 4)
    {
        id = item.MaterialIndex;
    }
    if (LevelColors == 5)
    {
        return (cluster.Flags & 0x10000u) != 0 ? float3(0.1, 0.9, 0.3) : float3(0.9, 0.3, 0.1);
    }
    if (LevelColors == 6)
    {
        if ((asset.Flags & 2u) == 0)
        {
            return float3(0, 0.9, 0.3);
        }
        uint page = asset.VertexOffset + cluster.PageIndex;
        bool resident = Residency[page].z != 0;
        if (!resident)
        {
            return float3(1, 0, 0);
        }
        if (cluster.Level > 0)
        {
            return float3(1, 0.6, 0);
        }
        return float3(0, 0.9, 0.3);
    }
    if (LevelColors == 7)
    {
        return VirtualGeometryLevelColor((asset.Flags >> 16) & 3u);
    }
    if (LevelColors == 8)
    {
        bool moving = false;
        for (uint row = 0; row < 4; ++row)
        {
            moving = moving || any(instance.World[row] != instance.PreviousWorld[row]);
        }
        return moving ? float3(1, 0.5, 0) : float3(0, 0.7, 1);
    }
    if (LevelColors == 9)
    {
        return float3(0.04, 0.01, 0);
    }
    if (LevelColors == 10)
    {
        float error = saturate(cluster.LodError / (cluster.Bounds.w + 0.00001));
        return float3(error, 1 - error, 0);
    }
    if (LevelColors == 11)
    {
        id = instance.Entity;
    }
    id = (id ^ 61u) ^ (id >> 16);
    id *= 9u;
    id ^= id >> 4;
    id *= 0x27d4eb2du;
    id ^= id >> 15;
    return float3(id & 255u, (id >> 8) & 255u, (id >> 16) & 255u) / 255.0;
}
[outputtopology("triangle")][numthreads(64, 1, 1)] void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex,
                                                             out vertices MeshVertex outputVertices[64],
                                                             out indices uint3 outputTriangles[124],
                                                             out primitives PrimitiveOutput primitiveOutput[124]) {
#ifdef VIRTUAL_GEOMETRY_BINNED
    uint count = NativeBinCount;
#else
    uint count = Statistics.Load(4);
#endif
    uint localIndex = group.x + group.y * min(count, 65535u);
    uint index = localIndex;
#ifdef VIRTUAL_GEOMETRY_BINNED
    index += NativeBinOffset;
#endif
    VirtualGeometryGPUCluster cluster = (VirtualGeometryGPUCluster)0;
    VisibleVirtualCluster item = (VisibleVirtualCluster)0;
    if (localIndex < count)
    {
        item = Visible[index];
        cluster = Clusters[item.ClusterIndex];
    }
    SetMeshOutputCounts(cluster.VertexCount, cluster.TriangleCount);
    if (localIndex >= count)
    {
        return;
    }
    GPUInstanceData instance = Instances[item.InstanceIndex];
    VirtualGeometryGPUAsset asset = Assets[item.Flags];
    for (uint vertexIndex = lane; vertexIndex < cluster.VertexCount; vertexIndex += 64)
    {
        uint address = VertexAddress(asset, cluster, vertexIndex);
        float3 position = NativePosition(asset, cluster, vertexIndex);
        MeshVertex vertex = (MeshVertex)0;
        float4 world = mul(float4(position, 1), instance.World);
        float4 view = mul(world, View);
        vertex.Position = mul(view, Projection);
#ifdef VIRTUAL_GEOMETRY_VELOCITY
        vertex.CurrentClip = vertex.Position;
        vertex.PreviousClip = mul(mul(float4(position, 1), instance.PreviousWorld), PreviousViewProjection);
        vertex.MaterialFlags = Materials.Load(item.MaterialIndex * GPU_SCENE_MATERIAL_STRIDE + 40);
#elif !defined(VIRTUAL_GEOMETRY_SHADOW)
        if ((asset.Flags & 2u) != 0)
        {
            uint slot = Residency[asset.VertexOffset + cluster.PageIndex].x;
            ByteAddressBuffer page = GeometryPages[NonUniformResourceIndex(slot)];
            if ((cluster.Flags & 0x10000u) != 0)
            {
                // 現在の PBR シェーダーは接線を使わないため、使用する属性だけを復号する。
                vertex.Normal = NativeNormal(VGDecodeNormal(page, cluster.VertexOffset, vertexIndex), instance);
                vertex.TexCoord = VGDecodeTexcoord(page, cluster.VertexOffset, vertexIndex, 0);
                vertex.Diffuse = VGDecodeColor(page, cluster.VertexOffset, vertexIndex);
            }
            else
            {
                vertex.Normal = NativeNormal(asfloat(page.Load3(address + 12)), instance);
                vertex.TexCoord = asfloat(page.Load2(address + 24));
                vertex.Diffuse = asfloat(page.Load4(address + 32));
            }
        }
        else
        {
            vertex.Normal = NativeNormal(asfloat(Vertices.Load3(address + 12)), instance);
            vertex.TexCoord = asfloat(Vertices.Load2(address + 24));
            vertex.Diffuse = asfloat(Vertices.Load4(address + 32));
        }
        if (LevelColors != 0)
        {
            vertex.Diffuse.rgb = NativeDebugColor(asset, cluster, item, instance);
        }
        vertex.WorldPos = world.xyz;
        vertex.ViewPos = view.xyz;
        vertex.CameraPos = CameraPos;
        vertex.InstanceID = item.InstanceIndex;
#endif
        outputVertices[vertexIndex] = vertex;
    }
    for (uint primitiveIndex = lane; primitiveIndex < cluster.TriangleCount; primitiveIndex += 64)
    {
        uint3 indices = TriangleIndices(asset, cluster, primitiveIndex);
        outputTriangles[primitiveIndex] = indices;
        float3 points[3];
        for (uint corner = 0; corner < 3; ++corner)
        {
            float3 position = NativePosition(asset, cluster, indices[corner]);
            points[corner] = mul(mul(float4(position, 1), instance.World), View).xyz;
        }
        primitiveOutput[primitiveIndex].Cull = dot(cross(points[1] - points[0], points[2] - points[0]), points[0]) >= 0;
    }
}
