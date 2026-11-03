// ---------------------------------------------------------------------------------------------
// メカクレ用ピクセルシェーダー。本体と同じ ShadeForward（トゥーン / ライブスタイル込み）で陰影を付け、
// 不透明度と「顔の向き」フェードを掛けた乗算済み α で HDR シーンへ合成する。
// 正面（N・V が大きい）ほど表示、横顔・後ろ向きでは自然に消える。
//
// 透過は髪だけに限定する（t3 = キャラ単位マスク、MekakureMaskPS 参照）：
//   R：自キャラの髪以外（瞼・頬・鼻・肌）が目より手前にあれば描かない
//      → 瞼の下に潜っている目の縁・横顔で頬越しに見える目は透けない
//   G：Padding ≥ 1 のとき、手前に見えているのが自キャラの髪のピクセルだけ描く
// パーツ別の設定（目 / ハイライト / 眉 / まつ毛 / 顔 / 追加）はメッシュごとの定数で渡される：
//   MaterialAlpha = 透け具合, ToonOutlineWidth = 奥行き, ToonOutlineScreenWidth / ConstantPadding = フェード,
//   Padding = 1 + マスクの効き, ToonOutlineUseScreenSpace bit0 = 皮膚で隠す
// 判定は ShadeForward より前に行い、不要なピクセルのライティングを省く。
// ---------------------------------------------------------------------------------------------
#define MODEL_SHADER_NO_ENTRY
#include "modelshaderPS.hlsl"

float4 main(in PSInput3D In) : SV_Target0
{
    float2 mask = SceneColorTexture.Load(int3(In.Position.xy, 0)).rg;

    // 皮膚の遮蔽（パーツ別に切り替え。ToonOutlineUseScreenSpace の bit0）
    //   許容差は「透過する奥行き」に比例（モデルのスケールに追従）
    [branch] if ((ToonOutlineUseScreenSpace & 1) != 0)
    {
        float eyeDistance = distance(In.WorldPos, CameraPos);
        float skinDistance = -mask.r;
        float tolerance = clamp(max(ToonOutlineWidth, 0.0f) * 0.01f, 0.0001f, 0.002f);
        clip(skinDistance - (eyeDistance - tolerance));
    }

    // 髪の被覆：Padding = 1 + マスクの効き（共通 × パーツ別）。0 なら被覆判定なし
    float hairTransmit = 1.0f;
    [branch] if (Padding > 0.5f)
    {
        clip(mask.g - 0.5f); // 手前が自キャラの髪ではない
        hairTransmit = lerp(1.0f, saturate(2.0f - mask.g), saturate(Padding - 1.0f));
    }

    float3 normal = SafeNormalizeCommon(In.Normal, float3(0.0f, 0.0f, 1.0f));
    float3 viewDir = SurfaceViewDirectionCommon(In.WorldPos, CameraPos);
    float fadeStart = ToonOutlineScreenWidth;
    float fadeEnd = min(ConstantPadding, fadeStart - 0.001f);
    float facing = smoothstep(fadeEnd, fadeStart, dot(normal, viewDir));
    clip(facing * hairTransmit - (1.0f / 255.0f));

    float4 premultiplied = ShadeForward(In) * (facing * hairTransmit);
    clip(premultiplied.a - (1.0f / 255.0f));
    return premultiplied;
}
