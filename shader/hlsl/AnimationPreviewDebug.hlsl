#define SHADER_3D
#include "common.hlsl"

struct DebugVertex
{
    float3 Position : POSITION;
    float Weight : BLENDWEIGHT;
};

struct DebugPixel
{
    float4 Position : SV_Position;
    float Weight : BLENDWEIGHT;
};

DebugPixel VSMain(DebugVertex input)
{
    DebugPixel output;
    output.Position = mul(mul(mul(float4(input.Position, 1.0f), World), View), Projection);
    output.Weight = input.Weight;
    return output;
}

float4 PSMain(DebugPixel input) : SV_Target
{
    if (Padding < 0.5f)
    {
        return float4(0.25f, 0.85f, 1.0f, 1.0f);
    }

    float weight = saturate(input.Weight);
    float3 low = float3(0.05f, 0.12f, 0.30f);
    float3 middle = float3(0.0f, 0.85f, 0.55f);
    float3 high = float3(1.0f, 0.28f, 0.04f);
    return float4(lerp(lerp(low, middle, saturate(weight * 2.0f)), high, saturate(weight * 2.0f - 1.0f)), 1.0f);
}
