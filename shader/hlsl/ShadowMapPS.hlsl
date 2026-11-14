#define SHADER_3D
// 影はシャドウマップ解像度で描くので、表示用の Mip LOD バイアスは掛けない。
#define RP_NO_MATERIAL_MIP_BIAS
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
    // PBRParams の PBRPadding は CPU 側 PBRConstants::Reserved と同じ位置（x = ShadowMode, y = ShadowCutoff）。
    // 以前は y（= ShadowCutoff 既定 0.5 → round で 0 = None）を影モードとして読んでおり、
    // 全キャスターが clip されてシャドウマップ / VSM が空になっていた。
    int shadowMode = (int)round(PBRPadding.x);
    clip((float)shadowMode - 0.5f);
    clip(2.5f - (float)shadowMode);
    if (shadowMode == 2)
    {
        if (UseTexture != 0)
        {
            opacity *= BaseTexture.Sample(MaterialSampler, uv).a;
        }
        clip(opacity - max(PBRPadding.y, 1.0f / 255.0f));
    }
}



