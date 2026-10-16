#ifndef WEATHER_SURFACE_HLSLI
#define WEATHER_SURFACE_HLSLI
// Weather on opaque PBR surfaces (RenderQuality): wetness (Lagarde 2013 "Water drop" series),
// puddles with animated rain ripples, and snow cover on upward facing surfaces.
// Uses WeatherParams0/1 and LensParams1.x (time) from PostProcessParams.

float WeatherHash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float WeatherValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(WeatherHash(i), WeatherHash(i + float2(1, 0)), u.x),
        lerp(WeatherHash(i + float2(0, 1)), WeatherHash(i + float2(1, 1)), u.x), u.y);
}

float WeatherFbm(float2 p)
{
    return WeatherValueNoise(p) * 0.55f + WeatherValueNoise(p * 2.03f + 7.1f) * 0.3f + WeatherValueNoise(p * 4.11f + 3.7f) * 0.15f;
}

// Expanding ring ripples from rain drops on a grid of cells (normal perturbation in xz).
float2 RainRipples(float2 position, float time, float intensity)
{
    float2 result = 0.0f;
    [unroll] for (int layer = 0; layer < 2; ++layer)
    {
        float2 p = position * (layer == 0 ? 2.3f : 3.7f) + layer * 17.0f;
        float2 cell = floor(p);
        float2 local = frac(p) - 0.5f;
        float seed = WeatherHash(cell + layer * 31.0f);
        float phase = frac(time * (0.9f + seed * 0.6f) + seed);
        float2 center = (float2(WeatherHash(cell + 3.1f), WeatherHash(cell + 5.7f)) - 0.5f) * 0.6f;
        float2 offset = local - center;
        float distance = length(offset);
        float ring = sin((distance - phase * 0.45f) * 60.0f) * saturate(1.0f - abs(distance - phase * 0.45f) * 12.0f);
        float fade = (1.0f - phase) * step(seed, intensity);
        result += offset / max(distance, 0.001f) * ring * fade;
    }
    return result * 0.35f;
}

// materialData: (metallic, roughness, F0)
void ApplyWeatherToSurface(float3 worldPos, inout float3 normal, inout float3 albedo, inout float3 materialData)
{
    float wetness = WeatherParams0.x;
    float puddleAmount = WeatherParams0.y;
    float snowAmount = WeatherParams0.z;
    float rain = WeatherParams0.w;
    if (wetness <= 0.001f && puddleAmount <= 0.001f && snowAmount <= 0.001f) return;
    float upFacing = saturate(normal.y);
    float noise = WeatherFbm(worldPos.xz * 0.35f);
    float metallic = saturate(materialData.r);

    // Wet surfaces: porous dielectrics darken (light is trapped in the water film), roughness drops.
    float porosity = (1.0f - metallic) * saturate(materialData.g * 1.5f);
    float wet = saturate(wetness * lerp(0.6f, 1.0f, upFacing));
    albedo = lerp(albedo, albedo * albedo, wet * porosity * 0.85f);
    materialData.g = lerp(materialData.g, min(materialData.g, 0.12f), wet);

    // Puddles: accumulate on flat upward faces, mirror like water with rain ripples.
    float puddle = smoothstep(1.0f - puddleAmount, 1.0f - puddleAmount + 0.12f, noise) * smoothstep(0.85f, 0.97f, normal.y);
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

    // Snow on upward facing surfaces; the noise breaks up the edge.
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
