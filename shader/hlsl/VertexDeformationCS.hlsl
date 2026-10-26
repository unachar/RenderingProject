#include "noise.hlsl"

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
    return float3(DeformationNoise::Perlin(float2(position.y + time, position.z + 11.2f), frequency, seed),
                  DeformationNoise::Perlin(float2(position.x + time, position.z + 37.9f), frequency, seed),
                  DeformationNoise::Perlin(float2(position.x + 93.1f, position.y + time), frequency, seed));
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
