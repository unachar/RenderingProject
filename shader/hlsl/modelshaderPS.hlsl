#define SHADER_3D
#define SHADER_FORWARD_LIGHTING
#include "common.hlsl"

Texture2D g_Texture : register(t0);
Texture2D g_NormalTexture : register(t2);
Texture2D<float4> SceneColorTexture : register(t3);
Texture2D<float4> EnvironmentTexture : register(t6);
SamplerState g_SamplerState : register(s0);

float IsMaterialClassForward(float value, float target)
{
    return abs(value - target) < 0.5f;
}

float3 SampleEnvironmentLatLong(float3 dir, float roughness)
{
    const float PI = 3.14159265358979323846f;
    dir = SafeNormalizeCommon(dir, float3(0.0f, 1.0f, 0.0f));
    float2 uv = float2(
        frac(atan2(dir.z, dir.x) / (2.0f * PI) + 0.5f),
        acos(clamp(dir.y, -1.0f, 1.0f)) / PI);
    return EnvironmentTexture.SampleLevel(g_SamplerState, uv, roughness * 8.0f).rgb;
}

#include "MaterialSurface.hlsli"
#include "SkyAtmosphere.hlsli"
#include "CloudMedium.hlsli"

Texture2D<float4> ForwardBaseColor : register(t26);
Texture2D<float> DepthTexture : register(t27);
Texture2D<float4> MonitorTexture : register(t12);
Texture2D<float2> RaytracedVisibility : register(t16);
Texture2D<float4> RaytracedReflections : register(t17);
Texture2D<float> VisibilityBitmaskAO : register(t13);
Texture2D<float4> DeinterleavedSSGI : register(t14);
Texture2D<float4> AtmosphereTexture : register(t28);
SamplerState LightingSampler : register(s4);
#define BaseColorTexture ForwardBaseColor
#define ShadowMapTexture g_ShadowMap
#define ShadowSampler g_ShadowSampler
#define TextureSampler LightingSampler
// MegaLights M7：Forward / 半透明もクラスターライトグリッドで評価（ルート引数 34：t44〜t46 + b10、35：ライトのテクスチャ）
#define MEGALIGHTS_GRID 1
#define MEGALIGHTS_FORWARD 1
#define MEGALIGHTS_LIGHT_TEXTURES 1
#define MEGALIGHTS_CB_REGISTER b10
#include "MegaLightsCommon.hlsli"
StructuredBuffer<MegaLightData> MegaLightBuffer : register(t44);
StructuredBuffer<uint2> MegaLightCells : register(t45);
StructuredBuffer<uint> MegaLightIndices : register(t46);
static uint g_MLCellIndex = ML_INVALID_CELL;
#include "SurfaceLighting.hlsli"

float SampleForwardDeferredShadowMap(float3 worldPos, float3 normal, float3 lightDir)
{
    float3 directLight;
    float3 aggregateDirection;
    float3 aggregateColor;
    float attenuation;
    float volume;
    float rangeBlend;
    float visibility;
    g_MLCellIndex = MLCellIndexFromWorld(worldPos);
    ResolveSurfaceLightAggregateShadowed(worldPos, ddx(worldPos), ddy(worldPos), normal,
        WorldToScreenUV(worldPos).xy * ViewportSize, clamp((int)round(VirtualShadowGlobal.z), 0, 3),
        false, normal, 0.0f, 0.0f, 0.0f, directLight, aggregateDirection, aggregateColor,
        attenuation, volume, rangeBlend, visibility);
    return visibility;
}

// ---------------------------------------------------------------------------------------------
// Refraction (RenderQuality: RaytracedRefraction). The refracted ray is traced against the scene
// TLAS (inline RayQuery) to find the surface behind the transparent object; its colour is taken
// from the opaque scene copy (t3) at the projected hit position. Without DXR the hit distance is
// estimated from the opaque depth buffer.
// ---------------------------------------------------------------------------------------------
float3 ReconstructForwardWorld(float2 uv, float depth)
{
    float4 world = mul(float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f), PPInvViewProjection);
    return world.xyz / max(abs(world.w), 0.000001f);
}

float3 SampleRefractedScene(float3 position, float3 direction, float distance)
{
    float3 target = position + direction * distance;
    float2 uv;
    if (ProjectWorldToScreenCommon(target, uv) && all(uv > 0.0f) && all(uv < 1.0f))
    {
        return SceneColorTexture.SampleLevel(g_SamplerState, uv, 0).rgb;
    }
    return EvaluatePhysicalSky(direction, 0.002f);
}

