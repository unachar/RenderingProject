#define SHADER_POSTPROCESS
#define SHADER_3D
#include "common.hlsl"

Texture2D<float4> BaseColorTexture : register(t0);
Texture2D<float4> NormalTexture : register(t1);
Texture2D<float4> PositionTexture : register(t2);
Texture2D<float> DepthTexture : register(t3);
Texture2D<float4> MaterialTexture : register(t4);
Texture2D<float4> ShadowGBufferTexture : register(t5);
Texture2D<float4> EnvironmentTexture : register(t6);
Texture2D<float> ShadowMapTexture : register(t7);

SamplerState TextureSampler : register(s0);
SamplerState ShadowSampler : register(s1);

float IsMaterialClass(float value, float target)
{
    return abs(value - target) < 0.5f;
}

float SampleDeferredShadowMap(float3 worldPos, float3 normal, float3 lightDir)
{
    float3 n = SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f));
    float3 l = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
    float4 lightClip = mul(float4(worldPos, 1.0f), LightViewProjection);
    float safeW = max(lightClip.w, 0.000001f);
    float3 lightNdc = lightClip.xyz / safeW;
    float2 shadowUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);

    float inBounds =
        (lightClip.w > 0.0f &&
         shadowUv.x >= 0.0f && shadowUv.x <= 1.0f &&
         shadowUv.y >= 0.0f && shadowUv.y <= 1.0f &&
         lightNdc.z >= 0.0f && lightNdc.z <= 1.0f) ? 1.0f : 0.0f;

    float texelSize = ShadowMapParams.x;
    float nDotL = saturate(dot(n, l));
    float bias = max(ShadowMapParams.y * (1.0f - nDotL), ShadowMapParams.z);
    float currentDepth = lightNdc.z - bias;

    float visibility = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float closestDepth = ShadowMapTexture.SampleLevel(ShadowSampler, shadowUv + float2(x, y) * texelSize, 0);
            visibility += (currentDepth <= closestDepth) ? 1.0f : 0.0f;
        }
    }

    visibility /= 25.0f;
    return lerp(1.0f, lerp(1.0f, visibility, inBounds), saturate(ShadowMapParams.w));
}



