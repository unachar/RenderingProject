ByteAddressBuffer BaseVertices : register(t0);
RWByteAddressBuffer CurrentVertices : register(u0);
RWByteAddressBuffer PreviousVertices : register(u1);

struct MotionData
{
    float4 AmplitudeAndPhase;
    float4 Parameters;
};

cbuffer DeformationConstants : register(b0)
{
    MotionData Current;
    MotionData Previous;
    uint4 Geometry;
};

uint NoiseHash(int2 cell, uint seed)
{
    uint value = uint(cell.x) * 0x9e3779b9u ^ uint(cell.y) * 0x85ebca6bu ^ seed;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float Gradient(int2 cell, float2 delta, uint seed)
{
    switch (NoiseHash(cell, seed) & 7u)
    {
        case 0:
            return delta.x;
        case 1:
            return -delta.x;
        case 2:
            return delta.y;
        case 3:
            return -delta.y;
        case 4:
            return (delta.x + delta.y) * 0.70710678118f;
        case 5:
            return (delta.x - delta.y) * 0.70710678118f;
        case 6:
            return (-delta.x + delta.y) * 0.70710678118f;
        default:
            return (-delta.x - delta.y) * 0.70710678118f;
    }
}

float Perlin(float2 position, float frequency, uint seed)
{
    position = clamp(position * clamp(frequency, 0.0001f, 2.0f), -10000.0f, 10000.0f);
    int2 cell = int2(floor(position));
    float2 local = position - cell;
    float2 weight = local * local * local * (local * (local * 6.0f - 15.0f) + 10.0f);
    float low = lerp(Gradient(cell, local, seed), Gradient(cell + int2(1, 0), local - float2(1, 0), seed), weight.x);
    float high = lerp(Gradient(cell + int2(0, 1), local - float2(0, 1), seed),
                      Gradient(cell + int2(1, 1), local - 1.0f, seed), weight.x);
    return clamp(lerp(low, high, weight.y) * 1.41421356237f, -1.0f, 1.0f);
}

float3 SpatialOffset(float3 position, MotionData motion, bool rest)
{
    float phase = rest ? 0 : motion.AmplitudeAndPhase.w;
    float frequency = motion.Parameters.x;
    float3 phases =
        float3(position.y * 0.73f + position.z, position.x + position.z * 0.61f, position.x * 0.83f + position.y) *
        frequency;
    int method = int(motion.Parameters.z);
    if (method == 0)
    {
        return sin(phases + phase);
    }
    if (method == 1)
    {
        return float3(cos(phases.y + phase), sin(phases.x + phase), sin(phases.z + phase));
    }
    float time = rest ? 0 : motion.Parameters.y;
    uint seed = asuint(motion.Parameters.w);
    return float3(Perlin(float2(position.y + time, position.z + 11.2f), frequency, seed),
                  Perlin(float2(position.x + time, position.z + 37.9f), frequency, seed),
                  Perlin(float2(position.x + 93.1f, position.y + time), frequency, seed));
}

float3 DeformPosition(float3 position, MotionData motion)
{
    return position + motion.AmplitudeAndPhase.xyz *
                          (SpatialOffset(position, motion, false) - SpatialOffset(position, motion, true));
}

float3 DeformNormal(float3 position, float3 normal, MotionData motion)
{
    normal = dot(normal, normal) > 1e-16f ? normalize(normal) : float3(0, 1, 0);
    float3 axis = abs(normal.y) > 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
    float3 tangent = normalize(cross(axis, normal));
    float3 bitangent = cross(normal, tangent);
    float stepSize = 0.001f / max(1.0f, motion.Parameters.x);
    float3 deltaT =
        DeformPosition(position + tangent * stepSize, motion) - DeformPosition(position - tangent * stepSize, motion);
    float3 deltaB = DeformPosition(position + bitangent * stepSize, motion) -
                    DeformPosition(position - bitangent * stepSize, motion);
    float3 transformed = cross(deltaT, deltaB);
    return dot(transformed, transformed) > 1e-16f ? normalize(transformed) : normal;
}

[numthreads(64, 1, 1)] void main(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint vertex = (group.x + group.y * 65535u) * 64u + thread;
    if (vertex >= Geometry.x)
    {
        return;
    }
    uint address = vertex * Geometry.y;
    float3 position = asfloat(BaseVertices.Load3(address));
    float3 normal = asfloat(BaseVertices.Load3(address + 12));
    CurrentVertices.Store3(address, asuint(DeformPosition(position, Current)));
    CurrentVertices.Store3(address + 12, asuint(DeformNormal(position, normal, Current)));
    PreviousVertices.Store3(address, asuint(DeformPosition(position, Previous)));
    PreviousVertices.Store3(address + 12, asuint(DeformNormal(position, normal, Previous)));
    uint2 texcoord = BaseVertices.Load2(address + 24);
    uint4 color = BaseVertices.Load4(address + 32);
    CurrentVertices.Store2(address + 24, texcoord);
    PreviousVertices.Store2(address + 24, texcoord);
    CurrentVertices.Store4(address + 32, color);
    PreviousVertices.Store4(address + 32, color);
}
