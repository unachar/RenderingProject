#define SHADER_3D
#include "common.hlsl"

Texture2D TextureBaseColor : register(t0);
Texture2D TextureNormal : register(t2);
SamplerState Sampler : register(s0);

PS_OUTPUT_GEOMETRY main(PSInput3D input)
{
    PS_OUTPUT_GEOMETRY output;

    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : max(input.Diffuse.a, 0.0f);
    float4 baseColor = float4(input.Diffuse.rgb, 1.0f);
    float3 normal = input.Normal;
    if (UseTexture)
    {
        baseColor *= TextureBaseColor.Sample(Sampler, input.TexCoord);
    }

    clip(baseColor.a - 0.01f);

    if (UseNormalMap != 0)
    {
        float3 tangentNormal = TextureNormal.Sample(Sampler, input.TexCoord).xyz * 2.0f - 1.0f;
        normal = ApplyNormalMapCommon(normal, input.WorldPos, input.TexCoord, tangentNormal);
    }

    output.Color = baseColor;
    output.Normal = MakeGBufferNormal(normal);
    output.Position = float4(input.WorldPos, 1.0f);
    
    output.Depth = saturate(input.Position.z);
    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
    bool usePartParams = MaterialMode != 1;
    output.Material.r = usePartParams ? partParams.Basic.x : mMetallic;
    output.Material.g = usePartParams ? partParams.Basic.y : mRoughness;
    output.Material.b = usePartParams ? partParams.Basic.z : mFresnel;
    output.Material.a = materialPartId;
    output.Shadow = usePartParams
        ? float4(partParams.Shadow0.x, partParams.Shadow0.y, partParams.Shadow0.z, 0.0f)
        : float4(ShadowThreshold, ShadowSoftness, ShadowStrength, 0.0f);
    output.RimStyle = usePartParams
        ? float4(partParams.Highlight.x, partParams.Highlight.y, partParams.RimStyle.x, partParams.RimStyle.y)
        : float4(RimStrength, RimThreshold, RimSoftness, RimPower);
    output.RimLight = usePartParams
        ? float4(partParams.RimLight.rgb, partParams.RimStyle.z)
        : float4(RimColor, RimAlbedoBlend);

    return output;
}
