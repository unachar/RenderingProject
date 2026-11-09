#define SHADER_3D
#include "common.hlsl"

PSInput3D main(VSInput3D input)
{
    PSInput3D output;
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    float4 viewPos = mul(worldPos, View);
    output.Position = mul(viewPos, Projection);
    output.Normal = normalize(mul(input.Normal, (float3x3)World));
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    output.WorldPos = worldPos.xyz;
    output.ViewPos = viewPos.xyz;
    output.CameraPos = CameraPos;

    // 球面法線補間 (SNI): 頭部球の中心から頂点へのベクトル（ワールド空間）
    // SniParams.xyz = 球の中心, SniParams.w = 半径（>0 で有効）
    float radius = max(SniParams.w, 0.0001f);
    float3 toCenter = (worldPos.xyz - SniParams.xyz) / radius;
    output.SniVector = toCenter * step(0.0f, SniParams.w);  // 無効時はゼロベクトル

    return output;
}
