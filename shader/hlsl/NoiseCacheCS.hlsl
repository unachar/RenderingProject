#include "ProceduralTexture.hlsli"
RWByteAddressBuffer Cache : register(u1, space15);
cbuffer CacheFrame : register(b0, space15)
{
    uint Clear;
    uint Frame;
};

groupshared ProceduralTexture::Parameters TileDefinition;
groupshared NoiseCacheKey TileKey;
groupshared uint DestinationPage;

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint bucket = group.x;
    if (bucket >= NoiseCacheBucketCount)
    {
        return;
    }
    uint requestAddress = bucket * NoiseCacheRequestBytes;
    uint firstPage = bucket * NoiseCacheWays;
    if (Clear != 0u)
    {
        if (lane < NoiseCacheWays)
        {
            uint address = NoiseCacheHeaderBytes + (firstPage + lane) * NoiseCacheEntryBytes;
            Cache.Store4(address, 0u);
            Cache.Store4(address + 16u, 0u);
        }
        if (lane == 0u)
        {
            ProceduralNoiseRequests.Store(requestAddress + NoiseCacheRequestStateOffset, 0u);
            if (bucket == 0u)
            {
                Cache.Store4(0u, uint4(1u, 0u, 0u, 0u));
                Cache.Store4(16u, 0u);
            }
        }
        return;
    }
    // Rendering has completed and only this group can reset this bucket.
    // Empty buckets avoid shared-memory synchronization.
    if (ProceduralNoiseRequests.Load(requestAddress + NoiseCacheRequestStateOffset) == 0u)
    {
        return;
    }
    if (lane == 0u)
    {
        TileKey.Words = ProceduralNoiseRequests.Load4(requestAddress);
        TileKey.TileY = ProceduralNoiseRequests.Load(requestAddress + 16u);
        TileDefinition = ProceduralNoiseRequests.Load<ProceduralTexture::Parameters>(requestAddress + 32u);
        DestinationPage = firstPage;
        uint oldest = 0xffffffffu;
        [unroll] for (uint way = 0u; way < NoiseCacheWays; ++way)
        {
            uint page = firstPage + way;
            uint address = NoiseCacheHeaderBytes + page * NoiseCacheEntryBytes;
            uint4 previous = Cache.Load4(address);
            if ((all(previous == TileKey.Words) && Cache.Load(address + 16u) == TileKey.TileY) ||
                all(previous.xy == 0u))
            {
                DestinationPage = page;
                break;
            }
            uint age = Cache.Load(address + 20u);
            if (age < oldest)
            {
                oldest = age;
                DestinationPage = page;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    int2 levels = int2(TileKey.Words.z & 255u, (TileKey.Words.z >> 8u) & 255u) - 128;
    float2 step = exp2(float2(levels));
    int2 tile = int2(asint(TileKey.Words.w), asint(TileKey.TileY));
    [loop] for (uint sampleIndex = lane; sampleIndex < 256u; sampleIndex += 64u)
    {
        uint2 vertex = uint2(sampleIndex % NoiseCacheVerticesPerSide, sampleIndex / NoiseCacheVerticesPerSide);
        float2 uv = float2(tile * int(NoiseCacheCellsPerSide) + int2(vertex)) * step;
        float3 color = ProceduralTexture::Color(TileDefinition, ProceduralTexture::Scalar(TileDefinition, uv));
        uint2 packed = uint2(f32tof16(color.x) | (f32tof16(color.y) << 16u), f32tof16(color.z));
        Cache.Store2(NoiseCacheTexelAddress(DestinationPage, vertex), packed);
    }
    // Publish metadata after this group's texels. The frame-end UAV barrier
    // publishes the completed pages to subsequent rendering dispatches.
    DeviceMemoryBarrierWithGroupSync();
    if (lane == 0u)
    {
        uint address = NoiseCacheHeaderBytes + DestinationPage * NoiseCacheEntryBytes;
        Cache.Store4(address, TileKey.Words);
        Cache.Store4(address + 16u, uint4(TileKey.TileY, Frame, 0u, 0u));
        ProceduralNoiseRequests.Store(requestAddress + NoiseCacheRequestStateOffset, 0u);
    }
}
