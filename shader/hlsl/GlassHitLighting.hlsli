#ifndef GLASS_HIT_LIGHTING_INCLUDED
#define GLASS_HIT_LIGHTING_INCLUDED

// 二次ヒットの照明は、ラスタライズされたガラスのマテリアル、モニターやデカールのテクスチャ、
// 主ピクセルのライトタイル、デノイズ済みの太陽光から意図的に独立させる。
float3 GlassHitBRDF(float3 N, float3 V, float3 L, float3 color, float metallic, float roughness, float f0,
                    float diffuseWeight)
{
    const float PI = 3.14159265359f;
    float NoL = saturate(dot(N, L));
    float NoV = max(saturate(dot(N, V)), 1e-4f);
    if (NoL <= 0.0f)
    {
        return 0.0f;
    }
    float3 H = SafeNormalizeCommon(V + L, N);
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));
    float r = clamp(roughness, 0.04f, 1.0f);
    float a = r * r;
    float a2 = a * a;
    float denominator = NoH * NoH * (a2 - 1.0f) + 1.0f;
    float D = a2 / max(PI * denominator * denominator, 1e-7f);
    float k = (r + 1.0f) * (r + 1.0f) * 0.125f;
    float G = (NoL / max(NoL * (1 - k) + k, 1e-5f)) * (NoV / max(NoV * (1 - k) + k, 1e-5f));
    float dielectricF0 = saturate(f0);
    float3 F0 = lerp(float3(dielectricF0, dielectricF0, dielectricF0), color, saturate(metallic));
    float3 F = F0 + (1 - F0) * pow(1 - VoH, 5);
    float3 specular = D * G * F / max(4 * NoL * NoV, 1e-5f);
    float3 diffuse = (1 - F) * (1 - saturate(metallic)) * color * saturate(diffuseWeight) / PI;
    return (diffuse + specular) * NoL;
}

float3 GlassEvaluateWorldLight(float3 P, float3 N, float3 V, float3 color, float metallic, float roughness, float f0,
                               float diffuseWeight, float4 directionRange, float4 positionType, float4 extra,
                               float4 lightColor, bool castsShadow)
{
    float3 L;
    float attenuation, volume;
    ResolveSingleLightCommon(P, directionRange, positionType, extra, L, attenuation, volume);
    const bool rectangle = positionType.w > 3.5f;
    if (rectangle)
    {
        float3 toCenter = positionType.xyz - P;
        L = SafeNormalizeCommon(toCenter, N);
        float fade = saturate(1 - length(toCenter) / max(directionRange.w, 0.01f));
        attenuation = fade * fade;
    }
    if (attenuation <= 1e-6f || lightColor.a <= 0 || dot(N, L) <= 0)
    {
        return 0.0f;
    }
    float3 brdf;
    if (rectangle)
    {
        float3 emitNormal = SafeNormalizeCommon(directionRange.xyz, float3(0, -1, 0));
        brdf = EvaluateRectLightLtcTinted(N, V, P, positionType.xyz, emitNormal, extra.x, extra.y, color,
                                          float3(metallic, roughness, f0), saturate(diffuseWeight).xxx, 1.0f);
    }
    else
    {
        brdf = GlassHitBRDF(N, V, L, color, metallic, roughness, f0, diffuseWeight);
    }
    if (max(brdf.x, max(brdf.y, brdf.z)) <= 1e-6f)
    {
        return 0.0f;
    }
    float visibility = 1;
    if (castsShadow && SceneRayFlags.y > 0.5f && SceneRayFlags.z > 0.5f)
    {
        float range =
            positionType.w < 0.5f ? SceneRayParameters.x : min(length(positionType.xyz - P), SceneRayParameters.x);
        visibility = TraceSceneShadow(P, N, L, range);
    }
    return brdf * max(lightColor.rgb, 0) * max(lightColor.a, 0) * attenuation * visibility;
}

float3 GlassEvaluateWorldDirect(float3 P, float3 N, float3 V, float3 color, float metallic, float roughness, float f0,
                                float diffuseWeight)
{
    float3 result = 0;
    bool megaActive = MLForwardGridActive();
    uint count = (uint)clamp((int)round(LightCount.x), 0, MAX_SHADER_LIGHTS);
    [loop] for (uint i = 0; i < count; ++i)
    {
        // デカールやモニターのライトにはラスターピクセルが必要なので、別のヒット面で
        // ガラス描画時の座標を使ってサンプリングしない。
        if (LightFlags[i].x < 0.5f || LightFlags[i].w >= 0.5f)
        {
            continue;
        }
        if (megaActive && LightFlags[i].x > 1.5f)
        {
            continue;
        }
        bool shadow = SceneLightCastsShadowCommon(i, LightShadowData[i].x >= -0.5f);
        result += GlassEvaluateWorldLight(P, N, V, color, metallic, roughness, f0, diffuseWeight, LightDirections[i],
                                          LightPositionTypes[i], LightExtras[i], LightColors[i], shadow);
    }
    if (megaActive)
    {
        // セルは主ガラスピクセルではなく、二次ヒットのワールド位置から求める。
        // 画面外のヒットではライトリスト全体へフォールバックする。
        uint cellIndex = MLCellIndexFromWorld(P);
        uint2 cell = uint2(0, MLCounts.x);
        bool hasCell = cellIndex != ML_INVALID_CELL;
        if (hasCell)
        {
            cell = MegaLightCells[cellIndex];
        }
        [loop] for (uint j = 0; j < cell.y; ++j)
        {
            uint index = hasCell ? MegaLightIndices[cell.x + j] : j;
            if (index >= MLCounts.x)
            {
                continue;
            }
            MegaLightData light = MegaLightBuffer[index];
            if (!MLLightAffectsForward(light))
            {
                continue;
            }
            result +=
                GlassEvaluateWorldLight(P, N, V, color, metallic, roughness, f0, diffuseWeight, light.DirectionRange,
                                        light.PositionType, light.Extra, light.Color, MLLightCastsShadow(light));
        }
    }
    return max(result, 0.0f);
}
#endif