float4 main(PSInputPostProcess input) : SV_Target
{
    
    const float PI = 3.14159265358979323846f;
    float4 baseColor = BaseColorTexture.Sample(TextureSampler, input.TexCoord);
    float4 normal = NormalTexture.Sample(TextureSampler, input.TexCoord);
    float4 position = PositionTexture.Sample(TextureSampler, input.TexCoord);
    float4 material = MaterialTexture.Sample(TextureSampler, input.TexCoord);
    float4 shadowParams = ShadowGBufferTexture.Sample(TextureSampler, input.TexCoord);

    float shaderClass = material.a;
    bool background = shaderClass < -0.5f;
    bool transparent = IsMaterialClass(shaderClass, 0.0f);
    bool shadow = IsMaterialClass(shaderClass, 5.0f);
    bool lit = IsMaterialClass(shaderClass, 8.0f);
    bool pbr = IsMaterialClass(shaderClass, 11.0f);
    bool brdf = IsMaterialClass(shaderClass, 12.0f);
    bool btdf = IsMaterialClass(shaderClass, 13.0f);
    bool bsdf = IsMaterialClass(shaderClass, 14.0f);
    
    if (background || transparent)
    {
        return baseColor;
    }

   
    float shadowThreshold = shadowParams.r;
    float shadowSoftness = max(shadowParams.g, 0.0001f);
    float shadowStrength = saturate(shadowParams.b);



    float3 lightDir;
    float3 lightColor;
    float lightAttenuation;
    float volumeScatter;
    float rangeBlend;
    ResolveLightAggregate(position.xyz, lightDir, lightColor, lightAttenuation, volumeScatter, rangeBlend);
    float lightIntensity = LightColor.a;

    
    float Metallic = material.r;
    float Roughness = material.g;
    float f0 = material.b;
    float3 surfaceNormal = normalize(normal.xyz);
    
    float3 NdotL = saturate(dot(surfaceNormal, lightDir));
    float3 environmentColor = EnvironmentTexture.SampleLevel(TextureSampler, input.TexCoord, Roughness * 10.0f).rgb;
    NdotL += lightIntensity;
    
    
    float3 diffuse = 0.0f;
    {
        float3 light = lightColor.rgb * saturate(dot(lightDir, surfaceNormal));
        
        float2 iblTexcoord;
        iblTexcoord.x = -atan2(surfaceNormal.x, surfaceNormal.z) / (PI * 2);
        iblTexcoord.y = asin(surfaceNormal.y) / PI;
        light += EnvironmentTexture.SampleLevel(TextureSampler, iblTexcoord, Roughness  * 10.0f).rgb * 6;
        diffuse = light * baseColor.rgb / PI;
    }
    
    if(shadow)
    {
        float shadowVisibility = SampleDeferredShadowMap(position.xyz, surfaceNormal, lightDir);
        float castShadow = saturate(1.0f - shadowVisibility);
        float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
        float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
        float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
        float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
        float3 shadowTint = saturate(float3(0.015f, 0.014f, 0.017f) + environmentColor * 0.16f + lightTint * 0.025f);
        float3 litColor = baseColor.rgb * (0.12f + NdotL * (0.22f + lightColor.rgb * 0.72f));
        baseColor.rgb = lerp(litColor, baseColor.rgb * shadowTint - 0.08f, shadowDensity);
    }

    if(lit)
    {
        float shadowVisibility = SampleDeferredShadowMap(position.xyz, surfaceNormal, lightDir);
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
        float PI = 3.14159265359f;

        float3 N = normalize(surfaceNormal + 0.00001f);
        float3 L = normalize(lightDir + 0.00001f);
        float3 V = normalize(CameraPos - position.xyz + 0.00001f);
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

        float r = roughness + 1.0f;
        float k = (r * r) / 8.0f;

        float G_L = NdotLScalar / max(NdotLScalar * (1.0f - k) + k, 0.00001f);
        float G_V = NdotV / max(NdotV * (1.0f - k) + k, 0.00001f);
        float G = G_L * G_V;

        float oneMinusVdotH = 1.0f - VdotH;
        float oneMinusVdotH2 = oneMinusVdotH * oneMinusVdotH;
        float oneMinusVdotH5 = oneMinusVdotH2 * oneMinusVdotH2 * oneMinusVdotH;

        float3 F = F0 + (1.0f - F0) * oneMinusVdotH5;

        float3 specularBRDF = D * G * F;
        specularBRDF /= max(4.0f * NdotLScalar * NdotV, 0.00001f);

        float3 kS = F;
        float3 kD = 1.0f - kS;
        kD *= 1.0f - metallic;

        float3 diffuseBRDF = kD * albedo / PI;

        float shadowVisibility = SampleDeferredShadowMap(position.xyz, N, L);

        float3 directLight =
        (diffuseBRDF + specularBRDF)
        * lightColor.rgb
        * lightIntensity
        * NdotLScalar
        * shadowVisibility;
        
        float3 R = reflect(-V, N);
        R = normalize(R);

        float2 reflectionUV;
        reflectionUV.x = atan2(R.z, R.x) / (2.0f * PI);
        reflectionUV.y = acos(R.y) / PI;

        float2 normalUV;
        normalUV.x = atan2(N.z, N.x) / (2.0f * PI);
        normalUV.y = acos(N.y) / PI;

        float maxMip = 8.0f;

        float3 envSpecular = EnvironmentTexture.SampleLevel(TextureSampler,reflectionUV,roughness * maxMip).rgb;

        float3 irradiance = EnvironmentTexture.SampleLevel(TextureSampler,normalUV,maxMip).rgb;

        float oneMinusNdotV = 1.0f - NdotV;
        float oneMinusNdotV2 = oneMinusNdotV * oneMinusNdotV;
        float oneMinusNdotV5 = oneMinusNdotV2 * oneMinusNdotV2 * oneMinusNdotV;

        float3 roughnessF0Max = max(
        float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness),F0);

        float3 F_IBL = F0 + (roughnessF0Max - F0) * oneMinusNdotV5;

        float3 ambientKD = 1.0f - F_IBL;
        ambientKD *= 1.0f - metallic;

        float3 ambientDiffuse = ambientKD * albedo * irradiance / ( 0.5f * PI);
        float3 ambientSpecular = envSpecular * F_IBL;

        float3 ambient = ambientDiffuse + ambientSpecular;

        baseColor.rgb = directLight + ambient;
    }

    if (brdf)
    {
        float3 N = normalize(surfaceNormal + 0.00001f);
        float3 L = normalize(lightDir + 0.00001f);
        float3 V = normalize(CameraPos - position.xyz + 0.00001f);
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
        float FPower = oneMinusVdotH * oneMinusVdotH * oneMinusVdotH * oneMinusVdotH * oneMinusVdotH;
        float3 F = F0 + (1.0f - F0) * FPower;

        float3 specularBRDF = (D * G * F) / max(4.0f * NdotLScalar * NdotV, 0.00001f);
        float3 diffuseBRDF = (1.0f - F) * (1.0f - metallic) * albedo / PI;
        float shadowVisibility = SampleDeferredShadowMap(position.xyz, N, L);

        float3 directReflection = (diffuseBRDF + specularBRDF) * lightColor.rgb * lightIntensity * NdotLScalar * shadowVisibility;
        float3 R = normalize(reflect(-V, N));
        float2 reflectionUV = float2(atan2(R.z, R.x) / (2.0f * PI), acos(R.y) / PI);
        float3 envReflection = EnvironmentTexture.SampleLevel(TextureSampler, reflectionUV, roughness * 8.0f).rgb * F;

        baseColor.rgb = directReflection + envReflection;
    }

    if (btdf)
    {
        float3 N = normalize(surfaceNormal + 0.00001f);
        float3 L = normalize(lightDir + 0.00001f);
        float3 V = normalize(CameraPos - position.xyz + 0.00001f);
        float roughness = clamp(material.g, 0.04f, 1.0f);
        float transmission = saturate(1.0f - material.r) * lerp(0.35f, 1.0f, roughness);
        float backNdotL = saturate(dot(-N, L));
        float frontNdotV = saturate(dot(N, V));
        float fresnel = pow(1.0f - frontNdotV, 5.0f);
        float shadowVisibility = SampleDeferredShadowMap(position.xyz, N, L);

        float eta = 1.0f / 1.45f;
        float3 T = normalize(refract(-V, N, eta));
        float2 transmissionUV = float2(atan2(T.z, T.x) / (2.0f * PI), acos(T.y) / PI);
        float3 transmittedEnv = EnvironmentTexture.SampleLevel(TextureSampler, transmissionUV, roughness * 8.0f).rgb;
        float3 transmittedLight = baseColor.rgb * lightColor.rgb * lightIntensity * backNdotL * transmission * shadowVisibility;
        float3 transmittedAmbient = baseColor.rgb * transmittedEnv * transmission * lerp(0.35f, 0.8f, fresnel);

        baseColor.rgb = transmittedLight + transmittedAmbient;
    }

    if (bsdf)
    {
        float3 N = normalize(surfaceNormal + 0.00001f);
        float3 L = normalize(lightDir + 0.00001f);
        float3 V = normalize(CameraPos - position.xyz + 0.00001f);
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
        float oneMinusVdotH = 1.0f - VdotH;
        float FPower = oneMinusVdotH * oneMinusVdotH * oneMinusVdotH * oneMinusVdotH * oneMinusVdotH;
        float3 F = F0 + (1.0f - F0) * FPower;
        float shadowVisibility = SampleDeferredShadowMap(position.xyz, N, L);

        float3 reflected = (((1.0f - F) * (1.0f - metallic) * albedo / PI) + ((D * G * F) / max(4.0f * NdotLScalar * NdotV, 0.00001f)))
            * lightColor.rgb * lightIntensity * NdotLScalar * shadowVisibility;

        float transmission = saturate(1.0f - metallic) * lerp(0.25f, 0.85f, roughness);
        float backNdotL = saturate(dot(-N, L));
        float3 transmitted = albedo * lightColor.rgb * lightIntensity * backNdotL * transmission * shadowVisibility;

        float3 R = normalize(reflect(-V, N));
        float2 reflectionUV = float2(atan2(R.z, R.x) / (2.0f * PI), acos(R.y) / PI);
        float3 envReflection = EnvironmentTexture.SampleLevel(TextureSampler, reflectionUV, roughness * 8.0f).rgb * F;

        float eta = 1.0f / 1.45f;
        float3 T = normalize(refract(-V, N, eta));
        float2 transmissionUV = float2(atan2(T.z, T.x) / (2.0f * PI), acos(T.y) / PI);
        float3 envTransmission = EnvironmentTexture.SampleLevel(TextureSampler, transmissionUV, roughness * 8.0f).rgb * albedo * transmission;

        baseColor.rgb = reflected + transmitted + envReflection + envTransmission * 0.5f;
    }

    baseColor.a = 1.0f;
    return baseColor;
}
