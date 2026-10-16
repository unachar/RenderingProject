#include "VirtualGeometryCommon.hlsli"

StructuredBuffer<VisibleVirtualCluster> InputClusters : register(t0);
ByteAddressBuffer InputCounters : register(t1);
RWStructuredBuffer<uint4> Bins : register(u0); // offset, count, scatter cursor, material
RWStructuredBuffer<VisibleVirtualCluster> SortedClusters : register(u1);
RWByteAddressBuffer Commands : register(u2);
RWByteAddressBuffer Counters : register(u3);
cbuffer BinningParameters : register(b0)
{
    uint Capacity, ExactMaterialBins, MaterialCount, Padding;
};

uint InputCount()
{
    return min(InputCounters.Load(4),Capacity);
}
uint MaterialBin(uint material)
{
    return material < ExactMaterialBins ? material : ExactMaterialBins;
}

[numthreads(64,1,1)]
void Reset(uint3 id : SV_DispatchThreadID)
{
    if (id.x <= ExactMaterialBins) Bins[id.x] = 0;
    if (id.x < 8) Counters.Store(id.x*4,0);
}

[numthreads(64,1,1)]
void Count(uint3 id : SV_DispatchThreadID)
{
    if (id.x == 0 && InputCounters.Load(4) > Capacity)
    {
        uint unused;
        Counters.InterlockedAdd(16,InputCounters.Load(4)-Capacity,unused);
    }
    if (id.x >= InputCount()) return;
    uint material = InputClusters[id.x].MaterialIndex;
    uint bin = MaterialBin(material);
    uint unused;
    InterlockedAdd(Bins[bin].y,1,unused);
    if (bin == ExactMaterialBins) Counters.InterlockedAdd(8,1,unused);
    if (material >= MaterialCount) Counters.InterlockedAdd(12,1,unused);
}

// At most 4096 exact bins by default. One ordered scan creates compact commands
// without CPU readback or an entry for every empty material in the scene.
[numthreads(1,1,1)]
void Prefix()
{
    uint offset = 0, active = 0;
    for (uint bin = 0; bin <= ExactMaterialBins; ++bin)
    {
        uint count = min(Bins[bin].y,Capacity-offset);
        Bins[bin] = uint4(offset,count,0,bin < ExactMaterialBins ? bin : 0xffffffffu);
        if (count != 0 && active <= ExactMaterialBins)
        {
            uint address = active*20;
            uint x = min(count,65535u);
            Commands.Store2(address,uint2(offset,count));
            Commands.Store3(address+8,uint3(x,(count+x-1)/x,1));
            ++active;
        }
        offset += count;
    }
    Counters.Store(0,active);
    Counters.Store(4,offset);
}

[numthreads(64,1,1)]
void Scatter(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= InputCount()) return;
    VisibleVirtualCluster record = InputClusters[id.x];
    uint bin = MaterialBin(record.MaterialIndex);
    uint cursor;
    InterlockedAdd(Bins[bin].z,1,cursor);
    uint destination = Bins[bin].x+cursor;
    if (cursor >= Bins[bin].y || Bins[bin].x >= Capacity || cursor >= Capacity-Bins[bin].x)
    {
        uint unused;
        Counters.InterlockedAdd(24,1,unused);
        return;
    }
    SortedClusters[destination] = record;
}

