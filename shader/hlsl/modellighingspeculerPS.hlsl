#define SHADER_3D
#include "common.hlsl"

Texture2D g_Texture : register(t0);
Texture2D g_NormalTexture : register(t2);
SamplerState g_SamplerState : register(s0);

float3 SafeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return (lenSq > 0.000001f) ? value * rsqrt(lenSq) : fallback;
}

float Luminance(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float3 AdjustSaturation(float3 color, float saturation)
{
    float luma = Luminance(color);
    return lerp(float3(luma, luma, luma), color, saturation);
}

float ToonStep(float value, float threshold, float softness)
{
    return smoothstep(threshold - softness, threshold + softness, value);
}

float Colorfulness(float3 color)
{
    float maxChannel = max(color.r, max(color.g, color.b));
    float minChannel = min(color.r, min(color.g, color.b));
    return maxChannel - minChannel;
}

float SkinMask(float3 albedo)
{
    float warm = saturate((albedo.r - albedo.b) * 2.7f + (albedo.g - albedo.b) * 0.8f);
    float lumaMask = saturate((Luminance(albedo) - 0.35f) * 2.5f);
    return warm * lumaMask;
}

float HairMask(float3 albedo)
{
    float blonde = saturate((albedo.r + albedo.g - albedo.b * 1.55f - 0.45f) * 1.7f);
    float notWhite = 1.0f - saturate((Luminance(albedo) - 0.82f) * 4.0f);
    return blonde * notWhite;
}

float BlueMask(float3 albedo)
{
    return saturate((albedo.b - albedo.r) * 2.4f + (albedo.g - albedo.r) * 0.9f);
}

float WhiteMask(float3 albedo)
{
    float lowChroma = 1.0f - saturate(Colorfulness(albedo) * 5.0f);
    float bright = saturate((Luminance(albedo) - 0.58f) * 3.0f);
    return lowChroma * bright;
}

float DarkMask(float3 albedo)
{
    return saturate((0.38f - Luminance(albedo)) * 3.5f);
}

float3 BuildShadowColor(float3 albedo, float rangeBlend)
{
    float skin = SkinMask(albedo);
    float hair = HairMask(albedo) * (1.0f - skin * 0.55f);
    float blue = BlueMask(albedo);
    float white = WhiteMask(albedo);
    float dark = DarkMask(albedo);

    float3 tint = float3(0.78f, 0.74f, 0.94f);
    tint = lerp(tint, float3(1.08f, 0.73f, 0.68f), skin);
    tint = lerp(tint, float3(1.08f, 0.76f, 0.50f), hair);
    tint = lerp(tint, float3(0.55f, 0.69f, 1.12f), blue);
    tint = lerp(tint, float3(0.78f, 0.76f, 0.96f), white);
    tint = lerp(tint, float3(0.30f, 0.28f, 0.48f), dark);
    tint = lerp(tint, float3(0.88f, 0.84f, 1.0f), rangeBlend * 0.22f);

    return saturate(albedo * tint + float3(0.035f, 0.025f, 0.055f));
}

float3 BuildMidColor(float3 albedo, float rangeBlend)
{
    float skin = SkinMask(albedo);
    float hair = HairMask(albedo) * (1.0f - skin * 0.55f);
    float blue = BlueMask(albedo);
    float white = WhiteMask(albedo);
    float dark = DarkMask(albedo);

    float3 tint = float3(0.98f, 0.98f, 1.06f);
    tint = lerp(tint, float3(1.13f, 0.98f, 0.90f), skin);
    tint = lerp(tint, float3(1.16f, 0.95f, 0.72f), hair);
    tint = lerp(tint, float3(0.80f, 0.96f, 1.30f), blue);
    tint = lerp(tint, float3(1.02f, 1.00f, 1.12f), white);
    tint = lerp(tint, float3(0.52f, 0.48f, 0.74f), dark);

    return saturate(albedo * lerp(tint, float3(1.06f, 1.05f, 1.10f), rangeBlend * 0.16f));
}

float3 BuildLitColor(float3 albedo)
{
    float skin = SkinMask(albedo);
    float hair = HairMask(albedo) * (1.0f - skin * 0.55f);
    float blue = BlueMask(albedo);
    float white = WhiteMask(albedo);
    float dark = DarkMask(albedo);

    float3 tint = float3(1.08f, 1.08f, 1.08f);
    tint = lerp(tint, float3(1.22f, 1.04f, 0.94f), skin);
    tint = lerp(tint, float3(1.24f, 0.98f, 0.70f), hair);
    tint = lerp(tint, float3(0.75f, 0.94f, 1.42f), blue);
    tint = lerp(tint, float3(1.08f, 1.04f, 1.18f), white);
    tint = lerp(tint, float3(0.60f, 0.56f, 0.92f), dark);

    return saturate(albedo * tint + white * float3(0.06f, 0.05f, 0.10f));
}

float3 BuildSpecularColor(float3 albedo, float metallic)
{
    float blue = BlueMask(albedo);
    float white = WhiteMask(albedo);
    float skin = SkinMask(albedo);
    float3 nonMetalSpec = lerp(float3(0.98f, 0.96f, 1.0f), float3(0.72f, 0.90f, 1.0f), blue);
    nonMetalSpec = lerp(nonMetalSpec, float3(1.0f, 0.82f, 0.76f), skin * 0.35f);
    nonMetalSpec = lerp(nonMetalSpec, float3(1.0f, 0.98f, 1.0f), white);
    return lerp(nonMetalSpec, albedo, metallic);
}

float4 main(in PSInput3D In) : SV_Target
{
    float4 baseColor = In.Diffuse;
    if (UseTexture != 0)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, In.TexCoord);
    }
    baseColor.a *= saturate(MaterialAlpha);

    clip(baseColor.a - 0.01f);

    float3 albedo = saturate(baseColor.rgb);
    float kawaiiBlend = saturate(KawaiiBlend);
    albedo = saturate(AdjustSaturation(albedo, BaseSaturation) * BaseBrightness);

    float3 viewDir = SafeNormalize(CameraPos - In.WorldPos, float3(0.0f, 0.0f, -1.0f));
    float normalBlend = saturate(NormalBlend);
    float normalBias = clamp(NormalBias, -1.0f, 1.0f);
    float3 meshNormal = SafeNormalize(In.Normal * ((FlipNormal != 0) ? -1.0f : 1.0f), float3(0.0f, 1.0f, 0.0f));
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = g_NormalTexture.Sample(g_SamplerState, In.TexCoord).xyz * 2.0f - 1.0f;
        meshNormal = ApplyNormalMapCommon(meshNormal, In.WorldPos, In.TexCoord, tangentNormal);
    }
    float3 roundedNormal = SafeNormalize(meshNormal + viewDir, meshNormal);
    float3 normal = SafeNormalize(lerp(meshNormal, roundedNormal, normalBlend), meshNormal);

    float3 lightDir;
    float3 lightColor;
    float lightAttenuation;
    float volumeScatter;
    float rangeBlend;
    ResolveLightAggregate(In.WorldPos, In.Position.xy, lightDir, lightColor, lightAttenuation, volumeScatter, rangeBlend);
    float3 halfDir = SafeNormalize(lightDir + viewDir, normal);

    float lightIntensity = 1.0f;

    float rawNDotL = dot(normal, lightDir) + normalBias;
    float halfLambert = saturate(rawNDotL * lerp(0.5f, 0.32f, rangeBlend) + lerp(0.5f, 0.68f, rangeBlend));

    float shadowThreshold = lerp(ShadowThreshold, ShadowThreshold * 0.62f, rangeBlend);
    float midThreshold = saturate(shadowThreshold + lerp(0.24f, 0.18f, rangeBlend));
    float softness = lerp(ShadowSoftness, ShadowSoftness * 1.8f, rangeBlend);

    float shadowMask = ToonStep(halfLambert, shadowThreshold, softness);
    float midMask = ToonStep(halfLambert, midThreshold, softness * 1.3f);

    float3 neutralShadow = albedo * float3(0.62f, 0.64f, 0.72f);
    float3 neutralMid = albedo * float3(0.92f, 0.94f, 1.0f);
    float3 neutralLit = albedo * float3(1.10f, 1.08f, 1.04f);
    float3 shadowColor = lerp(neutralShadow, BuildShadowColor(albedo, rangeBlend), kawaiiBlend) * ShadowStrength;
    float3 midColor = lerp(neutralMid, BuildMidColor(albedo, rangeBlend), kawaiiBlend) * MidStrength;
    float3 litColor = lerp(neutralLit, BuildLitColor(albedo), kawaiiBlend) * LitStrength;

    float3 toonDiffuse = lerp(shadowColor, midColor, shadowMask);
    toonDiffuse = lerp(toonDiffuse, litColor, midMask);
    toonDiffuse *= lerp(float3(1.0f, 1.0f, 1.0f), lightColor, 0.55f) * lightIntensity * lightAttenuation;

    float nDotH = saturate(dot(normal, halfDir));
    float specRaw = pow(nDotH, lerp(96.0f, 18.0f, saturate(mRoughness)));
    float specMask = ToonStep(specRaw, SpecularThreshold, 0.018f);
    float3 specular = BuildSpecularColor(albedo, saturate(mMetallic)) * specMask * SpecularStrength;

    float rim = 1.0f - saturate(dot(normal, viewDir));
    float rimMask = ToonStep(rim, lerp(RimThreshold, RimThreshold * 0.78f, rangeBlend), 0.055f);
    rimMask *= ToonStep(rawNDotL, -0.25f, 0.35f);
    float3 rimColor = rimMask * lerp(float3(0.38f, 0.48f, 0.80f), albedo * 0.45f, 0.20f) * RimStrength;

    float3 ambient = BuildShadowColor(albedo, rangeBlend) * lerp(0.22f, 0.30f, rangeBlend);
    float3 color = ambient + toonDiffuse + specular + rimColor;
    color += lightColor * volumeScatter * lightIntensity * 0.35f;

    color = AdjustSaturation(color, 1.16f);
    color = lerp(color, sqrt(saturate(color)), 0.12f);
    return float4(saturate(color) * baseColor.a, baseColor.a);
}
