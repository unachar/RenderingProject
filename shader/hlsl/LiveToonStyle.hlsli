#ifndef LIVE_TOON_STYLE_HLSLI
#define LIVE_TOON_STYLE_HLSLI

// ---------------------------------------------------------------------------------------------
// ライブ用トゥーンスタイル（学マスのライブシーン風）。ToonLighting.hlsli の拡張。
// パラメーターはシーン全体の LightParams.LiveToonStyle0..5（source/rendering/livetoonstyle.h）。
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

// キャラ別陰影ランプ（LiveToonStyle6/7 の表。slot 1..6）のテクスチャ番号。無効なら < 0。
float LiveToonCharacterRampIndex(uint slot)
{
    if (slot == 0u || slot > 6u)
    {
        return -1.0f;
    }
    if (slot <= 4u)
    {
        return slot == 1u ? LiveToonStyle6.x : (slot == 2u ? LiveToonStyle6.y : (slot == 3u ? LiveToonStyle6.z : LiveToonStyle6.w));
    }
    return slot == 5u ? LiveToonStyle7.x : LiveToonStyle7.y;
}

// 陰影ランプ。横軸 = 光量（左 = 影、右 = 光）。
//   キャラ別（学マス方式、1024x4 の 1 枚）: RGB = 肌の乗算色、A = 肌以外のカラー↔シェードカラーの補間係数
//   シーン共通: 従来どおり（上半分 = 肌、下半分 = 肌以外。PackedShadowRamp なら学マス方式の 1 段）
// 戻り値の w が 0 のときはランプ無効。
float4 LiveToonShadowRamp(float lit, bool skin, float3 albedo, float3 shadedAlbedo)
{
#ifdef MEGALIGHTS_LIGHT_TEXTURES
    const float characterIndex = g_LiveToonExtra.Valid ? LiveToonCharacterRampIndex(g_LiveToonExtra.RampSlot) : -1.0f;
    const bool characterRamp = characterIndex >= 0.0f;
    const float sceneIndex = LiveToonStyle4.x;
    [branch] if ((characterRamp || sceneIndex >= 0.0f) && LiveToonStyle4.y > 0.0f)
    {
        uint index = (uint)(characterRamp ? characterIndex : sceneIndex);
        bool packedRamp = characterRamp || LiveToonStyle4.z > 0.5f;
        float2 uv = float2(clamp(lit, 0.01f, 0.99f), packedRamp ? 0.5f : (skin ? 0.25f : 0.75f));
        float4 ramp = MegaLightTextures[NonUniformResourceIndex(index)].SampleLevel(TextureSampler, uv, 0.0f);
        float3 color = albedo * ramp.rgb;
        if (packedRamp && !skin)
        {
            color = lerp(shadedAlbedo, albedo, saturate(ramp.a));
        }
        return float4(color, saturate(LiveToonStyle4.y));
    }
#endif
    return float4(1.0f, 1.0f, 1.0f, 0.0f);
}

