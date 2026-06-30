#define SHADER_3D
#include "common.hlsl"

Texture2D g_Texture : register(t0);
SamplerState g_SamplerState : register(s0);

GBufferOutput main(PSInput3D In)
{
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : max(In.Diffuse.a, 0.0f);
    float4 baseColor = float4(In.Diffuse.rgb, 1.0f);
    if (UseTexture != 0)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, In.TexCoord);
    }

    clip(baseColor.a - 0.01f);

    float3 normal = In.Normal.xyz * ((FlipNormal != 0) ? -1.0f : 1.0f);

    GBufferOutput output;
    output.Color = baseColor;
    output.Normal = MakeGBufferNormal(normal);
    output.Position = (Padding > 0.5f) ? float4(-In.WorldPos, 1.0f) : float4(In.WorldPos, 1.0f);
    output.Depth = saturate((In.Position.z / In.Position.w));
    
    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
    bool usePartParams = MaterialMode != 1;
    output.Material.r = usePartParams ? partParams.Basic.x : mMetallic;
    output.Material.g = usePartParams ? partParams.Basic.y : mRoughness;
    output.Material.b = usePartParams ? partParams.Basic.z : mFresnel;
    output.Material.a = materialPartId;
    output.Shadow.r = usePartParams ? partParams.Shadow0.x : ShadowThreshold;
    output.Shadow.g = usePartParams ? partParams.Shadow0.y : ShadowSoftness;
    output.Shadow.b = usePartParams ? partParams.Shadow0.z : ShadowStrength;
    output.Shadow.a = 1.0f;
    output.Color.a = 1.0f;
    return output;
}
