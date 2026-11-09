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

    // 球面法線補間 (SNI): 頭部球の中心から頂点へのベクトル（ワールド空間）
    float radius = max(SniParams.w, 0.0001f);
    float3 toCenter = (world.xyz - SniParams.xyz) / radius;
    output.SniVector = toCenter * step(0.0f, SniParams.w);

    return output;
}
