#ifndef LIVE_TOON_MATERIAL_HLSLI
#define LIVE_TOON_MATERIAL_HLSLI

// ---------------------------------------------------------------------------------------------
// ライブトゥーンのマテリアルテクスチャ（学マス講演の Def / 顔陰影反転 / 質感ランプに相当）。
// ジオメトリ段（DeferredPS / modelshaderPS）で評価し、結果は
//   ・陰影のしきい値・ぼかし → GBuffer の Shadow.r / g（Forward は g_LiveToonShadow）へ
//   ・質感ランプ             → ベースカラーへ
// として既存の経路に流す。GBuffer の追加チャンネルもルートシグネチャの変更も不要。
//
//  Def テクスチャ   : R = 影しきい値の補正（0.5 で変化なし。明るいほど影が出にくい）
//                     G = 境界のぼかし倍率（0.5 で等倍、1 で 2 倍）
//                     B = 質感ランプのマスク（1 で全面に適用）
//  顔陰影反転       : R = 影側でも光を残す領域（頬など）。白いほど強く光を残す
//  質感ランプ       : 横軸 = 視線と法線の角度（左 = 正面、右 = 縁）。オーロラ・偏光塗料風の色変化
// ---------------------------------------------------------------------------------------------

struct LiveToonMaterialSurface
{
    float3 Albedo;
    float ShadowThreshold;
    float ShadowSoftness;
};

void ApplyLiveToonMaterialTextures(uint4 textures, float4 parameters, float2 uv, float3 normal, float3 viewDir,
                                   float materialClass, SamplerState materialSampler,
                                   inout LiveToonMaterialSurface surface)
{
    if (!IsLiveToonClass(materialClass))
    {
        return;
    }
    float rampMask = 1.0f;
    [branch] if (textures.x < 512u && parameters.x > 0.0f)
    {
        float4 def = LoadMaterialTexture(textures.x).Sample(materialSampler, uv);
        surface.ShadowThreshold = saturate(surface.ShadowThreshold - (def.r - 0.5f) * parameters.x);
        surface.ShadowSoftness *= lerp(1.0f, max(def.g * 2.0f, 0.05f), saturate(parameters.x));
        rampMask = lerp(1.0f, def.b, saturate(parameters.x));
    }
    [branch] if (textures.y < 512u && parameters.y > 0.0f)
    {
        float faceLight = LoadMaterialTexture(textures.y).Sample(materialSampler, uv).r;
        // しきい値を 0 へ寄せる：影側（N・L < 0）でも光が残る。真後ろからの光では従来どおり影になる。
        surface.ShadowThreshold = lerp(surface.ShadowThreshold, 0.0f, saturate(faceLight * parameters.y));
    }
    [branch] if (textures.z < 512u && (parameters.z > 0.0f || parameters.w > 0.0f))
    {
        float facing = saturate(dot(normal, viewDir));
        float u = clamp(1.0f - facing, 0.02f, 0.98f);
        float3 ramp = LoadMaterialTexture(textures.z).SampleLevel(materialSampler, float2(u, 0.5f), 0.0f).rgb;
        float3 multiplied = lerp(surface.Albedo, surface.Albedo * ramp * 2.0f, saturate(parameters.z) * rampMask);
        surface.Albedo = saturate(multiplied + ramp * max(parameters.w, 0.0f) * rampMask);
    }
}
#endif
