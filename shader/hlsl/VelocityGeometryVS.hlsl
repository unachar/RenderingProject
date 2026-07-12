#define SHADER_3D
#include "common.hlsl"

struct VelocityInput
{
    float3 Position : POSITION;
    float3 PreviousPosition : PREVPOS;
};

struct VelocityOutput
{
    float4 Position : SV_POSITION;
    float4 CurrentClip : TEXCOORD0;
    float4 PreviousClip : TEXCOORD1;
};

VelocityOutput main(VelocityInput input)
{
    VelocityOutput output;
    float4 currentWorld = mul(float4(input.Position, 1.0f), World);
    float4 currentView = mul(currentWorld, View);
    output.CurrentClip = mul(currentView, Projection);
    output.Position = output.CurrentClip;

    float4 previousWorld = mul(float4(input.PreviousPosition, 1.0f), PreviousWorld);
    output.PreviousClip = mul(previousWorld, PreviousViewProjection);
    return output;
}
