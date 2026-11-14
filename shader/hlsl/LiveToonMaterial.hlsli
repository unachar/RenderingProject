#ifndef LIVE_TOON_MATERIAL_HLSLI
#define LIVE_TOON_MATERIAL_HLSLI

// ---------------------------------------------------------------------------------------------
// ライブトゥーンのマテリアルテクスチャ（学マス講演の Def / 顔陰影反転 / 質感ランプ / シェードカラー / レイヤーに相当）。
// ジオメトリ段（DeferredPS / modelshaderPS）で評価し、結果は
//   ・陰影のしきい値・ぼかし → GBuffer の Shadow.r / g（Forward は g_LiveToonShadow）へ
//   ・質感ランプ             → ベースカラーへ
// として既存の経路に流す。GBuffer の追加チャンネルもルートシグネチャの変更も不要。
//
//  Def テクスチャ   : R = 影しきい値の補正（0.5 で変化なし。明るいほど影が出にくい）
//                     G = 境界のぼかし倍率、またはスムースネス（マテリアル設定で切替）
//                     B = 質感ランプのマスク（1 で全面に適用）
//  顔陰影反転       : R = 影側でも光を残す領域（頬など）。白いほど強く光を残す
//  質感ランプ       : 横軸 = 視線と法線の角度（左 = 正面、右 = 縁）。オーロラ・偏光塗料風の色変化
// ---------------------------------------------------------------------------------------------

float4 SampleLiveToonMap(uint index, SamplerState materialSampler, float2 uv)
{
#ifdef SHADER_VISIBILITY_RESOLVE
    return SampleMaterialGradCommon(LoadMaterialTexture(index), materialSampler, uv, VisibilityUvDx, VisibilityUvDy);
#else
    return SampleMaterialBiasCommon(LoadMaterialTexture(index), materialSampler, uv);
#endif
}

struct LiveToonMaterialSurface
{
    float3 Albedo;
    float ShadowThreshold;
    float ShadowSoftness;
    float Roughness;
    LiveToonExtra Extra; // ApplyLiveToonMaterialTextures が必ず設定する
};

// textures2 : x = シェードカラー（RGB = 影色, A = 肌マスク）, y = レイヤー（汗・髪の乱れ。左半分 = カラー+ブレンド,
//             右半分 = Def）, z = bit0-7 キャラ陰影ランプのスロット / bit8-15 質感ランプ行, w = bit0 Def を学マス配置で読む
// parameters2: x = シェードカラーの強さ, y = レイヤーのブレンド量（アニメーション可）, z = リムマスクの効き,
//              w = スペキュラマスクの効き
void ApplyLiveToonMaterialTextures(uint4 textures, float4 parameters, uint4 textures2, float4 parameters2, float2 uv,
                                   float3 normal, float3 viewDir, float materialClass, float metallic,
                                   SamplerState materialSampler, inout LiveToonMaterialSurface surface)
{
    surface.Extra = LiveToonExtraDefault();
    if (!IsLiveToonClass(materialClass) || IsLiveToonEyeClass(materialClass))
    {
        return;
    }
    const bool gakumasDef = (textures2.w & 1u) != 0u;
    const uint rampSlot = textures2.z & 0xffu;
    const uint rampRow = (textures2.z >> 8) & 0xffu;
    const bool hasShade = textures2.x < 512u;
    const bool hasLayer = textures2.y < 512u && parameters2.y > 0.0f;
    const bool hasDef = textures.x < 512u && parameters.x > 0.0f;

    // 学マス配置の Def（R = 陰影オフセット & リム, G = スムースネス, B = メタリック, A = スペキュラマスク）
    float4 defValue = float4(0.5f, 1.0f - surface.Roughness, metallic, 1.0f);
    float rampMask = 1.0f;
    [branch] if (hasDef)
    {
        float4 def = SampleLiveToonMap(textures.x, materialSampler, uv);
        if (gakumasDef)
        {
            defValue = lerp(defValue, def, saturate(parameters.x));
        }
        else
        {
            // 従来の配置（R = しきい値補正, G = ぼかし or スムースネス, B = 質感ランプのマスク）
            surface.ShadowThreshold = saturate(surface.ShadowThreshold - (def.r - 0.5f) * parameters.x);
            if ((textures.w & 1u) != 0u)
            {
                surface.Roughness = lerp(surface.Roughness, 1.0f - def.g, saturate(parameters.x));
                defValue.g = 1.0f - surface.Roughness;
            }
            else
            {
                surface.ShadowSoftness *= lerp(1.0f, max(def.g * 2.0f, 0.05f), saturate(parameters.x));
            }
            rampMask = lerp(1.0f, def.b, saturate(parameters.x));
        }
    }

    // レイヤーテクスチャ（汗・髪の乱れ）：2:1 の 1 枚に、左半分 = カラー（A = 下地とのブレンド）、右半分 = Def。
    // UV は本体と同じ（このエンジンの頂点は UV を 1 組だけ持つ）。
    [branch] if (hasLayer)
    {
        float2 layerUv = float2(frac(uv.x) * 0.5f, uv.y);
        float4 layerColor = SampleLiveToonMap(textures2.y, materialSampler, layerUv);
        float4 layerDef = SampleLiveToonMap(textures2.y, materialSampler, layerUv + float2(0.5f, 0.0f));
        float weight = saturate(layerColor.a * parameters2.y);
        surface.Albedo = lerp(surface.Albedo, layerColor.rgb, weight);
        defValue = lerp(defValue, layerDef, weight);
    }

    float rimMask = 1.0f;
    float specularMask = 1.0f;
    if (gakumasDef || hasLayer)
    {
        surface.ShadowThreshold = saturate(surface.ShadowThreshold - (defValue.r - 0.5f));
        surface.Roughness = saturate(1.0f - defValue.g);
        metallic = saturate(defValue.b);
        specularMask = lerp(1.0f, saturate(defValue.a), saturate(parameters2.w));
        // 陰影オフセット 0.0-0.5 を 0-1 へ（凹んだ所＝陰になりやすい所にはリムを乗せない）
        rimMask = lerp(1.0f, saturate(defValue.r * 2.0f), saturate(parameters2.z));
    }

    [branch] if (textures.y < 512u && parameters.y > 0.0f)
    {
        float faceLight = SampleLiveToonMap(textures.y, materialSampler, uv).r;
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

    const bool extended = hasShade || hasLayer || gakumasDef || rampSlot != 0u || rampRow != 0u;
    if (!extended)
    {
        return;
    }
    LiveToonExtra extra = LiveToonExtraDefault();
    extra.Valid = true;
    extra.Skin = abs(materialClass - 3.0f) < 0.5f;
    [branch] if (hasShade)
    {
        // シェードカラー：影色を直接描いたテクスチャ（落ち影も同じ色）。A は肌領域のマスク。
        float4 shade = SampleLiveToonMap(textures2.x, materialSampler, uv);
        extra.HasShade = true;
        extra.ShadeColor = lerp(surface.Albedo, shade.rgb, saturate(parameters2.x));
        extra.Skin = shade.a > 0.5f;
    }
    extra.RampSlot = rampSlot;
    extra.TextureRampRow = rampRow;
    extra.RimMask = rimMask;
    extra.Metallic = metallic;
    extra.SpecularMask = specularMask;
    surface.Extra = extra;
}
#endif



