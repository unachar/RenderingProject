Texture2D<float4> MaterialEmissionAO : register(t33);
#define SHADER_POSTPROCESS
#define SHADER_3D
#include "common.hlsl"
// Physical sky from the atmosphere LUT (bound at root parameter 26, t40/t41, clamp sampler s4).
#define SKY_LUT 1
#define SKY_LUT_SCATTER_REGISTER t40
#define SKY_LUT_TRANSMISSION_REGISTER t41
#define SKY_LUT_SAMPLER_REGISTER s4
#include "SkyAtmosphere.hlsli"
#include "CloudMedium.hlsli"
#pragma warning(disable: 4000)

Texture2D<float4> BaseColorTexture : register(t0);
Texture2D<float4> NormalTexture : register(t1);
Texture2D<float> DepthTexture : register(t2);
Texture2D<float4> MaterialTexture : register(t3);
Texture2D<float4> ShadowGBufferTexture : register(t4);
Texture2D<float4> EnvironmentTexture : register(t6);
Texture2DArray<float> ShadowMapTexture : register(t7);
Texture2D<float4> AtmosphereTexture : register(t8);
Texture2D<float4> MonitorTexture : register(t12);
Texture2D<float> VisibilityBitmaskAO : register(t13);
Texture2D<float4> DeinterleavedSSGI : register(t14);
Texture2D<float2> RaytracedVisibility : register(t16);
Texture2D<float4> RaytracedReflections : register(t17);

SamplerState TextureSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

// DFG / LTC tables (root parameter 27: t42, t43).
#define SURFACE_PBR_LUTS 1
#include "SurfaceLighting.hlsli"
#include "WeatherSurface.hlsli"

