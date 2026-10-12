#include "GpuSceneCommon.hlsli"
StructuredBuffer<GPUInstanceData> Instances : register(t0);
StructuredBuffer<GPUMeshData> Meshes : register(t1);
ByteAddressBuffer Materials : register(t2);
RWStructuredBuffer<uint> Visibility : register(u0);
cbuffer SceneCullConstants : register(b0)
{
    row_major float4x4 ViewProjection;
    uint InstanceCount;
    uint MeshCount;
    uint MaterialCount;
    uint EnableFrustum;
};
[numthreads(64, 1, 1)]
void main(uint3 dispatch : SV_DispatchThreadID)
{
    uint id = dispatch.x;
    if (id >= InstanceCount) return;
    GPUInstanceData instance = Instances[id];
    if (!(instance.Flags & 1u) || instance.MeshIndex >= MeshCount || instance.MaterialIndex >= MaterialCount)
    {
        Visibility[id] = 0;
        return;
    }
    // Validate the shared references on the GPU as well as the instance layout.
    GPUMeshData mesh = Meshes[instance.MeshIndex];
    uint parameterCount = Materials.Load(instance.MaterialIndex * GPU_SCENE_MATERIAL_STRIDE + 48u);
    if (!mesh.VertexCount || parameterCount > 160)
    {
        Visibility[id] = 0;
        return;
    }
    uint outside = 63u;
    [unroll]
    for (uint corner = 0; corner < 8; ++corner)
    {
        float3 signs = float3((corner & 1) ? 1 : -1, (corner & 2) ? 1 : -1, (corner & 4) ? 1 : -1);
        float4 position = mul(float4(instance.BoundsCenter.xyz + signs * instance.BoundsExtents.xyz, 1), ViewProjection);
        uint mask = (position.x < -position.w ? 1u : 0u) | (position.x > position.w ? 2u : 0u)
            | (position.y < -position.w ? 4u : 0u) | (position.y > position.w ? 8u : 0u)
            | (position.z < 0 ? 16u : 0u) | (position.z > position.w ? 32u : 0u);
        outside &= mask;
    }
    Visibility[id] = !EnableFrustum || !(instance.Flags & 2u) || outside == 0 ? 1u : 0u;
}
