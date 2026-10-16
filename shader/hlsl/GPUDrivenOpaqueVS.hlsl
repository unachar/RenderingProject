#define SHADER_3D
#define SHADER_GPU_DRIVEN
#include "common.hlsl"
#include "GPUDrivenOpaque.hlsli"
PSInput3D main(VSInput3D input)
{
    GPUInstanceData instance = gSceneInstances[GPUInstanceID];
    PSInput3D output;
    float4 world = mul(float4(input.Position, 1), instance.World);
    float4 view = mul(world, View);
    output.Position = mul(view, Projection);
    output.Normal = normalize(mul(input.Normal, (float3x3)instance.World));
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    output.WorldPos = world.xyz;
    output.ViewPos = view.xyz;
    output.CameraPos = CameraPos;
    output.InstanceID = GPUInstanceID;
    return output;
}

