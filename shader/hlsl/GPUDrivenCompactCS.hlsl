#include "GpuSceneCommon.hlsli"
StructuredBuffer<GPUInstanceData> gInstances : register(t0);
StructuredBuffer<uint4> gVisibility : register(t1);
StructuredBuffer<uint4> gGeometry : register(t2);
RWStructuredBuffer<uint4> gVisible : register(u0);
RWByteAddressBuffer gCount : register(u1);

cbuffer Limits : register(b0)
{
    uint Slots;
    uint Capacity;
    uint MeshCount;
}
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint slot = id.x;
    if (slot >= Slots)
    {
        return;
    }
    GPUInstanceData instance = gInstances[slot];
    // bit 2 は対応する静的な不透明インスタンスでのみ設定する。
    uint4 visible = gVisibility[slot];
    if ((instance.Flags & 5u) != 5u || visible.w != 1 || visible.y >= MeshCount)
    {
        return;
    }
    uint4 mesh = gGeometry[visible.y];
    if (mesh.w == 0 || mesh.y == 0)
    {
        return;
    }
    uint target = gCount.Load(0), observed;
    for (;;)
    {
        if (target >= Capacity)
        {
            uint unused;
            gCount.InterlockedAdd(4, 1, unused);
            return;
        }
        gCount.InterlockedCompareExchange(0, target, target + 1, observed);
        if (observed == target)
        {
            break;
        }
        target = observed;
    }
    gVisible[target] = visible;
}

