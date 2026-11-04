#ifndef LIVE_TOON_STYLE_HLSLI
#define LIVE_TOON_STYLE_HLSLI

// ---------------------------------------------------------------------------------------------
// ライブ用トゥーンスタイル（学マスのライブシーン風）。ToonLighting.hlsli の拡張。
// パラメーターはシーン全体の LightParams.LiveToonStyle0..3（source/rendering/livetoonstyle.h）。
// Forward（modelshaderPS）と Deferred（DeferredLightingPS）の両方が SurfaceLighting.hlsli 経由で
// 同じ関数を使うので、経路による見た目の差は出ない。
//
//  メインライト（平行光源）: セル陰影 + 影の乗算色 + 落ち影の濃さ + ランプスペキュラ + 天使の輪 + リム
//  加算ライト（点 / スポット / 矩形）: 陰影の境界を作らず、距離減衰した明るさを柔らかく足す
//                                      （ステージの色ライトが輪郭に乗るハードリムもここで加える）
// すべての項は albedo と radiance に線形。HDR のまま返し、トーンマップは後段に任せる。
// ---------------------------------------------------------------------------------------------

float LiveToonLuminance(float3 value)
{
    return max(dot(value, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}

float3 LiveToonSaturate(float3 color, float amount)
{
    float l = LiveToonLuminance(color);
    return max(lerp(float3(l, l, l), color, amount), 0.0f);
}

// 陰影ランプ（シーン全体）。横軸 = 光量（左 = 影、右 = 光）、上半分 = 肌、下半分 = 肌以外。
// 戻り値の w が 0 のときはランプ無効。
float4 LiveToonShadowRamp(float lit, bool skin)
{
#ifdef MEGALIGHTS_LIGHT_TEXTURES
    [branch] if (LiveToonStyle4.x >= 0.0f && LiveToonStyle4.y > 0.0f)
    {
        uint index = (uint)LiveToonStyle4.x;
        float2 uv = float2(clamp(lit, 0.01f, 0.99f), skin ? 0.25f : 0.75f);
        float3 ramp = MegaLightTextures[NonUniformResourceIndex(index)].SampleLevel(TextureSampler, uv, 0.0f).rgb;
        return float4(ramp, saturate(LiveToonStyle4.y));
    }
#endif
    return float4(1.0f, 1.0f, 1.0f, 0.0f);
}

// 影領域の色。乗算色で色相をずらし、彩度を少し上げて「濁らない影」にする。
float3 LiveToonShadowColor(float3 albedo, bool skin, float strength)
{
    float tintAmount = saturate(LiveToonStyle0.y) * saturate(strength);
    float3 tint = lerp(float3(1.0f, 1.0f, 1.0f), saturate(LiveToonStyle1.rgb), tintAmount);
    if (skin)
    {
        // 肌の影は暖色へ（血色）。青紫の乗算色をそのまま肌に掛けると顔色が悪く見える。
        float3 warm = float3(1.0f, 0.80f, 0.76f);
        tint = lerp(tint, warm, saturate(LiveToonStyle1.w));
    }
    return LiveToonSaturate(albedo * tint, 1.0f + 0.25f * tintAmount);
}

// 髪の天使の輪（Kajiya-Kay 風の異方性バンド）。髪束はおおむね縦方向に流れる前提で、
// 接線を「上方向を法線に直交化したもの」として近似する（UV 接線に依存しないので PMX / VRM / FBX 共通）。
float LiveToonAngelRing(float3 normal, float3 viewDir, float3 lightDir)
{
    float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 tangent = up - normal * dot(normal, up);
    float tangentLength = dot(tangent, tangent);
    if (tangentLength < 1e-4f)
    {
        return 0.0f;
    }
    tangent *= rsqrt(tangentLength);
    float3 halfDir = LiveToonNormalize(lightDir + viewDir, normal);
    float tDotH = dot(tangent, halfDir);
    float sinTH = sqrt(saturate(1.0f - tDotH * tDotH));
    float ring = pow(sinTH, 96.0f);
    // 段のついた帯にする（アニメ塗りのハイライト）
    return LiveToonBand(ring, 0.55f, 0.08f) * saturate(dot(normal, lightDir) + 0.2f);
}

// radiance: 影を含まない放射輝度（強度・距離減衰・大気を含む）
// visibility: 影の可視率（色付き透過影を含む）
// lightType : LightPositionType.w（0 = 平行光源）
float3 EvaluateLiveToonStageLight(float3 albedo, float3 normal, float3 viewDir, float3 lightDir, float3 radiance,
                                  float3 visibility, float lightType, float materialClass, float roughness,
                                  float shadowThreshold, float shadowSoftness, float shadowStrength)
{
    bool skin = abs(materialClass - 3.0f) < 0.5f;
    bool hair = abs(materialClass - 1.0f) < 0.5f;
    float nDotL = dot(normal, lightDir);
    float nDotV = saturate(dot(normal, viewDir));
    float edge = 1.0f - nDotV;
    float rimWidth = clamp(LiveToonStyle2.z, 0.02f, 1.0f);
    float radianceLuma = LiveToonLuminance(radiance);
    float3 rimLight = lerp(radianceLuma.xxx, radiance, saturate(LiveToonStyle2.w));
    float3 rimAlbedo = lerp(albedo, float3(1.0f, 1.0f, 1.0f), 0.45f);

    // ---- 加算ライト：境界を作らない柔らかい明るさ ----
    if (lightType > 0.5f)
    {
        float wrap = saturate(LiveToonStyle0.z);
        float diffuse = saturate((nDotL + wrap) / (1.0f + wrap));
        // 回り込みが大きいほど「ライトの近くにいるだけで明るい」ボリューム感になる
        diffuse = lerp(diffuse, 0.5f + 0.5f * diffuse, wrap * 0.5f);
        float3 skinRadiance = skin ? lerp(radianceLuma.xxx, radiance, 0.7f) : radiance;
        float3 additive = albedo * skinRadiance * visibility * diffuse * max(LiveToonStyle0.w, 0.0f);

        // ステージの色ライトが輪郭に乗る強いリム（逆光側ほど強い）
        float hardRim = LiveToonBand(edge, 1.0f - rimWidth * 0.55f, 0.03f);
        hardRim *= LiveToonBand(saturate(nDotL + 0.35f), 0.25f, 0.2f);
        additive += rimAlbedo * rimLight * visibility * hardRim * max(LiveToonStyle2.y, 0.0f) * (skin ? 0.5f : 1.0f) * g_ToonRimScale;
        return additive;
    }

    // ---- トゥーン陰影用メインライト ----
    float wrapMain = skin ? 0.30f : 0.10f;
    float coordinate = saturate((nDotL + wrapMain) / (1.0f + wrapMain));
    float softness = max(shadowSoftness, skin ? 0.06f : 0.015f);
    float litBand = LiveToonBand(coordinate, saturate(shadowThreshold), softness);
    // 落ち影：陰影と同じ乗算色の領域にまとめる（濃さだけ独立に調整）
    float castVisibility = LiveToonLuminance(visibility);
    float cast = lerp(1.0f, saturate(castVisibility), saturate(LiveToonStyle3.w));
    float lit = litBand * cast;

    float3 skinRadiance = skin ? lerp(radianceLuma.xxx, radiance, 0.55f) : radiance;
    float3 shadowColor = LiveToonShadowColor(albedo, skin, shadowStrength);
    // 影の中の光量（真っ黒にしない下限）。影色は乗算色で作るので明るさは控えめに落とすだけ
    float shadowLevel = lerp(1.0f, skin ? 0.62f : 0.48f, saturate(shadowStrength));
    float3 diffuse = lerp(shadowColor * shadowLevel, albedo, lit) * skinRadiance;
    // 陰影ランプ：ベタ影 → ランプの色のグラデーション（肌と肌以外を 1 枚で描き分け）
    float4 shadowRamp = LiveToonShadowRamp(lit, skin);
    diffuse = lerp(diffuse, albedo * shadowRamp.rgb * skinRadiance, shadowRamp.w);
    // 色付きの透過影（ガラス越し）は色だけ反映
    float3 visibilityTint = visibility / max(castVisibility, 1e-3f);
    diffuse *= lerp(float3(1.0f, 1.0f, 1.0f), saturate(visibilityTint), step(1e-3f, castVisibility));

    // ランプ（段付き）スペキュラ
    float3 halfDir = LiveToonNormalize(lightDir + viewDir, normal);
    float specularPower = lerp(80.0f, 16.0f, saturate(roughness));
    float specular = pow(saturate(dot(normal, halfDir)), specularPower);
    float ramp = LiveToonBand(specular, 0.55f, 0.06f) * 0.7f + LiveToonBand(specular, 0.85f, 0.03f) * 0.3f;
    float rampStrength = max(LiveToonStyle3.x, 0.0f) * (skin ? 0.15f : (hair ? 0.6f : 1.0f));
    float3 specularTerm = lerp(albedo, float3(1.0f, 1.0f, 1.0f), 0.5f) * radiance * ramp * rampStrength * lit;

    if (hair)
    {
        float ring = LiveToonAngelRing(normal, viewDir, lightDir);
        specularTerm += lerp(albedo, float3(1.0f, 1.0f, 1.0f), 0.6f) * radiance * ring * cast *
                        max(LiveToonStyle3.y, 0.0f);
    }

    // ふわっとしたリム（光源側に寄せる）＋ 輪郭を締める強いリム（逆光側）
    float softRim = smoothstep(1.0f - rimWidth, 1.0f, edge) * saturate(nDotL * 0.5f + 0.5f);
    float hardRim = LiveToonBand(edge, 1.0f - rimWidth * 0.45f, 0.025f) *
                    LiveToonBand(-dot(viewDir, lightDir), -0.1f, 0.3f);
    float3 rim = rimAlbedo * rimLight * cast *
                 (softRim * max(LiveToonStyle2.x, 0.0f) + hardRim * max(LiveToonStyle2.y, 0.0f)) *
                 (skin ? 0.45f : 1.0f) * g_ToonRimScale;

    return diffuse + specularTerm + rim;
}

// 従来のトゥーンとライブスタイルを LiveToonStyle0.x でブレンドする入口。
float3 EvaluateLiveToonLightStyled(float3 albedo, float3 normal, float3 viewDir, float3 lightDir, float3 radiance,
                                   float3 visibility, float lightType, float materialClass, float roughness,
                                   float shadowThreshold, float shadowSoftness, float shadowStrength)
{
    // マテリアルグラフの Toon ノード（Toon Light Ramp / Face SDF / Rim / Hair）：ライト 1 灯ごとに、albedo / 影の強さ / 加算項を調整して
    // 既存の計算（セル陰影・ランプスペキュラ・天使の輪・リム）をそのまま使う。g_ToonNode.Mask == 0 なら従来どおり
    float3 nodeExtra = 0.0f;
#ifndef TOON_NODE_LIGHTING_DISABLED // Deferred（GBuffer に追加のチャンネルが無い）はノードのライトごとの評価を持たない
    [branch] if (g_ToonNode.Mask != 0u)
    {
        ToonNodeLightResult node = EvaluateToonNodeLight(g_ToonNode, albedo, normal, viewDir, lightDir, radiance, visibility, shadowStrength);
        albedo = node.Albedo;
        shadowStrength = node.ShadowStrength;
        visibility = node.Visibility;
        nodeExtra = node.Extra;
    }
#endif
    float blend = saturate(LiveToonStyle0.x);
    float3 legacy = 0.0f;
    [branch] if (blend < 0.999f)
    {
        legacy = EvaluateLiveToonLight(albedo, normal, viewDir, lightDir, radiance * visibility, materialClass,
                                       roughness, shadowThreshold, shadowSoftness, shadowStrength);
    }
    [branch] if (blend <= 0.001f)
    {
        return legacy + nodeExtra;
    }
    float3 live = EvaluateLiveToonStageLight(albedo, normal, viewDir, lightDir, radiance, visibility, lightType,
                                             materialClass, roughness, shadowThreshold, shadowSoftness, shadowStrength);
    return lerp(legacy, live, blend) + nodeExtra;
}

// 環境スペキュラ（IBL の縁の映り込み）。environment は反射方向でサンプルした環境色。
float3 EvaluateLiveToonEnvironmentSpecular(float3 albedo, float3 normal, float3 viewDir, float3 environment,
                                           float materialClass)
{
    float strength = max(LiveToonStyle3.z, 0.0f) * saturate(LiveToonStyle0.x);
    if (strength <= 0.0f)
    {
        return 0.0f;
    }
    bool skin = abs(materialClass - 3.0f) < 0.5f;
    float fresnel = pow(1.0f - saturate(dot(normal, viewDir)), 4.0f);
    float3 tint = lerp(albedo, float3(1.0f, 1.0f, 1.0f), 0.6f);
    return tint * environment * (0.04f + 0.96f * fresnel) * strength * (skin ? 0.35f : 1.0f);
}

// 影側の環境光にも乗算色を入れて、陰の色が環境光で白く濁らないようにする。
float3 EvaluateLiveToonAmbientStyled(float3 albedo, float3 environmentIrradiance, float materialClass)
{
    float3 legacy = EvaluateLiveToonAmbient(albedo, environmentIrradiance, materialClass);
    float blend = saturate(LiveToonStyle0.x);
    if (blend <= 0.001f)
    {
        return legacy;
    }
    bool skin = abs(materialClass - 3.0f) < 0.5f;
    float3 tinted = EvaluateLiveToonAmbient(LiveToonShadowColor(albedo, skin, 1.0f), environmentIrradiance,
                                            materialClass);
    return lerp(legacy, tinted, blend);
}
#endif

