#include "GpuSceneCommon.hlsli"
StructuredBuffer<GPUInstanceData> gInstances : register(t0);
StructuredBuffer<uint> gVisibility : register(t1);
StructuredBuffer<uint4> gGeometry : register(t2);
RWStructuredBuffer<uint> gVisible : register(u0);
RWByteAddressBuffer gCount : register(u1);
cbuffer Limits : register(b0) { uint Slots; uint Capacity; uint MeshCount; }
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint slot = id.x;
    if (slot >= Slots) return;
    GPUInstanceData instance = gInstances[slot];
    // bit 2 is set only for supported static opaque instances.
    if ((instance.Flags & 5u) != 5u || gVisibility[slot] == 0 || instance.MeshIndex >= MeshCount) return;
    uint4 mesh = gGeometry[instance.MeshIndex];
    if (mesh.w == 0 || mesh.y == 0) return;
    uint target;
    gCount.InterlockedAdd(0, 1, target);
    // Capacity >= slot capacity guarantees this condition for every append.
    if (target < Capacity) gVisible[target] = slot;
}
