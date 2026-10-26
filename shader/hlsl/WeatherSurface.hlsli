#include "noise.hlsl"

#ifndef WEATHER_SURFACE_HLSLI
#define WEATHER_SURFACE_HLSLI

// 不透明 PBR 面の天候（RenderQuality）：濡れ（Lagarde 2013 の「Water drop」シリーズ）、
// 動く雨の波紋を伴う水たまり、上向きの面の積雪。
// PostProcessParams の WeatherParams0/1 と LensParams1.x（時刻）を使う。

// セルのグリッド上の雨粒から広がる環状の波紋（xz の法線摂動）。
float2 RainRipples(float2 position, float time, float intensity)
{
    float2 result = 0.0f;
    [unroll] for (int layer = 0; layer < 2; ++layer)
    {
        float2 p = position * (layer == 0 ? 2.3f : 3.7f) + layer * 17.0f;
        float2 cell = floor(p);
        float2 local = frac(p) - 0.5f;
        float seed = Hash21(cell + layer * 31.0f);
        float phase = frac(time * (0.9f + seed * 0.6f) + seed);
        float2 center = (float2(Hash21(cell + 3.1f), Hash21(cell + 5.7f)) - 0.5f) * 0.6f;
        float2 offset = local - center;
        float distance = length(offset);
        float ring = sin((distance - phase * 0.45f) * 60.0f) * saturate(1.0f - abs(distance - phase * 0.45f) * 12.0f);
        float fade = (1.0f - phase) * step(seed, intensity);
        result += offset / max(distance, 0.001f) * ring * fade;
    }
    return result * 0.35f;
}

// materialData：（金属度、粗さ、F0）
void ApplyWeatherToSurface(float3 worldPos, inout float3 normal, inout float3 albedo, inout float3 materialData)
{
    float wetness = WeatherParams0.x;
    float puddleAmount = WeatherParams0.y;
    float snowAmount = WeatherParams0.z;
    float rain = WeatherParams0.w;
    if (wetness <= 0.001f && puddleAmount <= 0.001f && snowAmount <= 0.001f)
    {
        return;
    }
    float upFacing = saturate(normal.y);
    float noise = WeatherFbm(worldPos.xz * 0.35f);
    float metallic = saturate(materialData.r);

    // 濡れた面：多孔質の誘電体は水膜に光が閉じ込められて暗くなり、粗さが下がる。
    float porosity = (1.0f - metallic) * saturate(materialData.g * 1.5f);
    float wet = saturate(wetness * lerp(0.6f, 1.0f, upFacing));
    albedo = lerp(albedo, albedo * albedo, wet * porosity * 0.85f);
    materialData.g = lerp(materialData.g, min(materialData.g, 0.12f), wet);

    // 水たまり：平らな上向き面にたまり、鏡のような水面に雨の波紋が立つ。
    float puddle =
        smoothstep(1.0f - puddleAmount, 1.0f - puddleAmount + 0.12f, noise) * smoothstep(0.85f, 0.97f, normal.y);
    puddle *= saturate(puddleAmount * 4.0f);
    if (puddle > 0.001f)
    {
        float3 waterNormal = float3(0.0f, 1.0f, 0.0f);
        if (rain > 0.001f)
        {
            float2 ripple = RainRipples(worldPos.xz, LensParams1.x, rain);
            waterNormal = normalize(float3(ripple.x, 1.0f, ripple.y));
        }
        normal = normalize(lerp(normal, waterNormal, puddle));
        albedo = lerp(albedo, albedo * 0.45f, puddle);
        materialData.g = lerp(materialData.g, 0.02f, puddle);
        materialData.r = lerp(materialData.r, 0.0f, puddle);
        materialData.b = lerp(materialData.b, 0.02f, puddle);
    }

    // 上向きの面の雪。ノイズで縁を不規則にする。
    if (snowAmount > 0.001f)
    {
        float coverage = upFacing + (noise - 0.5f) * 0.35f;
        float snow = smoothstep(1.0f - snowAmount, 1.0f - snowAmount + 0.15f, coverage);
        albedo = lerp(albedo, float3(0.86f, 0.89f, 0.93f), snow);
        materialData.r = lerp(materialData.r, 0.0f, snow);
        materialData.g = lerp(materialData.g, 0.7f, snow);
        materialData.b = lerp(materialData.b, 0.04f, snow);
        normal = normalize(lerp(normal, float3(0.0f, 1.0f, 0.0f), snow * 0.5f));
    }
}
#endif