float4 main(PSInputPostProcess input) : SV_Target
{

    const float PI = 3.14159265358979323846f;
    // Lighting runs at GBuffer resolution. Integer fetches preserve material
    // classes and avoid filtering work for these per-pixel attributes.
    const int3 gbufferPixel = int3(int2(input.Position.xy), 0);
    float4 material = MaterialTexture.Load(gbufferPixel);
    float shaderClass = material.a;
    bool background = shaderClass < -0.5f;
    bool transparent = IsMaterialClass(shaderClass, 0.0f);
	float4 baseColor = BaseColorTexture.Load(gbufferPixel);
    float3 giReflectance = baseColor.rgb * (1 - saturate(material.x));

	if (transparent)
	{
		return baseColor;
	}

	float4 atmosphereMedia = AtmosphereTexture.SampleLevel(
		TextureSampler, input.TexCoord, 0);
	float fogTransmittance = saturate(atmosphereMedia.a);
	if (background)
	{
		if (SkyParameters.x > 0.5f)
		{
			// The sky integral already includes atmospheric extinction and scattering.
			return float4(EvaluateSkyAtmosphere(ReconstructPostProcessViewRayCommon(input.TexCoord)), 1.0f);
		}
		baseColor.rgb =
			(baseColor.rgb + AtmosphereBackgroundCommon(input.TexCoord)) *
			fogTransmittance + atmosphereMedia.rgb;
		return baseColor;
	}

	bool toon = IsMaterialClass(shaderClass, 4.0f);
	bool shadow = IsMaterialClass(shaderClass, 5.0f);
	bool lit = IsMaterialClass(shaderClass, 8.0f);
	bool pbr = IsMaterialClass(shaderClass, 11.0f);
	const int shadowDebugMode = (int)round(ShadowDebugGlobal.x);
	const bool needsDeferredLighting =
		toon || shadow || lit || pbr || shadowDebugMode > 0;
	[branch]
	if (!needsDeferredLighting)
	{
		baseColor.rgb = baseColor.rgb * fogTransmittance + atmosphereMedia.rgb;
		baseColor.a = 1.0f;
		return baseColor;
	}

	float4 normal = NormalTexture.Load(gbufferPixel);
	float depth = DepthTexture.Load(gbufferPixel);
	float3 position = ReconstructPostProcessWorldPositionCommon(input.TexCoord, depth);
    float3 positionDx = ddx(position);
    float3 positionDy = ddy(position);
	float4 shadowParams = ShadowGBufferTexture.Load(gbufferPixel);


    float shadowThreshold = shadowParams.r;
    float shadowSoftness = max(shadowParams.g, 0.0001f);
    float shadowStrength = saturate(shadowParams.b);



    float3 lightDir;
    float3 lightColor;
    float lightAttenuation;
    float volumeScatter;
    float rangeBlend;
    float aggregateShadowVisibility;
    float3 surfaceNormal = DecodeGBufferNormal(normal.xyz);
    if (pbr)
    {
        // Extended material model (shadow GBuffer alpha) and weather on PBR surfaces.
        g_ShadingModel = DecodeShadingModel(shadowParams.a);
        ApplyWeatherToSurface(position, surfaceNormal, baseColor.rgb, material.rgb);
    }
    float3 viewDir = SafeNormalizeCommon(PPCameraPos.xyz - position, surfaceNormal);
    float3 pbrDirectLight;
    int materialShadowFilterRadius =
        pbr
            ? clamp(
                (int)round(lerp(
                    max(VirtualShadowGlobal.z, 1.0f),
                    3.0f,
                    saturate(shadowSoftness * 2.0f))),
                1,
                3)
            : clamp((int)round(VirtualShadowGlobal.z), 0, 3);
    ResolveSurfaceLightAggregateShadowed(
        position,
        positionDx,
        positionDy,
        surfaceNormal,
        input.Position.xy,
        materialShadowFilterRadius,
        pbr,
        viewDir,
        baseColor.rgb,
        material.rgb,
        shadowSoftness,
        pbrDirectLight,
        lightDir,
        lightColor,
        lightAttenuation,
        volumeScatter,
        rangeBlend,
        aggregateShadowVisibility);

    if (shadowDebugMode == 1 && VirtualShadowGlobal.x > 0.5f)
    {


        return float4(aggregateShadowVisibility.xxx, 1.0f);
    }
    if (shadowDebugMode > 1 &&
        VirtualShadowGlobal.x > 0.5f && VirtualShadowGlobal.x < 1.5f)
    {
        int selectedLevel = max((int)round(VirtualShadowGlobal.y) - 1, 0);
        float2 selectedUv = float2(0.5f, 0.5f);
        [unroll]
        for (int level = 0; level < 4; ++level)
        {
            if (level >= (int)round(VirtualShadowGlobal.y)) break;
            float4 clip = mul(float4(position.xyz, 1.0f), VirtualShadowViewProjections[level]);
            float3 ndc = clip.xyz / max(clip.w, 0.000001f);
            float2 uv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
            selectedUv = uv;
            float pageGrid = max(ShadowDebugGlobal.w, 1.0f);
            float residentGrid = max(ShadowDebugGlobal.z, 1.0f);
            float firstResident = (pageGrid - residentGrid) * 0.5f;
            float2 pageCoord = floor(saturate(uv) * pageGrid);
            if (all(uv >= 0.0f) && all(uv < 1.0f) &&
                all(pageCoord >= firstResident) &&
                all(pageCoord < firstResident + residentGrid))
            {
                selectedLevel = level;
                break;
            }
        }
        if (shadowDebugMode == 2)
        {
            const float3 levelColors[4] = {
                float3(0.10f, 0.85f, 0.30f), float3(0.15f, 0.45f, 1.00f),
                float3(1.00f, 0.70f, 0.10f), float3(0.95f, 0.18f, 0.25f) };
            return float4(levelColors[selectedLevel], 1.0f);
        }
        float2 page = floor(saturate(selectedUv) * max(ShadowDebugGlobal.w, 1.0f));
        float pageHash = frac(dot(page, float2(0.06711056f, 0.00583715f)));
        float3 pageColor = frac(pageHash + float3(0.0f, 0.333f, 0.667f));
        return float4(pageColor, 1.0f);
    }
    [branch]
    if (shadow || toon || lit)
    {
        float lightIntensity = LightColor.a;


        float Roughness = material.g;

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
            float castShadow = saturate(1.0f - aggregateShadowVisibility);
            float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
            float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
            float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
            float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
            float3 shadowTint = saturate(float3(0.015f, 0.014f, 0.017f) + environmentColor * 0.16f + lightTint * 0.025f);
            float3 litColor = baseColor.rgb * (0.12f + NdotL * (0.22f + lightColor.rgb * 0.72f));
            baseColor.rgb = lerp(litColor, baseColor.rgb * shadowTint - 0.08f, shadowDensity);
        }

        if(toon)
        {
            float castShadow = saturate(1.0f - aggregateShadowVisibility);
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

        if(lit)
        {
            float castShadow = saturate(1.0f - aggregateShadowVisibility);
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

    }

    float4 authoredEmissionAO = MaterialEmissionAO.Load(gbufferPixel);
    if (pbr)
    {
        SurfaceData surface = BuildLightingSurface(baseColor, surfaceNormal, material.rgb, authoredEmissionAO);
        // Emission is applied after the screen-space effects below.
        surface.Emissive = 0.0f;
        baseColor.rgb = EvaluateSurfaceEnvironment(surface, viewDir, position,
            input.TexCoord, shadowSoftness, pbrDirectLight);
    }


    baseColor.rgb = ApplySurfaceScreenEffects(baseColor.rgb, authoredEmissionAO.rgb, input.TexCoord, giReflectance);
    baseColor.a = 1.0f;
    return baseColor;
}


