#include "CloudNoise.hlsli"
cbuffer NoiseParameters : register(b0) { uint Seed; uint Extent; };
RWTexture3D<float> OutputNoise : register(u0);
[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= Extent)) return;
    float3 uv = (float3(id) + 0.5) / Extent;
    float perlin = PeriodicPerlin(uv, 4, Seed) * 0.625 +
        PeriodicPerlin(uv, 8, Seed + 1) * 0.25 + PeriodicPerlin(uv, 16, Seed + 2) * 0.125;
    float billows = 1 - WorleyFbm(uv, Seed + 71);
    OutputNoise[id] = saturate(perlin * 0.65 + billows * 0.35);
}
