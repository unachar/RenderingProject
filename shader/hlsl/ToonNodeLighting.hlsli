#ifndef TOON_NODE_LIGHTING_HLSLI
#define TOON_NODE_LIGHTING_HLSLI
// =====================================================================================================
// マテリアルグラフの Toon ノードのライトごとの評価（Toon Light Ramp / Face SDF Shadow / Directional Rim Light /
// Hair Anisotropic Highlight）。ToonLighting.hlsli / LiveToonStyle.hlsli の既存のライティング経路に統合する：
//
//   画素ごと（ジオメトリ段 / フォワード）  マテリアルグラフ VM が、ノードの「パラメーター」を Ext 出力に評価する（ライトに依存しない）
//   ライトごと（EvaluateLiveToonLightStyled） このファイルの EvaluateToonNodeLight が、ライト方向 / 放射輝度 / 可視率に応じて
//                                            albedo / 影の強さ / 加算項を調整して、既存の陰影・スペキュラ・リムの計算をそのまま使う
//   → ライトが 2000 灯あっても、画素ごとの VM 評価は 1 回。ライトごとの追加コストは数十演算だけ（テクスチャも VM も読まない）
//
// Deferred の GBuffer にはこの追加パラメーターを入れる余地が無いため、Deferred では ApplyMaterialGraphToonDeferred で
// 近似（Shadow.r / g / b = 影のしきい値 / ぼかし / 強さ）だけを GBuffer に書く。完全な評価は Forward のトゥーン経路
// （IMPLEMENTATION_STATUS.md に記載）
// =====================================================================================================
#include "MaterialGraphNodesExt.hlsli"

#define MG_VM_WANT_EXT 1 // MaterialGraphVM.hlsli の出力に Ext（陰影ピン）を含める

#define TOON_NODE_RAMP 1u
#define TOON_NODE_RIM 2u
#define TOON_NODE_HAIR 4u
#define TOON_NODE_FACE 8u

struct ToonNodeProfile
{
    uint Mask; // 有効なノード（TOON_NODE_*）。0 = ノードなし（従来どおり）
    float4 Ramp;                 // (threshold, softness, steps, strength * mask)
    float3 RampColor1;           // 最も暗い帯
    float3 RampColor2;
    float3 RampColor3;           // 最も明るい帯
    float RampWrap;
    float4 RimParams;            // (width, power, strength, directional mask)
    float3 RimColor;
    float4 HairParams;           // (anisotropy, roughness, width, shift)
    float3 HairTangent;
    float HairStrength;
    float3 HairColor;
    float4 FaceParams;           // (sdfA, sdfB, threshold, smoothness)
    float3 FaceForward;
    float FaceFlip;
    float3 FaceRight;
    float FaceStrength;
    float3 FaceShadowColor;
};

// 画素ごとのプロファイル（Forward：ApplyMaterialGraphToonProfile が設定。Deferred は未設定のまま = 従来の見た目）
static ToonNodeProfile g_ToonNode;
// 既存のリム（EvaluateLiveToonLight / EvaluateLiveToonStageLight）の係数。Directional Rim Light が有効なら 0（二重にならない）
static float g_ToonRimScale = 1.0f;

struct ToonNodeLightResult
{
    float3 Albedo;
    float ShadowStrength;
    float3 Visibility;
    float3 Extra; // 加算項（リム / 髪のハイライト）
};

float ToonNodeLuma(float3 v)
{
    return dot(v, float3(0.2126f, 0.7152f, 0.0722f));
}

// ライト 1 灯ぶんの評価。radiance は影を含まない放射輝度、visibility は影の可視率
ToonNodeLightResult EvaluateToonNodeLight(ToonNodeProfile p, float3 albedo, float3 normal, float3 viewDir, float3 lightDir,
                                          float3 radiance, float3 visibility, float shadowStrength)
{
    ToonNodeLightResult r;
    r.Albedo = albedo;
    r.ShadowStrength = shadowStrength;
    r.Visibility = visibility;
    r.Extra = 0.0f;
    float castVisibility = saturate(ToonNodeLuma(visibility));
    float nDotL = dot(normal, lightDir);
    // 落ち影はランプの影色に反映する（可視率 0 = いちばん暗い帯）。ランプが無いときは従来どおり
    float nDotLEffective = lerp(-1.0f, nDotL, castVisibility);
    float3 shade = float3(1.0f, 1.0f, 1.0f);
    bool shaded = false;
    if ((p.Mask & TOON_NODE_RAMP) != 0u)
    {
        float q = MGX_ToonBand(nDotLEffective, p.RampWrap, p.Ramp.z, p.Ramp.x, p.Ramp.y);
        shade = lerp(float3(1, 1, 1), MGX_ToonRampColor(q, p.RampColor1, p.RampColor2, p.RampColor3), saturate(p.Ramp.w));
        shaded = true;
    }
    if ((p.Mask & TOON_NODE_FACE) != 0u)
    {
        // 顔の SDF 陰影：ランプの代わりに、顔の向きとライトの水平角で陰影が決まる（Strength = 顔のマスク）
        float lit = MGX_FaceSdfLit(p.FaceParams.x, p.FaceParams.y, p.FaceForward, p.FaceRight, lightDir, p.FaceParams.z, p.FaceParams.w,
                                   p.FaceFlip > 0.5f);
        lit *= castVisibility;
        float3 faceShade = lerp(p.FaceShadowColor, float3(1, 1, 1), lit);
        shade = lerp(shade, faceShade, saturate(p.FaceStrength));
        shaded = true;
    }
    if (shaded)
    {
        // 既存の陰影の計算を無効にして（ShadowStrength = 0 ならベタ塗りの lit 扱い）、ランプ / SDF の色を albedo に掛ける
        r.Albedo = albedo * shade;
        r.ShadowStrength = 0.0f;
        r.Visibility = float3(1, 1, 1); // 落ち影はすでに帯に反映した（二重に暗くしない）
    }
    float3 lit = radiance * castVisibility;
    if ((p.Mask & TOON_NODE_RIM) != 0u)
    {
        float rim = MGX_ToonRim(normal, viewDir, lightDir, p.RimParams.x, p.RimParams.y, p.RimParams.w) * p.RimParams.z;
        r.Extra += p.RimColor * rim * lit;
    }
    if ((p.Mask & TOON_NODE_HAIR) != 0u)
    {
        float h = MGX_HairHighlight(p.HairTangent, normal, viewDir, lightDir, p.HairParams.x, p.HairParams.y, p.HairParams.z, p.HairParams.w) *
                  p.HairStrength;
        r.Extra += p.HairColor * h * lit;
    }
    return r;
}

#endif
