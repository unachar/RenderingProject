#ifndef NOISE_CACHE_LAYOUT_INCLUDED
#define NOISE_CACHE_LAYOUT_INCLUDED
// Keep these constants in sync with ProcStudio::NoiseSampleCache.
static const uint NoiseCacheBucketCount = 8192u;
static const uint NoiseCacheWays = 4u;
static const uint NoiseCacheEntryBytes = 32u;
static const uint NoiseCacheHeaderBytes = 32u;
static const uint NoiseCacheRequestBytes = 256u;
static const uint NoiseCacheRequestStateOffset = 252u;
static const uint NoiseCacheVerticesPerSide = 16u;
static const uint NoiseCacheCellsPerSide = 15u;
static const uint NoiseCacheTexelBase = NoiseCacheHeaderBytes + NoiseCacheBucketCount * NoiseCacheWays * NoiseCacheEntryBytes;

struct NoiseCacheKey
{
    uint4 Words;
    uint TileY;
};

uint NoiseCacheHash(NoiseCacheKey key)
{
    uint h = key.Words.x ^ (key.Words.y * 0x9e3779b9u) ^ (key.Words.z * 0x85ebca6bu) ^
             (key.Words.w * 0xc2b2ae35u) ^ (key.TileY * 0x27d4eb2fu);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    return (h ^ (h >> 16)) & (NoiseCacheBucketCount - 1u);
}

uint NoiseCacheTexelAddress(uint page, uint2 vertex)
{
    return NoiseCacheTexelBase + (page * 256u + vertex.y * NoiseCacheVerticesPerSide + vertex.x) * 8u;
}

float3 NoiseCacheUnpackColor(uint2 packed)
{
    return float3(f16tof32(packed.x & 0xffffu), f16tof32(packed.x >> 16u), f16tof32(packed.y & 0xffffu));
}
#endif
