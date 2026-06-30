#include "pbrToonCommon.slang"

static const float PI = 3.14159265359f;
static const float EPSILON = 1.0e-5f;

struct LightParams
{
    float4 LightDirection;
    float4 LightColor;
};

struct PBRParams
{
    float Metallic;
    float Roughness;
    float2 Padding;
};



struct Surface
{
    float3 albedo;
    float alpha;
    float3 normal;
    float3 viewDir;
    float3 lightDir;
    float3 lightColor;
    float lightIntensity;
    float lightRange;
    float metallic;
    float roughness;
};

interface IToonRamp
{
    float evaluate(float nDotL);
}

struct AliciaFaceBodyRamp : IToonRamp
{
    float evaluate(float nDotL)
    {
        float wrapped = saturate(nDotL * 0.5f + 0.5f);
        float shadow = smoothstep(0.18f, 0.26f, wrapped);
        float mid = smoothstep(0.48f, 0.58f, wrapped);
        float lit = smoothstep(0.74f, 0.84f, wrapped);
        return 0.38f + shadow * 0.23f + mid * 0.22f + lit * 0.17f;
    }
}

struct AliciaHairRamp : IToonRamp
{
    float evaluate(float nDotL)
    {
        float wrapped = saturate(nDotL * 0.55f + 0.45f);
        float shadow = smoothstep(0.22f, 0.32f, wrapped);
        float lit = smoothstep(0.67f, 0.82f, wrapped);
        return 0.30f + shadow * 0.36f + lit * 0.22f;
    }
}

struct MicrofacetBRDF
{
    float distributionGGX(float nDotH, float roughness)
    {
        float a = roughness * roughness;
        float a2 = a * a;
        float denom = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
        return a2 / max(PI * denom * denom, EPSILON);
    }

    float geometrySchlickGGX(float nDotV, float roughness)
    {
        float r = roughness + 1.0f;
        float k = (r * r) * 0.125f;
        return nDotV / max(nDotV * (1.0f - k) + k, EPSILON);
    }

    float geometrySmith(float nDotV, float nDotL, float roughness)
    {
        return geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
    }

    float3 fresnelSchlick(float cosTheta, float3 f0)
    {
        return f0 + (1.0f - f0) * pow(saturate(1.0f - cosTheta), 5.0f);
    }

    float3 specular(Surface s)
    {
        float3 halfDir = normalize(s.viewDir + s.lightDir);
        float nDotV = max(dot(s.normal, s.viewDir), 0.001f);
        float nDotL = max(dot(s.normal, s.lightDir), 0.0f);
        float nDotH = max(dot(s.normal, halfDir), 0.0f);
        float vDotH = max(dot(s.viewDir, halfDir), 0.0f);

        float3 f0 = lerp(float3(0.04f), s.albedo, s.metallic);
        float d = distributionGGX(nDotH, s.roughness);
        float g = geometrySmith(nDotV, nDotL, s.roughness);
        float3 f = fresnelSchlick(vDotH, f0);

        return (d * g * f) / max(4.0f * nDotV * nDotL, EPSILON);
    }
}

Texture2D<float4> g_Texture : register(t0);
SamplerState g_SamplerState : register(s0);
ConstantBuffer<LightParams> g_Light : register(b1);
ConstantBuffer<PBRParams> g_PBR : register(b2);

