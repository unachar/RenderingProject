Texture2DArray<float> DeinterleavedDepth : register(t0);
Texture2DArray<float4> DeinterleavedNormal : register(t1);
RWStructuredBuffer<uint> RayOrder : register(u0);

cbuffer ScreenSpaceConstants : register(b0)
{
    float4x4 InvViewProjection;
    uint2 FullExtent;
    uint2 DeinterleavedExtent;
    float4 EffectParams;
    uint4 FeatureFlags;
};

groupshared uint BinCounts[16];
groupshared uint BinOffsets[16];
groupshared uint BinCursor[16];
groupshared uint LocalBins[64];
groupshared uint ValidCount;
groupshared uint InvalidCursor;

uint Part1By1(uint x)
{
    x &= 0x000000ffu;
    x = (x | (x << 4u)) & 0x00000f0fu;
    x = (x | (x << 2u)) & 0x00003333u;
    x = (x | (x << 1u)) & 0x00005555u;
    return x;
}

uint Morton2D(uint2 value)
{
    return Part1By1(value.x) | (Part1By1(value.y) << 1u);
}

float2 OctEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-5f);
    float2 oct = n.xy;
    if (n.z < 0.0f) oct = (1.0f - abs(oct.yx)) * (oct >= 0.0f ? 1.0f : -1.0f);
    return oct * 0.5f + 0.5f;
}

[numthreads(64, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint tilesX = (FullExtent.x + 7u) / 8u;
    uint tileIndex = groupId.x;
    uint2 tile = uint2(tileIndex % tilesX, tileIndex / tilesX);
    uint2 local = uint2(lane & 7u, lane >> 3u);
    uint2 pixel = tile * 8u + local;

    if (FeatureFlags.x == 0u)
    {
        RayOrder[tileIndex * 64u + lane] = all(pixel < FullExtent)
            ? (pixel.y * FullExtent.x + pixel.x)
            : 0xffffffffu;
        return;
    }

    if (lane < 16u)
    {
        BinCounts[lane] = 0u;
        BinOffsets[lane] = 0u;
        BinCursor[lane] = 0u;
    }
    if (lane == 0u)
    {
        ValidCount = 0u;
        InvalidCursor = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    bool valid = all(pixel < FullExtent);
    uint bin = 0u;
    if (valid)
    {
        uint slice = (pixel.x & 3u) | ((pixel.y & 3u) << 2u);
        uint2 dePixel = pixel >> 2u;
        float3 normal = normalize(DeinterleavedNormal.Load(int4(dePixel, slice, 0)).xyz * 2.0f - 1.0f);
        float depth = DeinterleavedDepth.Load(int4(dePixel, slice, 0));
        float hash = frac(dot(float2(pixel), float2(0.06711056f, 0.00583715f)) + depth * 7.13f);
        float angle = hash * 6.28318530718f;
        float3 tangent = normalize(abs(normal.y) < 0.98f ? cross(float3(0.0f, 1.0f, 0.0f), normal) : cross(float3(1.0f, 0.0f, 0.0f), normal));
        float3 bitangent = cross(normal, tangent);
        float3 ray = normalize(normal * 0.55f + tangent * cos(angle) + bitangent * sin(angle));
        uint2 oct = min((uint2)(saturate(OctEncode(ray)) * 15.0f), 15u);
        bin = (Morton2D(oct) ^ ((uint)(depth * 65535.0f) >> 6u)) & 15u;
        InterlockedAdd(BinCounts[bin], 1u);
    }
    LocalBins[lane] = bin;
    GroupMemoryBarrierWithGroupSync();

    if (lane < 16u)
    {
        uint prefix = 0u;
        [unroll]
        for (uint i = 0u; i < 16u; ++i)
        {
            if (i < lane) prefix += BinCounts[i];
        }
        BinOffsets[lane] = prefix;
    }
    if (lane == 0u)
    {
        uint total = 0u;
        [unroll]
        for (uint i = 0u; i < 16u; ++i) total += BinCounts[i];
        ValidCount = total;
    }
    GroupMemoryBarrierWithGroupSync();

    uint destination;
    if (valid)
    {
        uint localOffset;
        InterlockedAdd(BinCursor[LocalBins[lane]], 1u, localOffset);
        destination = BinOffsets[LocalBins[lane]] + localOffset;
    }
    else
    {
        uint invalidOffset;
        InterlockedAdd(InvalidCursor, 1u, invalidOffset);
        destination = ValidCount + invalidOffset;
    }
    RayOrder[tileIndex * 64u + destination] = valid ? (pixel.y * FullExtent.x + pixel.x) : 0xffffffffu;
}
