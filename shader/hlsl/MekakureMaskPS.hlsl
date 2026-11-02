// ---------------------------------------------------------------------------------------------
// メカクレの髪マスク。前髪のどこから・どれだけ目を透かすかを R8 マスクへ書く（学マス方式：
// 髪テクスチャのアルファチャンネルで透ける領域と透け具合を制御）。
//   Padding         = 0: 髪テクスチャのアルファ, 1: 専用マスク画像の R
//   ConstantPadding = 1 で白黒反転
//   MaterialAlpha   = マスクの効き（0 で常に 1 = 制限なし）
// 1 = 目を透かす、0 = 透かさない。重なった髪は MIN ブレンドで「透かさない」側が残る。
// ---------------------------------------------------------------------------------------------
#define SHADER_3D
#include "common.hlsl"

Texture2D g_Texture : register(t0);
SamplerState g_SamplerState : register(s0);

float main(PSInput3D In) : SV_Target0
{
    float4 texel = g_Texture.Sample(g_SamplerState, In.TexCoord);
    float mask = Padding > 0.5f ? texel.r : texel.a;
    mask = ConstantPadding > 0.5f ? 1.0f - mask : mask;
    return lerp(1.0f, saturate(mask), saturate(MaterialAlpha));
}
