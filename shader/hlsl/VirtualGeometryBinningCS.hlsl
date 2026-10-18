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
// 負荷削減：以前は 1 スレッドで全ビン（約 4097）を順番に処理しており GPU 1.8ms かかっていた。
// 1 グループ 1024 スレッドの並列スキャン（オフセットとアクティブ数の 2 回）に変更。結果（順序・値）は同じ。
//   クランプ：「クランプ済みの累積」= min(生の累積, Capacity) なので、生の累積から直接求められる
#define PREFIX_THREADS 1024
groupshared uint gs_Scan[PREFIX_THREADS];

uint ExclusiveScan(uint value, uint thread)
{
    gs_Scan[thread] = value;
    GroupMemoryBarrierWithGroupSync();
    // Brent-Kung inclusive scan: disjoint readers/writers within each stage.
    // 2036 shared additions instead of 10240, with no wave-size assumptions.
    [unroll] for (uint stride = 1; stride < PREFIX_THREADS; stride <<= 1)
    {
        uint index = (thread + 1) * (stride << 1) - 1;
        if (index < PREFIX_THREADS) gs_Scan[index] += gs_Scan[index - stride];
        GroupMemoryBarrierWithGroupSync();
    }
    [unroll] for (uint stride = PREFIX_THREADS >> 2; stride > 0; stride >>= 1)
    {
        uint index = (thread + 1) * (stride << 1) - 1;
        if (index + stride < PREFIX_THREADS) gs_Scan[index + stride] += gs_Scan[index];
        GroupMemoryBarrierWithGroupSync();
    }
    uint inclusive = gs_Scan[thread];
    // Every lane must finish reading before the next scan reuses this array.
    GroupMemoryBarrierWithGroupSync();
    return inclusive - value;
}

[numthreads(PREFIX_THREADS,1,1)]
void Prefix(uint thread : SV_GroupIndex)
{
    const uint binCount = ExactMaterialBins + 1;
    const uint perThread = (binCount + PREFIX_THREADS - 1) / PREFIX_THREADS;
    const uint first = min(thread * perThread, binCount);
    const uint last = min(first + perThread, binCount);

    // 1) 生の個数の累積
    uint rawSum = 0;
    for (uint bin = first; bin < last; ++bin) rawSum += Bins[bin].y;
    uint rawOffset = ExclusiveScan(rawSum, thread);

    // 2) クランプ済みの個数と、このスレッドのアクティブ（個数 > 0）なビン数
    uint activeLocal = 0;
    uint running = rawOffset;
    for (uint bin2 = first; bin2 < last; ++bin2)
    {
        uint count = Bins[bin2].y;
        uint clamped = min(running + count, Capacity) - min(running, Capacity);
        activeLocal += clamped != 0 ? 1u : 0u;
        running += count;
    }
    uint activeOffset = ExclusiveScan(activeLocal, thread);

    // 3) 書き込み（以前と同じ並び・同じ値）
    running = rawOffset;
    uint active = activeOffset;
    for (uint bin3 = first; bin3 < last; ++bin3)
    {
        uint raw = Bins[bin3].y;
        uint offset = min(running, Capacity);
        uint count = min(running + raw, Capacity) - offset;
        Bins[bin3] = uint4(offset, count, 0, bin3 < ExactMaterialBins ? bin3 : 0xffffffffu);
        if (count != 0)
        {
            uint address = active * 20;
            uint x = min(count, 65535u);
            Commands.Store2(address, uint2(offset, count));
            Commands.Store3(address + 8, uint3(x, (count + x - 1) / x, 1));
            ++active;
        }
        running += raw;
    }
    if (thread == PREFIX_THREADS - 1)
    {
        Counters.Store(0, active);
        Counters.Store(4, min(running, Capacity));
    }
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



