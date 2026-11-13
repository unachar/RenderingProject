struct LightInstanceInput
{
    uint4 TileBounds;
    uint4 Metadata;
};

StructuredBuffer<LightInstanceInput> Instances : register(t0);
RWStructuredBuffer<uint> TileLightIndices : register(u0);
RWStructuredBuffer<uint> VolumetricLightIndices : register(u1);
RWByteAddressBuffer VolumetricCounter : register(u2);

cbuffer BuilderConstants : register(b0)
{
    uint LightCount;
    uint TileCountX;
    uint TileCountY;
    uint SlotsPerTile;
    uint BuildMode;
    uint MaximumVolumetricLights;
    uint TileStride;
    uint Padding;
};

groupshared LightInstanceInput g_Instances[160];

[numthreads(64, 1, 1)] void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    uint tileElementCount = TileCountX * TileCountY * TileStride;
    if (BuildMode == 0u)
    {
        if (index < tileElementCount)
        {
            TileLightIndices[index] = 0xffffffffu;
        }
        if (index < MaximumVolumetricLights)
        {
            VolumetricLightIndices[index] = 0xffffffffu;
        }
        if (index == 0u)
        {
            VolumetricCounter.Store(0, 0u);
        }
        return;
    }

    // BuildMode 1：タイル単位で構築する（1 スレッド = 1 タイル）。
    // 以前は 1 スレッド = 1 ライトで、ライトが覆う全タイルを 1 スレッドが直列に埋めていた。
    // 太陽などの画面全体を覆うライトは FHD（16px タイル）で 8,160 タイルを 1 スレッドが処理するため、
    // 解像度に比例して 1.6ms 以上かかっていた。タイル単位なら全タイルが並列に動き、
    // ライトの優先順（配列の順）でスロットが埋まるので結果も毎フレーム決定的になる（アトミック不要）。
    // ライトの入力（最大 160 個）は groupshared に一度だけ読み込んで全スレッドで共有する。
    uint groupIndex = dispatchThreadId.x & 63u;
    uint lightCount = min(LightCount, 160u);
    for (uint load = groupIndex; load < lightCount; load += 64u)
    {
        g_Instances[load] = Instances[load];
    }
    GroupMemoryBarrierWithGroupSync();

    // ボリューメトリック対象のライト一覧：先頭スレッドが優先順に詰める（最大 160 回の軽いループ）。
    if (index == 0u)
    {
        uint written = 0u;
        for (uint light = 0u; light < lightCount && written < MaximumVolumetricLights; ++light)
        {
            if (g_Instances[light].Metadata.w != 0u)
            {
                VolumetricLightIndices[written++] = g_Instances[light].Metadata.x;
            }
        }
        for (uint rest = written; rest < MaximumVolumetricLights; ++rest)
        {
            VolumetricLightIndices[rest] = 0xffffffffu;
        }
        VolumetricCounter.Store(0, written);
    }

    uint tileCount = TileCountX * TileCountY;
    if (index >= tileCount)
    {
        return;
    }
    uint2 tile = uint2(index % TileCountX, index / TileCountX);
    uint slots[8];
    [unroll] for (uint clear = 0u; clear < 8u; ++clear)
    {
        slots[clear] = 0xffffffffu;
    }
    [loop] for (uint light = 0u; light < lightCount; ++light)
    {
        LightInstanceInput instance = g_Instances[light];
        if (any(tile < instance.TileBounds.xy) || any(tile > instance.TileBounds.zw))
        {
            continue;
        }
        uint slotEnd = min(instance.Metadata.z, min(TileStride, 8u));
        [loop] for (uint slot = instance.Metadata.y; slot < slotEnd; ++slot)
        {
            if (slots[slot] == 0xffffffffu)
            {
                slots[slot] = instance.Metadata.x;
                break;
            }
        }
    }
    uint tileBase = index * TileStride;
    [unroll] for (uint write = 0u; write < 8u; ++write)
    {
        if (write < TileStride)
        {
            TileLightIndices[tileBase + write] = slots[write];
        }
    }
}