float3 ComputeRefraction(SurfaceData surface, float3 position, float3 normal, float3 viewDir, float2 screenUv,
    out float pathLength)
{
    float eta = 1.0f / max(surface.IOR, 1.0001f);
    float3 entering = refract(-viewDir, normal, eta);
    if (dot(entering, entering) < 0.0001f) entering = reflect(-viewDir, normal);
    entering = normalize(entering);
    // Thin shells (thickness ~0) leave in the original direction; solids keep the bent ray.
    float thickness = max(surface.Thickness, 0.0f);
    float solid = saturate(thickness * 4.0f);
    float3 exitDirection = normalize(lerp(-viewDir, entering, solid));
    float3 exitPosition = position + entering * thickness;
    pathLength = thickness;
    float hitDistance = 100.0f;
    bool traced = false;
#ifdef SHADER_RAYTRACING
    if (SceneRayFlags.z > 0.5f)
    {
        RayDesc ray;
        ray.Origin = exitPosition + exitDirection * 0.01f;
        ray.Direction = exitDirection;
        ray.TMin = 0.001f;
        ray.TMax = 200.0f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0xff, ray);
        while (query.Proceed()) { }
        hitDistance = query.CommittedStatus() != COMMITTED_NOTHING ? query.CommittedRayT() : 1000.0f;
        traced = true;
    }
#endif
    if (!traced)
    {
        // Screen space estimate: distance to the opaque surface behind this pixel.
        float opaqueDepth = DepthTexture.SampleLevel(g_SamplerState, screenUv, 0);
        float3 opaquePosition = ReconstructForwardWorld(screenUv, opaqueDepth);
        hitDistance = opaqueDepth >= 0.9999f ? 1000.0f : max(length(opaquePosition - position) - thickness, 0.05f);
    }
    return SampleRefractedScene(exitPosition, exitDirection, hitDistance);
}

