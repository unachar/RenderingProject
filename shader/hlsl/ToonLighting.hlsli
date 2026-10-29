#ifndef LIVE_TOON_LIGHTING_HLSLI
#define LIVE_TOON_LIGHTING_HLSLI

// Shared by Forward and Deferred. No material/global buffer reads here: Deferred
// must not borrow another entity's material controls from the full-screen pass.
bool IsLiveToonClass(float materialClass)
{
    return materialClass >= 0.5f && materialClass < 4.5f;
}

float4 PackLiveToonPomShadow(float4 authoredShadow, float visibility)
{
    // SHADOW is R8G8B8A8_UNORM. Toon does not use the PBR shading-model alpha;
    // zero alpha naturally means no POM occlusion for other GBuffer writers.
    return float4(authoredShadow.x, authoredShadow.y, authoredShadow.z, 1.0f - saturate(visibility));
}

float LiveToonPomVisibility(float encodedOcclusion)
{
    return 1.0f - saturate(encodedOcclusion);
}

float LiveToonBand(float value, float threshold, float softness)
{
    float width = max(softness, 0.001f);
    return smoothstep(threshold - width, threshold + width, value);
}

float3 LiveToonNormalize(float3 value, float3 fallback)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > 0.000001f ? value * rsqrt(lengthSquared) : fallback;
}

// Radiance already contains intensity, distance falloff and shadow visibility.
// Accumulate this for each light, never with an averaged direction: opposing
// blue/red spots need independent cel bands, highlights and directional rims.
float3 EvaluateLiveToonLight(float3 albedo, float3 normal, float3 viewDir, float3 lightDir,
                             float3 radiance, float materialClass, float roughness,
                             float shadowThreshold, float shadowSoftness, float shadowStrength)
{
    bool skin = abs(materialClass - 3.0f) < 0.5f;
    bool hair = abs(materialClass - 1.0f) < 0.5f;
    float nDotL = dot(normal, lightDir);
    float wrap = skin ? 0.38f : 0.12f;
    float diffuseCoordinate = saturate((nDotL + wrap) / (1.0f + wrap));
    float softness = max(shadowSoftness, skin ? 0.075f : 0.015f);
    float mid = LiveToonBand(diffuseCoordinate, saturate(shadowThreshold), softness);
    float lit = LiveToonBand(diffuseCoordinate, min(shadowThreshold + 0.24f, 0.95f), softness);
    float shadeFloor = skin ? 0.55f : 0.28f;
    float shade = lerp(1.0f, lerp(shadeFloor, 0.76f, mid) + 0.24f * lit, saturate(shadowStrength));

    // Skin retains a neutral component under saturated stage lights; cloth/hair
    // retain the full stage tint. This is a material profile, not an RGB skin mask.
    float luminance = max(dot(radiance, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
    float3 skinRadiance = lerp(float3(luminance, luminance, luminance), radiance, 0.55f);
    float3 diffuseRadiance = skin ? skinRadiance : radiance;

    float3 halfDir = LiveToonNormalize(lightDir + viewDir, normal);
    float specular = pow(saturate(dot(normal, halfDir)), lerp(96.0f, 20.0f, saturate(roughness)));
    specular = LiveToonBand(specular, hair ? 0.45f : 0.65f, 0.04f) * saturate(nDotL);
    float specularStrength = skin ? 0.025f : (hair ? 0.20f : 0.12f);

    float edge = 1.0f - saturate(dot(normal, viewDir));
    float rim = edge * edge * edge * edge;
    rim *= LiveToonBand(-dot(viewDir, lightDir), 0.1f, 0.3f);
    rim *= saturate(nDotL + 0.3f);
    float rimStrength = skin ? 0.16f : 0.42f;

    // Every term is linear in albedo, and zero for zero radiance. Keep HDR values
    // for exposure/bloom; final display mapping belongs to the post-process pass.
    return albedo * (diffuseRadiance * shade + radiance * (specular * specularStrength + rim * rimStrength));
}

float3 EvaluateLiveToonAmbient(float3 albedo, float3 environmentIrradiance, float materialClass)
{
    float fill = abs(materialClass - 3.0f) < 0.5f ? 0.20f : 0.12f;
    return albedo * (float3(fill, fill, fill) + environmentIrradiance * 0.25f);
}
#endif
