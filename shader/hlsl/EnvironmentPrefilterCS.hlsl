// Stage 2 J: GGX prefilter for equirectangular environment sources
// (local reflection probes). One dispatch fills one mip level of the
// prefiltered texture; the host runs levels 0..8 so the result matches
// the lighting shader's contract in SurfaceLighting.hlsli:
// specular sampled at mip = roughness * 8, irradiance at the last mip.
// Self-contained on purpose: the pass uses its own root signature
// (b0 constants, t0 source, u0 output, s0 static sampler).

Texture2D<float4> SourceEnvironment : register(t0);
RWTexture2D<float4> PrefilteredOutput : register(u0);
SamplerState LinearSampler : register(s0);

cbuffer PrefilterConstants : register(b0)
{
    uint2 OutputSize;
    uint2 SourceSize;
    float Roughness;
    uint SampleCount;
    float2 PrefilterPadding;
};

static const float PREFILTER_PI = 3.14159265359f;

// Inverse of the equirect mapping used by SurfaceLighting.hlsli.
float3 EquirectUvToDirection(float2 uv)
{
    const float phi = (uv.x - 0.5f) * 2.0f * PREFILTER_PI;
    const float theta = uv.y * PREFILTER_PI;
    const float sinTheta = sin(theta);
    return float3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
}

float2 DirectionToEquirectUv(float3 direction)
{
    float2 uv;
    uv.x = frac(atan2(direction.z, direction.x) / (2.0f * PREFILTER_PI) + 0.5f);
    uv.y = acos(clamp(direction.y, -1.0f, 1.0f)) / PREFILTER_PI;
    return uv;
}

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f; // 2^-32
}

float2 Hammersley(uint i, uint count)
{
    return float2(float(i) / float(count), RadicalInverseVdC(i));
}

float3 ImportanceSampleGGX(float2 xi, float3 N, float roughness)
{
    const float a = roughness * roughness;
    const float phi = 2.0f * PREFILTER_PI * xi.x;
    const float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
    const float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    const float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(up, N));
    const float3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float DistributionGGX(float NdotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (PREFILTER_PI * denom * denom);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= OutputSize.x || dispatchThreadId.y >= OutputSize.y)
    {
        return;
    }
    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(OutputSize);
    const float3 R = EquirectUvToDirection(uv);
    if (Roughness < 0.001f)
    {
        PrefilteredOutput[dispatchThreadId.xy] =
            float4(SourceEnvironment.SampleLevel(LinearSampler, DirectionToEquirectUv(R), 0.0f).rgb, 1.0f);
        return;
    }

    const float3 N = R;
    const float3 V = R;
    const float saTexel = 4.0f * PREFILTER_PI / float(SourceSize.x * SourceSize.y);
    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;
    const uint sampleCount = max(SampleCount, 1u);
    for (uint i = 0u; i < sampleCount; ++i)
    {
        const float2 xi = Hammersley(i, sampleCount);
        const float3 H = ImportanceSampleGGX(xi, N, Roughness);
        const float3 L = normalize(2.0f * dot(V, H) * H - V);
        const float NdotL = clamp(dot(N, L), 0.0f, 1.0f);
        if (NdotL > 0.0f)
        {
            const float NdotH = clamp(dot(N, H), 0.0f, 1.0f);
            const float HdotV = clamp(dot(H, V), 0.0f, 1.0f);
            const float pdf = DistributionGGX(NdotH, Roughness) * NdotH / (4.0f * HdotV) + 0.0001f;
            const float saSample = 1.0f / (float(sampleCount) * pdf);
            const float mipLevel = max(0.5f * log2(saSample / saTexel) + 1.0f, 0.0f);
            accumulated += SourceEnvironment.SampleLevel(LinearSampler, DirectionToEquirectUv(L), mipLevel).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    accumulated /= max(totalWeight, 1e-5f);
    PrefilteredOutput[dispatchThreadId.xy] = float4(accumulated, 1.0f);
}
