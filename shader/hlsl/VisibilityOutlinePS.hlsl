#define SHADER_3D
#include "common.hlsl"
#include "VisibilityCommon.hlsl"

uint4 main(PSInput3D input) : SV_Target0
{
    // 球面法線補間 (SNI): 頂点カラーの alpha (Diffuse.a) をマスクとして使用
    float headMask = step(15.0f, input.Diffuse.a);
    float3 normal = input.Normal.xyz;
    float3 sniNormal = SafeNormalizeCommon(input.SniVector, normal);
    normal = lerp(normal, sniNormal, headMask);

    return PackVisibilityPayload(input.Diffuse.rgb, normal, saturate(input.Position.z), 0.0f, 0.8f, 0.02f,
                                 (float)ShaderClass, 0.5f, 0.05f, 1.0f);
}
