// =============================================================================================
// MegaLights M1 : クラスター（フロクセル）ライトグリッドの構築
//   1. ClearCounters  : 使用量カウンタを 0 に
//   2. TileSliceMask  : 16px タイルごとに「ジオメトリが存在するスライス」の 32bit マスク
//   3. CoarseCull     : 粗セル（4x4 タイル × 4 スライス）ごとに全ライトを判定 → 粗リスト
//   4. FineCull       : 細セル（1 タイル × 1 スライス）ごとに粗リストだけを判定 → 細リスト
//   ・ジオメトリが無いセルは判定しない（深度による 2.5D カリング）
//   ・セル内のライトはインデックス昇順（並びが決定論的になり、加算順も毎フレーム同じ）
//   実行時に DXC（cs_6_5）でコンパイルする。.vcxproj への追加は不要。
// =============================================================================================
#define MEGALIGHTS_CB_REGISTER b0
#include "MegaLightsCommon.hlsli"

#define ML_MAX_LIGHTS     4096u
#define ML_FINE_CHUNK     256u

Texture2D<float> SceneDepth : register(t0);
StructuredBuffer<MegaLightData> Lights : register(t1);

RWStructuredBuffer<uint>  TileSliceMasks : register(u0);
RWStructuredBuffer<uint2> CoarseCells    : register(u1);
RWStructuredBuffer<uint>  CoarseIndices  : register(u2);
RWStructuredBuffer<uint2> FineCells      : register(u3);
RWStructuredBuffer<uint>  FineIndices    : register(u4);
RWByteAddressBuffer       Counters       : register(u5); // 0: 粗の使用数, 4: 細の使用数, 8: あふれたセル数, 12: セル内の最大ライト数

uint CoarseCellIndex(uint3 coarse)
{
    return (coarse.z * MLScreen.w + coarse.y) * MLScreen.z + coarse.x;
}

[numthreads(1, 1, 1)]
void ClearCounters()
{
    Counters.Store4(0, uint4(0u, 0u, 0u, 0u));
}

// --------------------------------------------------------------------------------------------
groupshared uint gs_TileMask;