// 影領域の色。乗算色で色相をずらし、彩度を少し上げて「濁らない影」にする。
float3 LiveToonShadowColor(float3 albedo, bool skin, float strength)
{
    // シェードカラーテクスチャ（学マス方式）：描かれた影色をそのまま使う（肌はランプの乗算で作る）。
    if (g_LiveToonExtra.Valid && g_LiveToonExtra.HasShade && !skin)
    {
        return g_LiveToonExtra.ShadeColor;
    }
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
    bool skin = LiveToonIsSkin(materialClass);
    bool hair = abs(materialClass - 1.0f) < 0.5f;
    float nDotL = dot(normal, lightDir);
    // リムマスク（Def の R の陰影オフセット由来）、スペキュラマスク（Def の A）、ライトごとのスペキュラ倍率
    const float rimMask = LiveToonRimMask();
    const float specularGain = LiveToonSpecularMask() * g_LightSpecularScale;
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
        // Local lights use visibility directly, independent of the directional cel boundary.
        const float shadowVisibility = saturate(LiveToonLuminance(visibility));
        if (shadowVisibility < 0.999f)
        {
            const float3 localShade = g_LiveToonExtra.Valid && g_LiveToonExtra.HasShade ? g_LiveToonExtra.ShadeColor
                                                                                       : float3(0.0f, 0.0f, 0.0f);
            const float4 shadowRamp = LiveToonShadowRamp(shadowVisibility, skin, albedo, localShade);
            const float3 rampLight = shadowRamp.rgb * skinRadiance * diffuse * max(LiveToonStyle0.w, 0.0f);
            const float rampWeight = shadowRamp.w * saturate(LiveToonStyle3.w) *
                                     (1.0f - smoothstep(0.9f, 1.0f, shadowVisibility));
            additive = lerp(additive, rampLight, rampWeight);
        }

        // ステージの色ライトが輪郭に乗る強いリム（逆光側ほど強い）
        float hardRim = LiveToonBand(edge, 1.0f - rimWidth * 0.55f, 0.03f);
        hardRim *= LiveToonBand(saturate(nDotL + 0.35f), 0.25f, 0.2f);
        additive += rimAlbedo * rimLight * visibility * hardRim * max(LiveToonStyle2.y, 0.0f) * (skin ? 0.5f : 1.0f) *
                    rimMask;
        float3 halfDir = LiveToonNormalize(lightDir + viewDir, normal);
        float highlight = pow(saturate(dot(normal, halfDir)), lerp(80.0f, 16.0f, saturate(roughness)));
        highlight = LiveToonBand(highlight, 0.55f, 0.06f) * saturate(nDotL);
        additive += LiveToonSpecularTint(albedo, 0.45f) * radiance * visibility * highlight *
                    max(LiveToonStyle5.x, 0.0f) * (skin ? 0.25f : 1.0f) * specularGain;
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
    if (g_LiveToonExtra.Valid && g_LiveToonExtra.HasShade && !skin)
    {
        shadowLevel = 1.0f; // 描かれたシェードカラーは明るさ込みの色
    }
    float3 diffuse = lerp(shadowColor * shadowLevel, albedo, lit) * skinRadiance;
    // 陰影ランプ：ベタ影 → ランプの色のグラデーション（肌と肌以外を 1 枚で描き分け）
    // The packed ramp shapes the N·L transition before the cel threshold.
    // Legacy two-row ramps keep their existing post-threshold coordinate.
    float rampCoordinate = LiveToonStyle4.z > 0.5f
                               ? saturate(coordinate + 0.5f - shadowThreshold) * cast
                               : lit;
    float4 shadowRamp = LiveToonShadowRamp(rampCoordinate, skin, albedo, shadowColor * shadowLevel);
    diffuse = lerp(diffuse, shadowRamp.rgb * skinRadiance, shadowRamp.w);
    // 色付きの透過影（ガラス越し）は色だけ反映
    float3 visibilityTint = visibility / max(castVisibility, 1e-3f);
    diffuse *= lerp(float3(1.0f, 1.0f, 1.0f), saturate(visibilityTint), step(1e-3f, castVisibility));

    // ランプ（段付き）スペキュラ
    float3 halfDir = LiveToonNormalize(lightDir + viewDir, normal);
    float specularPower = lerp(80.0f, 16.0f, saturate(roughness));
    float specular = pow(saturate(dot(normal, halfDir)), specularPower);
    float ramp = LiveToonBand(specular, 0.55f, 0.06f) * 0.7f + LiveToonBand(specular, 0.85f, 0.03f) * 0.3f;
    float rampStrength = max(LiveToonStyle3.x, 0.0f) * (skin ? 0.15f : (hair ? 0.6f : 1.0f));
    float3 specularTerm = LiveToonSpecularTint(albedo, 0.5f) * radiance * ramp * rampStrength * lit * specularGain;

#ifdef MEGALIGHTS_LIGHT_TEXTURES
    [branch] if (LiveToonStyle4.w > 0.0f && rampStrength > 0.0f)
    {
        uint rampCode = (uint)LiveToonStyle4.w;
        bool usePartRows = rampCode > 512u;
        uint index = (rampCode - 1u) % 512u;
        float rampY = 0.5f;
        const uint explicitRow = g_LiveToonExtra.Valid ? g_LiveToonExtra.TextureRampRow : 0u;
        if (explicitRow > 0u)
        {
            // マテリアルで行を指定（学マス：頂点カラーの質感ランプ ID に相当。縦に LiveToonStyle7.z 行）
            uint width, height;
            MegaLightTextures[NonUniformResourceIndex(index)].GetDimensions(width, height);
            uint rowCount = (uint)clamp(LiveToonStyle7.z, 1.0f, 16.0f);
            uint row = min(explicitRow - 1u, rowCount - 1u);
            float rowHeight = (float)height / (float)rowCount;
            // 行の中央のテクセルを読む（隣の行と混ざらない）
            rampY = (floor(row * rowHeight + rowHeight * 0.5f) + 0.5f) / (float)height;
        }
        else if (usePartRows)
        {
            uint width, height;
            MegaLightTextures[NonUniformResourceIndex(index)].GetDimensions(width, height);
            if (height >= 4u && height % 4u == 0u)
            {
                // Align to a texel center so even a four-pixel-high ramp cannot blend adjacent rows.
                uint row = (uint)clamp(round(materialClass) - 1.0f, 0.0f, 3.0f);
                uint rowHeight = height / 4u;
                rampY = (row * rowHeight + rowHeight / 2u + 0.5f) / height;
            }
        }
        float2 uv = float2(clamp(specular, 0.01f, 0.99f), rampY);
        float4 response = MegaLightTextures[NonUniformResourceIndex(index)].SampleLevel(TextureSampler, uv, 0.0f);
        // A：0 = スペキュラへ乗算、1 = ディフューズへ加算、中間 = ブレンド（学マスの質感ランプ）
        float diffuseMix = saturate(response.a);
        specularTerm *= response.rgb * (1.0f - diffuseMix);
        diffuse += response.rgb * radiance * (diffuseMix * rampStrength * lit * LiveToonSpecularMask());
    }
#endif

    if (hair)
    {
        float ring = LiveToonAngelRing(normal, viewDir, lightDir);
        specularTerm += lerp(albedo, float3(1.0f, 1.0f, 1.0f), 0.6f) * radiance * ring * cast *
                        max(LiveToonStyle3.y, 0.0f) * specularGain;
    }

    // ふわっとしたリム（光源側に寄せる）＋ 輪郭を締める強いリム（逆光側）
    float softRim = smoothstep(1.0f - rimWidth, 1.0f, edge) * saturate(nDotL * 0.5f + 0.5f);
    float hardRim = LiveToonBand(edge, 1.0f - rimWidth * 0.45f, 0.025f) *
                    LiveToonBand(-dot(viewDir, lightDir), -0.1f, 0.3f);
    float3 rim = rimAlbedo * rimLight * cast *
                 (softRim * max(LiveToonStyle2.x, 0.0f) + hardRim * max(LiveToonStyle2.y, 0.0f)) *
                 (skin ? 0.45f : 1.0f) * rimMask;

    float3 backlight = albedo * skinRadiance * saturate(-nDotL) * max(LiveToonStyle5.y, 0.0f);
    return diffuse + specularTerm + rim + backlight;
}

