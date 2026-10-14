#define SHADER_3D
#include "common.hlsl"
#include "MaterialSurface.hlsli"

Texture2D<float4> BaseTexture : register(t0);
SamplerState MaterialSampler : register(s0);

void main(float4 position : SV_POSITION, float2 uv : TEXCOORD, float vertexOpacity : COLOR)
{
    AssetMaterialParameters material = LoadAssetMaterial(0);
    float opacity = saturate(MaterialAlpha);
    if (material.Surface.z > 0.5f)
    {
        opacity *= material.BaseColor.a;
    }
    clip(opacity - (1.0f / 255.0f));
    int shadowMode = (int)round(PBRPadding.y);
    clip((float)shadowMode - 0.5f);
    clip(2.5f - (float)shadowMode);
    if (shadowMode == 2)
    {
        if (UseTexture != 0)
        {
            opacity *= BaseTexture.Sample(MaterialSampler, uv).a;
        }
        clip(opacity - max(PBRPadding.z, 1.0f / 255.0f));
    }
}
