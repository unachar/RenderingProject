#include "GpuSceneCommon.hlsli"
StructuredBuffer<GPUInstanceData> gInstances : register(t0);
StructuredBuffer<uint4> gVisible : register(t1);
StructuredBuffer<uint4> gGeometry : register(t2);
RWByteAddressBuffer gCount : register(u1);
RWByteAddressBuffer gCommands : register(u0);
cbuffer Limits : register(b0) { uint Slots; uint Capacity; uint MeshCount; }
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index >= min(gCount.Load(0), Capacity)) return;
    uint4 visible = gVisible[index];
    uint instanceID = visible.x;
    uint offset = index * 24u;
    if (instanceID >= Slots || visible.y >= MeshCount)
    {
        uint unused;
        gCount.InterlockedAdd(8,1,unused);
        gCommands.Store4(offset,0);
        gCommands.Store2(offset+16u,0);
        return;
    }
    uint4 mesh = gGeometry[visible.y];
    // Root constant followed by the five DWORDs in D3D12_DRAW_INDEXED_ARGUMENTS.
    gCommands.Store4(offset, uint4(instanceID, mesh.y, 1, mesh.x));
    gCommands.Store2(offset + 16u, uint2(mesh.z, 0));
}
