#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "GpuSceneCommon.hlsli"
#include "VirtualGeometryCommon.hlsli"
StructuredBuffer<GPUInstanceData> Instances : register(t30);
ByteAddressBuffer Materials : register(t31);
StructuredBuffer<VirtualGeometryGPUCluster> Clusters : register(t33);
StructuredBuffer<VirtualGeometryGPUAsset> Assets : register(t34);
ByteAddressBuffer Triangles : register(t35);
ByteAddressBuffer Vertices : register(t36);
StructuredBuffer<VisibleVirtualCluster> Visible : register(t37);
ByteAddressBuffer Statistics : register(t38);
cbuffer MeshDebug : register(b6) { uint LevelColors; }
struct PrimitiveOutput { bool Cull : SV_CullPrimitive; };
#ifdef VIRTUAL_GEOMETRY_VELOCITY
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
    uint address = asset.TriangleOffset + cluster.TriangleOffset + primitiveIndex*3;
    return uint3(VirtualGeometryByte(Triangles,address),VirtualGeometryByte(Triangles,address+1),VirtualGeometryByte(Triangles,address+2));
}
float3 NativeNormal(float3 normal, GPUInstanceData instance)
{
    float3 a = instance.World[0].xyz, b = instance.World[1].xyz, c = instance.World[2].xyz;
    float determinant = dot(a,cross(b,c));
    float3 transformed = normal.x*cross(b,c) + normal.y*cross(c,a) + normal.z*cross(a,b);
    if (abs(determinant) <= 1e-8 || dot(transformed,transformed) <= 1e-12) return float3(0,1,0);
    return normalize(transformed*sign(determinant));
}
[outputtopology("triangle")]
[numthreads(64,1,1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex,
    out vertices MeshVertex outputVertices[64], out indices uint3 outputTriangles[124],
    out primitives PrimitiveOutput primitiveOutput[124])
{
    uint count = Statistics.Load(4);
    uint index = group.x + group.y*min(count,65535u);
    VirtualGeometryGPUCluster cluster = (VirtualGeometryGPUCluster)0;
    VisibleVirtualCluster item = (VisibleVirtualCluster)0;
    if (index < count) { item = Visible[index]; cluster = Clusters[item.ClusterIndex]; }
    SetMeshOutputCounts(cluster.VertexCount,cluster.TriangleCount);
    if (index >= count) return;
    GPUInstanceData instance = Instances[item.InstanceIndex];
    VirtualGeometryGPUAsset asset = Assets[item.Flags];
    for (uint vertexIndex = lane; vertexIndex < cluster.VertexCount; vertexIndex += 64)
    {
        uint address = (asset.VertexOffset+cluster.VertexOffset+vertexIndex)*48;
        float3 position = asfloat(Vertices.Load3(address));
        MeshVertex vertex = (MeshVertex)0;
        float4 world = mul(float4(position,1),instance.World);
        float4 view = mul(world,View);
        vertex.Position = mul(view,Projection);
#ifdef VIRTUAL_GEOMETRY_VELOCITY
        vertex.CurrentClip = vertex.Position;
        vertex.PreviousClip = mul(mul(float4(position,1),instance.PreviousWorld),PreviousViewProjection);
        vertex.MaterialFlags = Materials.Load(item.MaterialIndex*GPU_SCENE_MATERIAL_STRIDE+40);
#else
        vertex.Normal = NativeNormal(asfloat(Vertices.Load3(address+12)),instance);
        vertex.TexCoord = asfloat(Vertices.Load2(address+24));
        vertex.Diffuse = asfloat(Vertices.Load4(address+32));
        if (LevelColors != 0) vertex.Diffuse.rgb = VirtualGeometryLevelColor(cluster.Level);
        vertex.WorldPos = world.xyz;
        vertex.ViewPos = view.xyz;
        vertex.CameraPos = CameraPos;
        vertex.InstanceID = item.InstanceIndex;
#endif
        outputVertices[vertexIndex] = vertex;
    }
    for (uint primitiveIndex = lane; primitiveIndex < cluster.TriangleCount; primitiveIndex += 64)
    {
        uint3 indices = TriangleIndices(asset,cluster,primitiveIndex);
        outputTriangles[primitiveIndex] = indices;
        float3 points[3];
        for (uint corner = 0; corner < 3; ++corner)
        {
            uint address = (asset.VertexOffset+cluster.VertexOffset+indices[corner])*48;
            float3 position = asfloat(Vertices.Load3(address));
            points[corner] = mul(mul(float4(position,1),instance.World),View).xyz;
        }
        primitiveOutput[primitiveIndex].Cull = dot(cross(points[1]-points[0],points[2]-points[0]),points[0]) >= 0;
    }
}