[numthreads(16, 16, 1)]
void TileSliceMask(uint3 groupId : SV_GroupID, uint3 pixel : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    if (groupIndex == 0u) gs_TileMask = 0u;
    GroupMemoryBarrierWithGroupSync();
    const bool volumetric = (MLGrid.w & ML_GRID_VOLUMETRIC) != 0u;
    if (all(pixel.xy < MLScreen.xy))
    {
        float depth = SceneDepth.Load(int3(pixel.xy, 0));
        if (volumetric && !MLIsGeometryDepth(depth))
        {
            // M6：空のピクセルはフォグが全スライスで見える
            InterlockedOr(gs_TileMask, 0xffffffffu);
        }
        else if (MLIsGeometryDepth(depth))
        {
            // ライティング側と同じ式でスライスを求め、境界の丸め誤差に備えて隣のスライスも立てる
            float viewZ = MLViewZFromDepth(depth);
            uint slice = MLSliceFromViewZ(viewZ);
            uint sliceLow = MLSliceFromViewZ(viewZ * 0.999f);
            uint sliceHigh = MLSliceFromViewZ(viewZ * 1.001f);
            uint first = min(slice, sliceLow);
            uint last = max(slice, sliceHigh);
            // M6（フォグ用）：カメラから面までのスライスはすべてフォグとして見える
            if (volumetric) first = 0u;
            uint bits = last >= 31u && first == 0u ? 0xffffffffu : ((2u << (last - first)) - 1u) << first;
            InterlockedOr(gs_TileMask, bits);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u) TileSliceMasks[groupId.y * MLGrid.x + groupId.x] = gs_TileMask;
}

// --------------------------------------------------------------------------------------------
groupshared uint gs_CoarseMask;
groupshared uint gs_HitBits[2];
groupshared uint gs_ListCount;
groupshared uint gs_ListOffset;
groupshared uint gs_List[ML_MAX_LIGHTS];

[numthreads(64, 1, 1)]
void CoarseCull(uint3 coarse : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    if (groupIndex == 0u)
    {
        gs_CoarseMask = 0u;
        gs_ListCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex < ML_COARSE_TILES * ML_COARSE_TILES)
    {
        uint2 tile = coarse.xy * ML_COARSE_TILES + uint2(groupIndex % ML_COARSE_TILES, groupIndex / ML_COARSE_TILES);
        if (all(tile < MLGrid.xy)) InterlockedOr(gs_CoarseMask, TileSliceMasks[tile.y * MLGrid.x + tile.x]);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint coarseIndex = CoarseCellIndex(coarse);
    const uint sliceBits = (gs_CoarseMask >> (coarse.z * ML_COARSE_SLICES)) & ((1u << ML_COARSE_SLICES) - 1u);
    if (sliceBits == 0u)
    {
        if (groupIndex == 0u) CoarseCells[coarseIndex] = uint2(0u, 0u);
        return;
    }

    // ジオメトリのあるスライスだけに絞った範囲で判定する
    const uint sliceMin = coarse.z * ML_COARSE_SLICES + firstbitlow(sliceBits);
    const uint sliceMax = coarse.z * ML_COARSE_SLICES + firstbithigh(sliceBits);
    const uint2 tileMin = coarse.xy * ML_COARSE_TILES;
    const uint2 tileMax = min(tileMin + (ML_COARSE_TILES - 1u), MLGrid.xy - 1u);
    float3 boundsMin, boundsMax;
    MLCellBounds(tileMin, tileMax, sliceMin, sliceMax, boundsMin, boundsMax);

    const uint lightCount = min(MLCounts.x, ML_MAX_LIGHTS);
    for (uint base = 0u; base < lightCount; base += 64u)
    {
        if (groupIndex < 2u) gs_HitBits[groupIndex] = 0u;
        GroupMemoryBarrierWithGroupSync();
        const uint lightIndex = base + groupIndex;
        bool hit = false;
        if (lightIndex < lightCount)
        {
            hit = MLSphereIntersectsAabb(MLLightViewSphere(Lights[lightIndex]), boundsMin, boundsMax);
        }
        if (hit) InterlockedOr(gs_HitBits[groupIndex >> 5u], 1u << (groupIndex & 31u));
        GroupMemoryBarrierWithGroupSync();
        const uint low = gs_HitBits[0];
        const uint high = gs_HitBits[1];
        if (hit)
        {
            // インデックス昇順に詰める（アトミックの到着順に依存しない）
            const uint rank = groupIndex < 32u
                ? countbits(low & ((1u << groupIndex) - 1u))
                : countbits(low) + countbits(high & ((1u << (groupIndex - 32u)) - 1u));
            gs_List[gs_ListCount + rank] = lightIndex;
        }
        GroupMemoryBarrierWithGroupSync();
        if (groupIndex == 0u) gs_ListCount += countbits(low) + countbits(high);
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex == 0u)
    {
        const uint count = gs_ListCount;
        uint offset = 0u;
        uint stored = 0u;
        if (count > 0u)
        {
            Counters.InterlockedAdd(0, count, offset);
            const uint capacity = MLCounts.w;
            stored = offset >= capacity ? 0u : min(count, capacity - offset);
            if (stored < count)
            {
                uint ignored;
                Counters.InterlockedAdd(8, 1u, ignored);
            }
        }
        gs_ListOffset = offset;
        gs_ListCount = stored;
        CoarseCells[coarseIndex] = uint2(offset, stored);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = groupIndex; k < gs_ListCount; k += 64u)
    {
        CoarseIndices[gs_ListOffset + k] = gs_List[k];
    }
}

// --------------------------------------------------------------------------------------------
// 粗セル 1 つ = 4x4 タイル × 4 スライス = 64 細セル = 64 スレッド
groupshared float4 gs_Spheres[ML_FINE_CHUNK];
groupshared uint gs_Ids[ML_FINE_CHUNK];

[numthreads(64, 1, 1)]
void FineCull(uint3 coarse : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    const uint2 coarseCell = CoarseCells[CoarseCellIndex(coarse)];
    const uint2 tile = coarse.xy * ML_COARSE_TILES + uint2(groupIndex % ML_COARSE_TILES, (groupIndex / ML_COARSE_TILES) % ML_COARSE_TILES);
    const uint slice = coarse.z * ML_COARSE_SLICES + groupIndex / (ML_COARSE_TILES * ML_COARSE_TILES);
    const bool valid = all(tile < MLGrid.xy) && slice < MLGrid.z;
    bool occupied = false;
    if (valid && coarseCell.y > 0u)
    {
        occupied = ((TileSliceMasks[tile.y * MLGrid.x + tile.x] >> slice) & 1u) != 0u;
    }
    float3 boundsMin = 0.0f;
    float3 boundsMax = 0.0f;
    if (occupied) MLCellBounds(tile, tile, slice, slice, boundsMin, boundsMax);

    // 1 周目：数える
    uint count = 0u;
    for (uint base = 0u; base < coarseCell.y; base += ML_FINE_CHUNK)
    {
        const uint chunk = min(ML_FINE_CHUNK, coarseCell.y - base);
        for (uint load = groupIndex; load < chunk; load += 64u)
        {
            const uint id = CoarseIndices[coarseCell.x + base + load];
            gs_Ids[load] = id;
            gs_Spheres[load] = MLLightViewSphere(Lights[id]);
        }
        GroupMemoryBarrierWithGroupSync();
        if (occupied)
        {
            for (uint k = 0u; k < chunk; ++k)
            {
                count += MLSphereIntersectsAabb(gs_Spheres[k], boundsMin, boundsMax) ? 1u : 0u;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    uint offset = 0u;
    uint stored = 0u;
    if (count > 0u)
    {
        Counters.InterlockedAdd(4, count, offset);
        const uint capacity = MLCounts.z;
        stored = offset >= capacity ? 0u : min(count, capacity - offset);
        uint ignored;
        if (stored < count) Counters.InterlockedAdd(8, 1u, ignored);
        Counters.InterlockedMax(12, count, ignored);
    }

    // 2 周目：書く（粗リストの順 = インデックス昇順）
    uint written = 0u;
    for (uint base2 = 0u; base2 < coarseCell.y; base2 += ML_FINE_CHUNK)
    {
        const uint chunk = min(ML_FINE_CHUNK, coarseCell.y - base2);
        for (uint load = groupIndex; load < chunk; load += 64u)
        {
            const uint id = CoarseIndices[coarseCell.x + base2 + load];
            gs_Ids[load] = id;
            gs_Spheres[load] = MLLightViewSphere(Lights[id]);
        }
        GroupMemoryBarrierWithGroupSync();
        if (occupied)
        {
            for (uint k = 0u; k < chunk && written < stored; ++k)
            {
                if (MLSphereIntersectsAabb(gs_Spheres[k], boundsMin, boundsMax))
                {
                    FineIndices[offset + written] = gs_Ids[k];
                    ++written;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (valid) FineCells[MLCellIndex(tile, slice)] = uint2(offset, stored);
}
