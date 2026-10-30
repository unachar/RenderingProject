// Hi-Z pyramid generation with FidelityFX SPD (single-pass downsampler).
// Flag-selectable alternative to the HiZCopyCS + HiZReduceCS chain in
// OcclusionCulling: the whole mip chain is produced by ONE dispatch.
// Min reduction, matching the existing reverse-Z Hi-Z convention (a tile
// stores the minimum depth of its texels). Vendored headers: see
// External/FidelityFX-SPD/VENDORED.md (MIT).

#define FFX_GPU
#define FFX_HLSL

Texture2D<float> SpdSourceImage : register(t0);
Texture2D<float> SpdDstMip5Image : register(t1);
globallycoherent RWTexture2D<float> SpdDstMips[12] : register(u0);
globallycoherent RWByteAddressBuffer SpdGlobalAtomic : register(u12);

cbuffer SpdConstants : register(b0)
{
    uint2 WorkGroupOffset;
    uint2 NumWorkGroupsAndMips;
};

groupshared float g_SpdIntermediate[16][16];

float4 SpdLoadSourceImage(int2 p, uint slice)
{
    return SpdSourceImage.Load(int3(p, 0)).xxxx;
}

float4 SpdLoad(int2 p, uint slice)
{
    return SpdDstMip5Image.Load(int3(p, 0)).xxxx;
}

void SpdStore(int2 p, float4 value, uint mip, uint slice)
{
    switch (mip)
    {
    case 0: SpdDstMips[0][p] = value.x; break;
    case 1: SpdDstMips[1][p] = value.x; break;
    case 2: SpdDstMips[2][p] = value.x; break;
    case 3: SpdDstMips[3][p] = value.x; break;
    case 4: SpdDstMips[4][p] = value.x; break;
    case 5: SpdDstMips[5][p] = value.x; break;
    case 6: SpdDstMips[6][p] = value.x; break;
    case 7: SpdDstMips[7][p] = value.x; break;
    case 8: SpdDstMips[8][p] = value.x; break;
    case 9: SpdDstMips[9][p] = value.x; break;
    case 10: SpdDstMips[10][p] = value.x; break;
    case 11: SpdDstMips[11][p] = value.x; break;
    }
}

float4 SpdLoadIntermediate(uint x, uint y)
{
    return float4(g_SpdIntermediate[x][y], 0.0, 0.0, 0.0);
}

void SpdStoreIntermediate(uint x, uint y, float4 value)
{
    g_SpdIntermediate[x][y] = value.x;
}

float4 SpdReduce4(float4 v0, float4 v1, float4 v2, float4 v3)
{
    return min(min(v0, v1), min(v2, v3));
}

void SpdIncreaseAtomicCounter(uint slice)
{
    uint unused;
    SpdGlobalAtomic.InterlockedAdd(0, 1, unused);
}

uint SpdGetAtomicCounter()
{
    return SpdGlobalAtomic.Load(0);
}

void SpdResetAtomicCounter(uint slice)
{
    SpdGlobalAtomic.Store(0, 0);
}

#include "../../External/FidelityFX-SPD/ffx_core.h"
#include "../../External/FidelityFX-SPD/ffx_spd.h"

[numthreads(256, 1, 1)] void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    SpdDownsample(groupId.xy, groupThreadId.x, NumWorkGroupsAndMips.y, NumWorkGroupsAndMips.x, 0, WorkGroupOffset);
}
