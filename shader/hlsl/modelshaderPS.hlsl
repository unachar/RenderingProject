#define SHADER_3D
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

float SampleForwardDeferredShadowMap(float3 worldPos, float3 normal, float3 lightDir)
{
    float shadowStrength = saturate(abs(ShadowMapParams.w));
    [branch]
    if (shadowStrength <= 0.0001f)
    {
        return 1.0f;
    }

    float3 n = SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f));
    float3 l = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
    float4 lightClip = mul(float4(worldPos, 1.0f), LightViewProjection);
    float safeW = max(lightClip.w, 0.000001f);
    float3 lightNdc = lightClip.xyz / safeW;
    float2 shadowUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);

    bool inBounds =
        lightClip.w > 0.0f &&
        shadowUv.x >= 0.0f && shadowUv.x <= 1.0f &&
        shadowUv.y >= 0.0f && shadowUv.y <= 1.0f &&
        lightNdc.z >= 0.0f && lightNdc.z <= 1.0f;
    [branch]
    if (!inBounds)
    {
        return 1.0f;
    }

    float texelSize = ShadowMapParams.x;
    float nDotL = saturate(dot(n, l));
    float bias = max(ShadowMapParams.y * (1.0f - nDotL), ShadowMapParams.z);
    float currentDepth = lightNdc.z - bias;

    int filterRadius = clamp((int)round(ShadowFilterParams.x), 0, 3);
    float visibility = SampleShadowMapPcf9(
        shadowUv, 0.0f, texelSize, currentDepth, filterRadius);
    return lerp(1.0f, visibility, shadowStrength);
}

float3 SampleEnvironmentLatLong(float3 dir, float roughness)
{
    const float PI = 3.14159265358979323846f;
    dir = SafeNormalizeCommon(dir, float3(0.0f, 1.0f, 0.0f));
    float2 uv = float2(atan2(dir.z, dir.x) / (2.0f * PI), acos(dir.y) / PI);
    return EnvironmentTexture.SampleLevel(g_SamplerState, uv, roughness * 8.0f).rgb;
}

