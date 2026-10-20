#define SHADER_3D
#define POM_PIXEL_DEPTH_OUTPUT
#include "common.hlsl"
Texture2D g_Texture : register(t0);       // インポスターのアルベドアトラス（sRGB）
Texture2D g_NormalTexture : register(t2); // 法線・深度・パーツ ID アトラス
SamplerState g_SamplerState : register(s0);

#include "ImpostorCommon.hlsli"

// ディファード（GBuffer MRT）。modelshaderPS_MRT と同じ GBuffer 規則で書き、深度は実際の表面位置を出す。
GBufferDepthOutput main(ImpostorPSInput input)
{
    ImpostorSurface surface = ResolveImpostorSurface(input);
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : surface.PartId;
    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
    bool usePartParams = MaterialMode != 1;

    GBufferDepthOutput output;
    output.Color = float4(surface.Albedo, 1.0f);
    output.Normal = MakeGBufferNormal(surface.WorldNormal);
    output.Depth = surface.Depth.xxxx;
    output.HardwareDepth = max(surface.Depth, input.Position.z);
    output.Material.r = usePartParams ? partParams.Basic.x : mMetallic;
    output.Material.g = usePartParams ? partParams.Basic.y : mRoughness;
    output.Material.b = usePartParams ? partParams.Basic.z : mFresnel;
    output.Material.a = materialPartId;
    output.Shadow = usePartParams ? float4(partParams.Shadow0.x, partParams.Shadow0.y, partParams.Shadow0.z, 0.0f)
                                  : float4(ShadowThreshold, ShadowSoftness, ShadowStrength, 0.0f);
    output.EmissionAO = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return output;
}