float4 ShadeForward(in PSInput3D In)
{
    const float PI = 3.14159265358979323846f;

    AssetMaterialParameters assetMaterial = LoadAssetMaterial(0);
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : max(In.Diffuse.a, 0.0f);
    // Model color alpha can store a material class even after switching to manual mode.
    float4 baseColor = float4(In.Diffuse.rgb, 1.0f);
    if (assetMaterial.Surface.z > 0.5f)
    {
        baseColor = float4(1, 1, 1, 1);
    }
    if (UseTexture != 0)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, In.TexCoord);
    }
    float assetMetallic = mMetallic;
    float assetRoughness = mRoughness;
    float4 assetEmissionAO;
    ApplyAssetMaterial(assetMaterial, In.TexCoord, g_SamplerState, baseColor, assetMetallic, assetRoughness, assetEmissionAO);
    baseColor.a = saturate(baseColor.a * MaterialAlpha);
    clip(baseColor.a - (1.0f / 255.0f));


    float3 surfaceNormal = In.Normal * ((FlipNormal != 0) ? -1.0f : 1.0f);
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = g_NormalTexture.Sample(g_SamplerState, In.TexCoord).xyz * 2.0f - 1.0f;
        surfaceNormal = ApplyNormalMapCommon(surfaceNormal, In.WorldPos, In.TexCoord, ScaleAssetNormal(tangentNormal, assetMaterial));
    }
    surfaceNormal = SafeNormalizeCommon(surfaceNormal, float3(0.0f, 1.0f, 0.0f));

    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
    bool usePartParams = MaterialMode != 1;
    float4 material = float4(
        usePartParams ? partParams.Basic.x : assetMetallic,
        usePartParams ? partParams.Basic.y : assetRoughness,
        usePartParams ? partParams.Basic.z : mFresnel,
        materialPartId);
    float4 shadowParams = usePartParams
        ? float4(partParams.Shadow0.x, partParams.Shadow0.y, partParams.Shadow0.z, 0.0f)
        : float4(ShadowThreshold, ShadowSoftness, ShadowStrength, 0.0f);

    float shaderClass = material.a;
    bool transparent = IsMaterialClassForward(shaderClass, 0.0f) || MaterialIsTransparent != 0;
    bool shadow = IsMaterialClassForward(shaderClass, 5.0f);
    bool lit = IsMaterialClassForward(shaderClass, 8.0f);
    bool pbr = IsMaterialClassForward(shaderClass, 11.0f);
    bool brdf = IsMaterialClassForward(shaderClass, 12.0f);
    bool btdf = IsMaterialClassForward(shaderClass, 13.0f);
    bool bsdf = IsMaterialClassForward(shaderClass, 14.0f);

    float shadowThreshold = shadowParams.r;
    float shadowSoftness = max(shadowParams.g, 0.0001f);
    float shadowStrength = saturate(shadowParams.b);

    float3 lightDir;
    float3 lightColor;
    float lightAttenuation;
    float volumeScatter;
    float rangeBlend;
    ResolveLightAggregate(In.WorldPos, In.Position.xy, lightDir, lightColor, lightAttenuation, volumeScatter, rangeBlend);
    float lightIntensity = LightColor.a;

    float metallicParam = material.r;
    float roughnessParam = material.g;
    float f0 = material.b;
    float3 NdotL = saturate(dot(surfaceNormal, lightDir));
    float3 environmentColor = EnvironmentTexture.SampleLevel(g_SamplerState, In.TexCoord, roughnessParam * 10.0f).rgb;
    NdotL += lightIntensity;

    float3 diffuse = 0.0f;
    {
        float3 light = lightColor.rgb * saturate(dot(lightDir, surfaceNormal));
        float2 iblTexcoord;
        iblTexcoord.x = -atan2(surfaceNormal.x, surfaceNormal.z) / (PI * 2);
        iblTexcoord.y = asin(surfaceNormal.y) / PI;
        light += EnvironmentTexture.SampleLevel(g_SamplerState, iblTexcoord, roughnessParam * 10.0f).rgb * 6;
        diffuse = light * baseColor.rgb / PI;
    }

    // Share the legacy visibility query instead of inlining the full light loop per class.
    float legacyShadowVisibility = 1.0f;
    bool useLegacyBrdf = brdf && !(pbr || transparent || btdf || bsdf);
    if (shadow || lit || useLegacyBrdf)
    {
        float3 shadowNormal = useLegacyBrdf ? normalize(surfaceNormal + 0.00001f) : surfaceNormal;
        legacyShadowVisibility = SampleForwardDeferredShadowMap(In.WorldPos, shadowNormal, lightDir);
    }

    if (shadow)
    {
        float shadowVisibility = legacyShadowVisibility;
        float castShadow = saturate(1.0f - shadowVisibility);
        float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
        float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
        float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
        float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
        float3 shadowTint = saturate(float3(0.015f, 0.014f, 0.017f) + environmentColor * 0.16f + lightTint * 0.025f);
        float3 litColor = baseColor.rgb * (0.12f + NdotL * (0.22f + lightColor.rgb * 0.72f));
        baseColor.rgb = lerp(litColor, baseColor.rgb * shadowTint - 0.08f, shadowDensity);
    }

    if (lit)
    {
        float shadowVisibility = legacyShadowVisibility;
        float castShadow = saturate(1.0f - shadowVisibility);
        float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
        float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
        float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
        float selfShadowMask = smoothstep(shadowThreshold - shadowSoftness, shadowThreshold + shadowSoftness, castShadow);
        shadowDensity = saturate(max(shadowDensity, selfShadowMask * shadowStrength * lerp(0.20f, 0.85f, lightShadowPower)));
        float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
        float3 shadowTint = saturate(float3(0.018f, 0.016f, 0.020f) + environmentColor + lightTint * 0.025f);
        float3 litColor = baseColor.rgb * (lightColor.rgb * 0.72f + NdotL);
        baseColor.rgb = diffuse * lerp(litColor, litColor * shadowTint, shadowDensity);
    }

    if (pbr || transparent || btdf || bsdf)
    {
        SurfaceData surface = BuildLightingSurface(baseColor, surfaceNormal, material.rgb, assetEmissionAO);
        surface.Transmission = saturate(Transparent0.y);
        surface.IOR = max(Transparent0.x, 1.0001f);
        surface.Thickness = max(Transparent1.x, 0.0f);
        surface.AbsorptionColor = max(Transparent1.yzw, 0.0f);
        float3 viewDir = SafeNormalizeCommon(CameraPos - In.WorldPos, surfaceNormal);
        float3 directLight;
        float visibility;
        int filterRadius = clamp((int)round(lerp(max(VirtualShadowGlobal.z, 1.0f),
            3.0f, saturate(shadowSoftness * 2.0f))), 1, 3);
        g_MLCellIndex = MLCellIndexFromWorld(In.WorldPos);
        ResolveSurfaceLightAggregateShadowed(In.WorldPos, ddx(In.WorldPos), ddy(In.WorldPos),
            surfaceNormal, In.Position.xy, filterRadius, true, viewDir, surface.BaseColor,
            material.rgb, shadowSoftness, directLight, lightDir, lightColor,
            lightAttenuation, volumeScatter, rangeBlend, visibility);
        uint width;
        uint height;
        ForwardBaseColor.GetDimensions(width, height);
        float2 screenUv = In.Position.xy / float2(max(width, 1u), max(height, 1u));
        float3 color = EvaluateSurfaceEnvironment(surface, viewDir, In.WorldPos,
            screenUv, shadowSoftness, directLight);
        color = ApplySurfaceScreenEffects(color - surface.Emissive, surface.Emissive, screenUv, surface.BaseColor * (1 - surface.Metallic));
        float3 premultiplied = color * surface.Opacity;
        float coverage = surface.Opacity;
        if (QualityFlags.w > 0.5f && surface.Transmission > 0.001f && surface.Opacity < 0.999f)
        {
            float pathLength;
            float3 refracted = ComputeRefraction(surface, In.WorldPos, surfaceNormal, viewDir, screenUv, pathLength);
            float f0 = pow((surface.IOR - 1.0f) / (surface.IOR + 1.0f), 2.0f);
            float fresnel = f0 + (1.0f - f0) * pow(1.0f - saturate(dot(surfaceNormal, viewDir)), 5.0f);
            // Beer-Lambert absorption inside the medium, tinted by the base colour.
            float3 transmittance = exp(-surface.AbsorptionColor * pathLength) * lerp(1.0f, surface.BaseColor, 0.5f);
            float refractive = (1.0f - surface.Opacity) * surface.Transmission;
            premultiplied += refractive * (1.0f - fresnel) * transmittance * refracted;
            coverage = surface.Opacity + refractive;
        }
        return float4(premultiplied, saturate(coverage));
    }

    if (brdf)
    {
        float3 N = normalize(surfaceNormal + 0.00001f);
        float3 L = normalize(lightDir + 0.00001f);
        float3 V = normalize(CameraPos - In.WorldPos + 0.00001f);
        float3 H = normalize(L + V + 0.00001f);
        float NdotLScalar = saturate(dot(N, L));
        float NdotV = saturate(dot(N, V));
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));
        float metallic = saturate(material.r);
        float roughness = clamp(material.g, 0.04f, 1.0f);
        float specularF0 = max(material.b, 0.04f);
        float3 albedo = baseColor.rgb;
        float3 F0 = lerp(float3(specularF0, specularF0, specularF0), albedo, metallic);
        float a = roughness * roughness;
        float a2 = a * a;
        float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
        float D = a2 / max(PI * d * d, 0.00001f);
        float k = ((roughness + 1.0f) * (roughness + 1.0f)) / 8.0f;
        float G_L = NdotLScalar / max(NdotLScalar * (1.0f - k) + k, 0.00001f);
        float G_V = NdotV / max(NdotV * (1.0f - k) + k, 0.00001f);
        float G = G_L * G_V;
        float FPower = pow(1.0f - VdotH, 5.0f);
        float3 F = F0 + (1.0f - F0) * FPower;
        float3 specularBRDF = (D * G * F) / max(4.0f * NdotLScalar * NdotV, 0.00001f);
        float3 diffuseBRDF = (1.0f - F) * (1.0f - metallic) * albedo / PI;
        float shadowVisibility = legacyShadowVisibility;
        float3 directReflection = (diffuseBRDF + specularBRDF) * lightColor.rgb * lightIntensity * NdotLScalar * shadowVisibility;
        float3 envReflection = SampleEnvironmentLatLong(normalize(reflect(-V, N)), roughness) * F;
        baseColor.rgb = directReflection + envReflection;
    }

    baseColor.rgb = baseColor.rgb * assetEmissionAO.a + assetEmissionAO.rgb;
    baseColor.rgb *= baseColor.a;
    return baseColor;
}

// ---------------------------------------------------------------------------------------------
// Output: SV_Target0 premultiplied colour (all ordinary passes).
// During the OIT transparent pass the PSO masks target 0 and blends SV_Target1/2 into the
// weighted blended OIT accumulation / revealage targets (McGuire & Bavoil 2013).
// Passes with a single render target ignore SV_Target1/2.
// ---------------------------------------------------------------------------------------------
struct ForwardOutput
{
    float4 Color : SV_Target0;
    float4 Accumulation : SV_Target1;
    float4 Revealage : SV_Target2;
};

ForwardOutput main(in PSInput3D In)
{
    float4 premultiplied = ShadeForward(In);
    ForwardOutput output;
    output.Color = premultiplied;
    float alpha = saturate(premultiplied.a);
    float viewDistance = length(In.WorldPos - CameraPos);
    // Eq. (7): distance based weight, robust for 0.1 .. 500 m.
    float weight = alpha * clamp(0.03f / (0.00001f + pow(viewDistance / 200.0f, 4.0f)), 0.01f, 3000.0f);
    output.Accumulation = float4(premultiplied.rgb, alpha) * weight;
    output.Revealage = alpha.xxxx;
    return output;
}

