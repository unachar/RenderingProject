#define SHADER_3D
#include "common.hlsl"

float4 main(VSInput3D input) : SV_POSITION
{
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    return mul(worldPos, LightViewProjection);
}
