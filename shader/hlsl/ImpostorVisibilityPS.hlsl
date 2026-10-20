#define SHADER_3D
#include "common.hlsl"
#include "VisibilityCommon.hlsl"
Texture2D g_Texture : register(t0);
Texture2D g_NormalTexture : register(t2);
SamplerState g_SamplerState : register(s0);

#include "ImpostorCommon.hlsli"

// Compute GBuffer（Visibility Buffer）経路。VisibilityPS と同じペイロードを書く。
struct ImpostorVisibilityOutput
{
    uint4 Payload : SV_Target0;
    float HardwareDepth : SV_DepthGreaterEqual;
};

ImpostorVisibilityOutput main(ImpostorPSInput input)
{
    ImpostorSurface surface = ResolveImpostorSurface(input);
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : surface.PartId;
    MaterialPartShaderParams part = ResolveMaterialPartParams(materialPartId);
    bool usePart = MaterialMode != 1;

    ImpostorVisibilityOutput output;
    output.HardwareDepth = max(surface.Depth, input.Position.z);
    output.Payload = PackVisibilityPayload(
        surface.Albedo, surface.WorldNormal, output.HardwareDepth, usePart ? part.Basic.x : mMetallic,
        usePart ? part.Basic.y : mRoughness, usePart ? part.Basic.z : mFresnel, materialPartId,
        usePart ? part.Shadow0.x : ShadowThreshold, usePart ? part.Shadow0.y : ShadowSoftness,
        usePart ? part.Shadow0.z : ShadowStrength);
    return output;
}
