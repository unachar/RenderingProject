// ---------------------------------------------------------------------------------------------
// メカクレのキャラ単位マスク（R32G32_FLOAT、MAX ブレンド、クリア値 R = -1e30 / G = 0）。
// プロセカ方式の「目の手前が自キャラの髪のときだけ透かす」を、ステンシルの代わりにこのマスクで判定する。
//   Padding（モード）
//     0: 髪 … 髪テクスチャのアルファで透け具合（学マス方式）
//     1: 髪 … 専用マスク画像の R で透け具合
//     2: 髪 … 透け具合の制限なし（被覆だけ。テクスチャのアルファ 0.5 未満は髪の穴として除外）
//     3: 髪以外（顔・瞼・肌・体） … カメラからの距離
//   ConstantPadding = 1 で白黒反転（モード 0 / 1）
//   MaterialAlpha   = 1 で生の透け具合を書く（効きはパーツ別に MekakurePS で掛ける）
// 出力
//   R = -距離（髪以外。MAX で最も手前の皮膚が残る）
//   G = 2 - 透け具合（髪。0 = 髪なし、1〜2 = 髪あり。重なった髪は MAX で「透かさない」側が残る）
// ---------------------------------------------------------------------------------------------
#define SHADER_3D
#include "common.hlsl"

Texture2D g_Texture : register(t0);
SamplerState g_SamplerState : register(s0);

static const float kMekakureNoValue = -1.0e30f;

float2 main(PSInput3D In) : SV_Target0
{
    float4 texel = g_Texture.Sample(g_SamplerState, In.TexCoord);
    [branch] if (Padding > 2.5f)
    {
        clip(texel.a - 0.5f); // アルファで抜いた部分（カットアウト）は目を隠さない
        return float2(-distance(In.WorldPos, CameraPos), 0.0f);
    }
    float transmit = 1.0f;
    [branch] if (Padding > 1.5f)
    {
        clip(texel.a - 0.5f); // 髪の穴（カットアウト）は被覆に含めない
    }
    else
    {
        float mask = Padding > 0.5f ? texel.r : texel.a;
        mask = ConstantPadding > 0.5f ? 1.0f - mask : mask;
        transmit = lerp(1.0f, saturate(mask), saturate(MaterialAlpha));
    }
    return float2(kMekakureNoValue, 2.0f - transmit);
}
