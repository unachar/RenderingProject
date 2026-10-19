#define SHADER_3D
#include "common.hlsl"

struct InstanceTransform
{
    row_major float4x4 World;
};

StructuredBuffer<InstanceTransform> InstanceTransforms : register(t0, space2);

PSInput3D main(VSInput3D input, uint instanceId : SV_InstanceID)
{
    PSInput3D output;
    row_major float4x4 instanceWorld = InstanceTransforms[instanceId].World;
    float4 worldPos = mul(float4(input.Position, 1.0f), instanceWorld);
    float4 viewPos = mul(worldPos, View);
    output.Position = mul(viewPos, Projection);
    output.Normal = normalize(mul(input.Normal, (float3x3)instanceWorld));
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    output.WorldPos = worldPos.xyz;
    output.ViewPos = viewPos.xyz;
    output.CameraPos = CameraPos;
    return output;
}
