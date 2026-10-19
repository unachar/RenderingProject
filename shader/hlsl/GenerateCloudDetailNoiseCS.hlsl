#include "CloudNoise.hlsli"

cbuffer NoiseParameters : register(b0)
{
    uint Seed;
    uint Extent;
};

RWTexture3D<float> OutputNoise : register(u0);
[numthreads(4, 4, 4)] void main(uint3 id : SV_DispatchThreadID) {
    if (any(id >= Extent))
    {
        return;
    }
    OutputNoise[id] = WorleyFbm((float3(id) + 0.5) / Extent, Seed + 127);
}