float4 main(in PSInput3D In) : SV_Target
{
    const float PI = 3.14159265358979323846f;

    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : max(In.Diffuse.a, 0.0f);
    float4 baseColor = float4(In.Diffuse.rgb, In.Diffuse.a);
    if (UseTexture != 0)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, In.TexCoord);
    }
    baseColor.a *= saturate(MaterialAlpha);

    float3 surfaceNormal = In.Normal * ((FlipNormal != 0) ? -1.0f : 1.0f);
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = g_NormalTexture.Sample(g_SamplerState, In.TexCoord).xyz * 2.0f - 1.0f;
        surfaceNormal = ApplyNormalMapCommon(surfaceNormal, In.WorldPos, In.TexCoord, tangentNormal);
    }
    surfaceNormal = SafeNormalizeCommon(surfaceNormal, float3(0.0f, 1.0f, 0.0f));

    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
    bool usePartParams = MaterialMode != 1;
    float4 material = float4(
        usePartParams ? partParams.Basic.x : mMetallic,
        usePartParams ? partParams.Basic.y : mRoughness,
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
    ResolveLightAggregate(In.WorldPos, lightDir, lightColor, lightAttenuation, volumeScatter, rangeBlend);
    float lightIntensity = LightColor.a;

    if (transparent || btdf || bsdf)
    {
        float3 N = SafeNormalizeCommon(surfaceNormal, float3(0.0f, 1.0f, 0.0f));
        float3 V = SafeNormalizeCommon(CameraPos - In.WorldPos, float3(0.0f, 0.0f, -1.0f));
        float ior = max(Transparent0.x, 1.0001f);
        float transmission = saturate(Transparent0.y);
        float transmissionRoughness = saturate(Transparent0.z);
        float refractionStrength = max(Transparent0.w, 0.0f);
        float thickness = max(Transparent1.x, 0.0f);
        float3 absorption = max(Transparent1.yzw, float3(0.0f, 0.0f, 0.0f));
        float3 beerLambert = exp(-absorption * thickness);

        float f0Scalar = (1.0f - ior) / (1.0f + ior);
        f0Scalar *= f0Scalar;
        float nDotV = saturate(dot(N, V));
        float oneMinusNdotV = 1.0f - nDotV;
        float oneMinusNdotV5 = oneMinusNdotV * oneMinusNdotV * oneMinusNdotV * oneMinusNdotV * oneMinusNdotV;
        float3 fresnel = f0Scalar + (1.0f - f0Scalar) * oneMinusNdotV5;

        float3 refractedDirection = refract(-V, N, rcp(ior));
        uint sceneWidth;
        uint sceneHeight;
        SceneColorTexture.GetDimensions(sceneWidth, sceneHeight);
        float2 sceneSize = max(float2((float)sceneWidth, (float)sceneHeight), float2(1.0f, 1.0f));
        float2 screenUv = In.Position.xy / sceneSize;
        float roughnessAttenuation = 1.0f - transmissionRoughness * 0.75f;
        float2 refractedUv = saturate(screenUv + refractedDirection.xy * refractionStrength * roughnessAttenuation);
        float3 sceneTransmission = SceneColorTexture.SampleLevel(g_SamplerState, refractedUv, transmissionRoughness * 4.0f).rgb;

        float shadowVisibility = SampleForwardDeferredShadowMap(In.WorldPos, N, lightDir);
        float3 shadowTransmittance = lerp(beerLambert, float3(1.0f, 1.0f, 1.0f), shadowVisibility);
        sceneTransmission *= beerLambert * shadowTransmittance * transmission;
        sceneTransmission *= lerp(float3(1.0f, 1.0f, 1.0f), saturate(baseColor.rgb), 0.18f);

        float3 reflectedDirection = reflect(-V, N);
        float3 reflection = SampleEnvironmentLatLong(reflectedDirection, transmissionRoughness);
        float3 dielectricColor = lerp(sceneTransmission, reflection, fresnel);
        dielectricColor += lightColor.rgb * lightIntensity * saturate(dot(N, lightDir)) * fresnel * shadowVisibility * 0.08f;

        float opacity = saturate(baseColor.a);
        return float4(dielectricColor * opacity, opacity);
    }

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

    if (shadow)
    {
        float shadowVisibility = SampleForwardDeferredShadowMap(In.WorldPos, surfaceNormal, lightDir);
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
        float shadowVisibility = SampleForwardDeferredShadowMap(In.WorldPos, surfaceNormal, lightDir);
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

    if (pbr)
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
        float oneMinusVdotH = 1.0f - VdotH;
        float oneMinusVdotH5 = oneMinusVdotH * oneMinusVdotH * oneMinusVdotH * oneMinusVdotH * oneMinusVdotH;
        float3 F = F0 + (1.0f - F0) * oneMinusVdotH5;
        float3 specularBRDF = (D * G * F) / max(4.0f * NdotLScalar * NdotV, 0.00001f);
        float3 diffuseBRDF = (1.0f - F) * (1.0f - metallic) * albedo / PI;
        float shadowVisibility = SampleForwardDeferredShadowMap(In.WorldPos, N, L);
        float3 directLight = (diffuseBRDF + specularBRDF) * lightColor.rgb * lightIntensity * NdotLScalar * shadowVisibility;
        float3 R = normalize(reflect(-V, N));
        float3 envSpecular = SampleEnvironmentLatLong(R, roughness);
        float3 irradiance = SampleEnvironmentLatLong(N, 1.0f);
        float oneMinusNdotV = 1.0f - NdotV;
        float oneMinusNdotV5 = oneMinusNdotV * oneMinusNdotV * oneMinusNdotV * oneMinusNdotV * oneMinusNdotV;
        float3 roughnessF0Max = max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0);
        float3 F_IBL = F0 + (roughnessF0Max - F0) * oneMinusNdotV5;
        float3 ambientKD = (1.0f - F_IBL) * (1.0f - metallic);
        baseColor.rgb = directLight + ambientKD * albedo * irradiance / (0.5f * PI) + envSpecular * F_IBL;
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
        float shadowVisibility = SampleForwardDeferredShadowMap(In.WorldPos, N, L);
        float3 directReflection = (diffuseBRDF + specularBRDF) * lightColor.rgb * lightIntensity * NdotLScalar * shadowVisibility;
        float3 envReflection = SampleEnvironmentLatLong(normalize(reflect(-V, N)), roughness) * F;
        baseColor.rgb = directReflection + envReflection;
    }

    if (btdf)
    {
        float3 N = normalize(surfaceNormal + 0.00001f);
        float3 L = normalize(lightDir + 0.00001f);
        float3 V = normalize(CameraPos - In.WorldPos + 0.00001f);
        float roughness = clamp(material.g, 0.04f, 1.0f);
        float transmission = saturate(1.0f - material.r) * lerp(0.35f, 1.0f, roughness);
        float backNdotL = saturate(dot(-N, L));
        float frontNdotV = saturate(dot(N, V));
        float fresnel = pow(1.0f - frontNdotV, 5.0f);
        float shadowVisibility = SampleForwardDeferredShadowMap(In.WorldPos, N, L);
        float3 T = normalize(refract(-V, N, 1.0f / 1.45f));
        float3 transmittedEnv = SampleEnvironmentLatLong(T, roughness);
        float3 transmittedLight = baseColor.rgb * lightColor.rgb * lightIntensity * backNdotL * transmission * shadowVisibility;
        float3 transmittedAmbient = baseColor.rgb * transmittedEnv * transmission * lerp(0.35f, 0.8f, fresnel);
        baseColor.rgb = transmittedLight + transmittedAmbient;
    }

    if (bsdf)
    {
        float3 N = normalize(surfaceNormal + 0.00001f);
        float3 L = normalize(lightDir + 0.00001f);
        float3 V = normalize(CameraPos - In.WorldPos + 0.00001f);
        float3 H = normalize(L + V + 0.00001f);
        float NdotLScalar = saturate(dot(N, L));
        float NdotV = saturate(dot(N, V));
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));
        float roughness = clamp(material.g, 0.04f, 1.0f);
        float metallic = saturate(material.r);
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
        float3 F = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
        float shadowVisibility = SampleForwardDeferredShadowMap(In.WorldPos, N, L);
        float3 reflected = (((1.0f - F) * (1.0f - metallic) * albedo / PI) + ((D * G * F) / max(4.0f * NdotLScalar * NdotV, 0.00001f)))
            * lightColor.rgb * lightIntensity * NdotLScalar * shadowVisibility;
        float transmission = saturate(1.0f - metallic) * lerp(0.25f, 0.85f, roughness);
        float3 transmitted = albedo * lightColor.rgb * lightIntensity * saturate(dot(-N, L)) * transmission * shadowVisibility;
        float3 envReflection = SampleEnvironmentLatLong(normalize(reflect(-V, N)), roughness) * F;
        float3 envTransmission = SampleEnvironmentLatLong(normalize(refract(-V, N, 1.0f / 1.45f)), roughness) * albedo * transmission;
        baseColor.rgb = reflected + transmitted + envReflection + envTransmission * 0.5f;
    }

    baseColor.rgb *= baseColor.a;
    return baseColor;
}