// 従来のトゥーンとライブスタイルを LiveToonStyle0.x でブレンドする入口。
float3 EvaluateLiveToonLightStyled(float3 albedo, float3 normal, float3 viewDir, float3 lightDir, float3 radiance,
                                   float3 visibility, float lightType, float materialClass, float roughness,
                                   float shadowThreshold, float shadowSoftness, float shadowStrength)
{
    if (IsLiveToonEyeClass(materialClass))
    {
        // White and iris keep a readable base in shadow. Authored glints use their own fill only.
        bool sclera = materialClass < 15.5f;
        bool glint = materialClass >= 16.5f;
        float facing = smoothstep(-0.15f, 0.75f, dot(normal, lightDir));
        float luminance = LiveToonLuminance(radiance);
        float3 softColor = lerp(luminance.xxx, radiance, sclera ? 0.25f : 0.45f);
        return glint ? 0.0f : albedo * softColor * visibility * facing * (sclera ? 0.25f : 0.32f);
    }
    float blend = saturate(LiveToonStyle0.x);
    float3 legacy = 0.0f;
    [branch] if (blend < 0.999f)
    {
        legacy = EvaluateLiveToonLight(albedo, normal, viewDir, lightDir, radiance * visibility, materialClass,
                                       roughness, shadowThreshold, shadowSoftness, shadowStrength);
    }
    [branch] if (blend <= 0.001f)
    {
        return legacy;
    }
    float3 live = EvaluateLiveToonStageLight(albedo, normal, viewDir, lightDir, radiance, visibility, lightType,
                                             materialClass, roughness, shadowThreshold, shadowSoftness, shadowStrength);
    return lerp(legacy, live, blend);
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
    bool skin = LiveToonIsSkin(materialClass);
    float fresnel = pow(1.0f - saturate(dot(normal, viewDir)), 4.0f);
    float3 tint = LiveToonSpecularTint(albedo, 0.6f);
    // メタリックは正面からも映り込む（学マスの Def B）
    float reflectance = lerp(0.04f + 0.96f * fresnel, 1.0f,
                             g_LiveToonExtra.Valid ? g_LiveToonExtra.Metallic * 0.75f : 0.0f);
    return tint * environment * reflectance * strength * (skin ? 0.35f : 1.0f) * LiveToonSpecularMask();
}

// 影側の環境光にも乗算色を入れて、陰の色が環境光で白く濁らないようにする。
float3 EvaluateLiveToonAmbientStyled(float3 albedo, float3 environmentIrradiance, float materialClass)
{
    if (IsLiveToonEyeClass(materialClass))
    {
        float fill = materialClass < 15.5f ? 0.42f : (materialClass < 16.5f ? 0.52f : 0.85f);
        return albedo * fill;
    }
    float3 legacy = EvaluateLiveToonAmbient(albedo, environmentIrradiance, materialClass);
    float blend = saturate(LiveToonStyle0.x);
    if (blend <= 0.001f)
    {
        return legacy;
    }
    bool skin = LiveToonIsSkin(materialClass);
    float3 tinted = EvaluateLiveToonAmbient(LiveToonShadowColor(albedo, skin, 1.0f), environmentIrradiance,
                                            materialClass);
    return lerp(legacy, tinted, blend);
}
#endif