float luminance(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float3 preserveTextureHue(float3 albedo, float shade)
{
    float luma = max(luminance(albedo), 0.08f);
    float3 chroma = albedo / luma;
    return saturate(chroma * shade);
}

float3 shadeWithRamp<TRamp : IToonRamp>(Surface s, TRamp ramp)
{
    float rawNDotL = dot(s.normal, s.lightDir);
    float rangeBlend = saturate((s.lightRange - 1.0f) / 7.0f);
    float nDotL = lerp(rawNDotL, rawNDotL * 0.35f + 0.65f, rangeBlend);
    float rampValue = ramp.evaluate(nDotL);
    float3 toonDiffuse = preserveTextureHue(s.albedo, rampValue) * s.lightColor * s.lightIntensity;

    MicrofacetBRDF brdf;
    float3 spec = brdf.specular(s);

    float specMask = smoothstep(0.16f, 0.34f, luminance(spec));
    float3 toonSpecular = specMask * lerp(float3(0.75f, 0.78f, 0.86f), s.albedo, 0.16f);

    float rim = 1.0f - saturate(dot(s.normal, s.viewDir));
    float rimStep = smoothstep(0.48f, 0.72f, rim) * smoothstep(-0.25f, 0.35f, rawNDotL);
    float3 rimColor = rimStep * float3(0.20f, 0.24f, 0.32f);

    float3 ambient = s.albedo * float3(0.20f, 0.21f, 0.24f);
    return ambient + toonDiffuse + toonSpecular * 0.65f + rimColor;
}

float3 applyAliciaPalette(float3 color, float3 albedo)
{
    float skinHint = saturate((albedo.r - albedo.b) * 2.8f + (albedo.g - albedo.b) * 0.7f);
    float hairHint = saturate((albedo.b + albedo.g) * 0.5f - albedo.r * 0.55f);

    float3 skinWarmth = float3(1.06f, 0.96f, 0.90f);
    float3 hairCool = float3(0.88f, 0.93f, 1.08f);
    float3 palette = lerp(float3(1.0f), skinWarmth, skinHint * 0.35f);
    palette = lerp(palette, hairCool, hairHint * 0.25f);
    return color * palette;
}

[shader("fragment")]
float4 main(PSInput3D input) : SV_Target
{
    float4 baseColor = input.Diffuse;
    if (g_Model.UseTexture != 0)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, input.TexCoord);
    }

    clip(baseColor.a - 0.01f);

    Surface surface;
    surface.albedo = saturate(baseColor.rgb);
    surface.alpha = baseColor.a;

    float normalFlip = (g_Model.FlipNormal != 0) ? -1.0f : 1.0f;
    surface.normal = safeNormalize(input.Normal * normalFlip, float3(0.0f, 1.0f, 0.0f));

    float3 cameraPos = (dot(input.CameraPos, input.CameraPos) > EPSILON)
        ? input.CameraPos
        : g_Model.CameraPos;
    surface.viewDir = safeNormalize(cameraPos - input.WorldPos, float3(0.0f, 0.0f, -1.0f));
    surface.lightDir = safeNormalize(g_Light.LightDirection.xyz, float3(0.0f, 1.0f, 0.0f));
    surface.lightColor = max(g_Light.LightColor.rgb, float3(0.0f));
    surface.lightIntensity = max(g_Light.LightColor.a, 0.0f);
    surface.lightRange = max(g_Light.LightDirection.w, 1.0f);
    surface.metallic = saturate(g_PBR.Metallic * 0.35f);
    surface.roughness = clamp(g_PBR.Roughness, 0.38f, 0.92f);

    AliciaFaceBodyRamp faceBodyRamp;
    AliciaHairRamp hairRamp;
    float hairBlend = saturate((surface.albedo.b + surface.albedo.g) * 0.55f - surface.albedo.r * 0.45f);
    float3 bodyShading = shadeWithRamp(surface, faceBodyRamp);
    float3 hairShading = shadeWithRamp(surface, hairRamp);

    float3 color = lerp(bodyShading, hairShading, hairBlend);
    color = applyAliciaPalette(color, surface.albedo);

    float3 outlineDarken = lerp(float3(0.92f), float3(1.0f), saturate(dot(surface.normal, surface.viewDir) + 0.2f));
    color *= outlineDarken;

    color = color / (color + float3(1.35f));
    color = pow(saturate(color), float3(1.0f / 2.2f));

    return float4(color, surface.alpha);
}
