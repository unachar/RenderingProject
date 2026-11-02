// ---------------------------------------------------------------------------------------------
// メカクレ（前髪越しに目・眉を透かす）用の頂点シェーダー。
//
// 目・眉・まつ毛メッシュを、HDR 半透明パスで「深度だけカメラ側へ寄せて」もう一度描く。
// 頂点を視線（カメラへのレイ）に沿って動かすので画面上の位置は変わらず、深度だけが手前になる。
//   - 目の手前 MekakureDepth 以内にある遮蔽物（前髪）は深度テストを通過 → 髪越しに透けて見える
//   - それより奥の遮蔽（後頭部・別キャラ）は通過しない → 後ろ向きで目が透けることはない
// 定数の流用（ConstantBuffer3D の輪郭線用フィールド。このパスでは輪郭線を描かない）：
//   ToonOutlineWidth       = MekakureDepth（ワールド単位）
//   ToonOutlineScreenWidth = フェード開始の N・V
//   ConstantPadding        = フェード終了の N・V
//   MaterialAlpha          = メカクレの不透明度 × マテリアル α
// ---------------------------------------------------------------------------------------------
#define SHADER_3D
#include "common.hlsl"

PSInput3D main(VSInput3D input)
{
    PSInput3D output;
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    float3 toCamera = SurfaceViewDirectionCommon(worldPos.xyz, CameraPos);
    float depthOffset = max(ToonOutlineWidth, 0.0f);
    if (abs(Projection[3][3]) < 0.5f)
    {
        // 透視投影：ニアクリップを越えないよう、カメラまでの距離の 90% までに制限する。
        depthOffset = min(depthOffset, length(CameraPos - worldPos.xyz) * 0.9f);
    }
    float4 pushedWorld = float4(worldPos.xyz + toCamera * depthOffset, 1.0f);
    output.Position = mul(mul(pushedWorld, View), Projection);
    output.Normal = normalize(mul(input.Normal, (float3x3)World));
    output.TexCoord = input.TexCoord;
    output.Diffuse = input.Diffuse;
    // ライティングは元の位置で行う（影・ライト・GI が本体の描画と一致する）。
    output.WorldPos = worldPos.xyz;
    output.ViewPos = mul(worldPos, View).xyz;
    output.CameraPos = CameraPos;
    return output;
}
