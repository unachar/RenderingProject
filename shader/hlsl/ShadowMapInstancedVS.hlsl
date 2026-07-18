#define SHADER_3D
#include "common.hlsl"

struct InstanceTransform
{
    row_major float4x4 World;
};

StructuredBuffer<InstanceTransform> InstanceTransforms : register(t0, space2);

float4 main(VSInput3D input, uint instanceId : SV_InstanceID) : SV_POSITION
{
    float4 worldPos = mul(float4(input.Position, 1.0f), InstanceTransforms[instanceId].World);
    return mul(worldPos, LightViewProjection);
}
