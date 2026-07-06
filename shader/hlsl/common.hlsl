// ==========================================================
// common.hlsl
// ==========================================================

struct GBufferOutput
{
    float4 Color : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
    float4 Depth : SV_Target3;
    float4 Material : SV_Target4;
    float4 Shadow : SV_Target5;
    float4 RimStyle : SV_Target6;
    float4 RimLight : SV_Target7;
};

struct PS_OUTPUT_GEOMETRY
{
    float4 Color : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
    float4 Depth : SV_Target3;
    float4 Material : SV_Target4;
    float4 Shadow : SV_Target5;
    float4 RimStyle : SV_Target6;
    float4 RimLight : SV_Target7;
};

struct PS_OUTPUT
{
    float4 Color : SV_Target;
};



float4 MakeGBufferNormal(float3 normal)
{
    return float4(normalize(normal), 1.0f);
}

float3 DecodeGBufferNormal(float3 normal)
{
    return normalize(normal);
}

#ifdef SHADER_3D

// ----------------------------------------------------------
// 3D 
// ----------------------------------------------------------

cbuffer ConstantBuffer3D : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Projection;
    int UseTexture;
    int FlipNormal;
    int UseNormalMap;
    int MaterialMode;
    float3 CameraPos;
    int ShaderClass;
    float mMetallic;
    float mRoughness;
    float mFresnel;
    float Padding;
    float ToonOutlineWidth;
    float ToonOutlineScreenWidth;
    float2 ViewportSize;
    int ToonOutlineUseScreenSpace;
    float MaterialAlpha;
    float2 ConstantPadding;
};

struct VSInput3D
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Diffuse  : COLOR;
};

struct PSInput3D
{
    float4 Position : SV_POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Diffuse  : COLOR;
    float3 WorldPos : TEXCOORD1;
    float3 ViewPos  : TEXCOORD2;
    float3 CameraPos : TEXCOORD3;
};

Texture2D<float> g_ShadowMap : register(t1);
SamplerState g_ShadowSampler : register(s1);

cbuffer ShadowParams : register(b3)
{
    float4x4 LightViewProjection;
    float4 ShadowMapParams; // x: texel size, y: depth bias, z: normal bias, w: strength
};

float SampleShadowMap(float3 worldPos, float3 normal, float3 lightDir)
{
    float normalLenSq = dot(normal, normal);
    float lightLenSq = dot(lightDir, lightDir);
    float3 n = (normalLenSq > 0.000001f) ? normal * rsqrt(normalLenSq) : float3(0.0f, 1.0f, 0.0f);
    float3 l = (lightLenSq > 0.000001f) ? lightDir * rsqrt(lightLenSq) : float3(0.0f, 1.0f, 0.0f);

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
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float closestDepth = g_ShadowMap.SampleLevel(g_ShadowSampler, shadowUv + float2(x, y) * texelSize, 0);
            visibility += (currentDepth <= closestDepth) ? 1.0f : 0.0f;
        }
    }

    visibility /= 9.0f;
    return lerp(1.0f, lerp(1.0f, visibility, inBounds), saturate(ShadowMapParams.w));
}

struct Light
{
    float3 Position;
    float3 Color;
    float3 Direction;
    float Intensity;
};

#endif // SHADER_3D

#if defined(SHADER_3D) || defined(SHADER_POSTPROCESS)

#define MAX_SHADER_LIGHTS 20

cbuffer LightParams : register(b1)
{
    float4 LightDirection;
    float4 LightColor;
    float4 LightPositionType; // xyz: position, w: 0 directional / 1 point / 2 spot / 3 volume
    float4 LightExtra;        // x: spot inner cos, y: spot outer cos, z: volume density, w: volume shape
    float4 LightCount;
    float4 LightDirections[MAX_SHADER_LIGHTS];
    float4 LightColors[MAX_SHADER_LIGHTS];
    float4 LightPositionTypes[MAX_SHADER_LIGHTS];
    float4 LightExtras[MAX_SHADER_LIGHTS];
    float4 AtmosphereParams0; // x: enabled, y: rayleigh, z: mie, w: density
    float4 AtmosphereParams1; // x: height falloff, y: extinction, z: mie g, w: distance scale
    float4 AtmosphereColor0;  // rgb: rayleigh color, a: light shaft strength
    float4 AtmosphereColor1;  // rgb: mie color, a: ambient strength
    float4 AtmosphereCamera;  // xyz: camera position
};

float3 SafeNormalizeCommon(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return (lenSq > 0.000001f) ? value * rsqrt(lenSq) : fallback;
}

float3 ApplyNormalMapCommon(float3 meshNormal, float3 worldPos, float2 texCoord, float3 tangentNormal)
{
    float3 n = SafeNormalizeCommon(meshNormal, float3(0.0f, 1.0f, 0.0f));
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(texCoord);
    float2 duv2 = ddy(texCoord);

    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    float useTbn = (abs(det) >= 0.000000000000000001f) ? 1.0f : 0.0f;
    float invDet = rcp(lerp(1.0f, det, useTbn));
    float3 tangent = (dp1 * duv2.y - dp2 * duv1.y) * invDet;
    tangent = SafeNormalizeCommon(tangent - n * dot(n, tangent), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = SafeNormalizeCommon(cross(n, tangent), float3(0.0f, 0.0f, 1.0f));
    float3 mappedNormal = SafeNormalizeCommon(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * n, n);
    return SafeNormalizeCommon(lerp(n, mappedNormal, useTbn), n);
}

float Hash13Common(float3 p)
{
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float ValueNoiseCommon(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);

    float n000 = Hash13Common(i + float3(0.0f, 0.0f, 0.0f));
    float n100 = Hash13Common(i + float3(1.0f, 0.0f, 0.0f));
    float n010 = Hash13Common(i + float3(0.0f, 1.0f, 0.0f));
    float n110 = Hash13Common(i + float3(1.0f, 1.0f, 0.0f));
    float n001 = Hash13Common(i + float3(0.0f, 0.0f, 1.0f));
    float n101 = Hash13Common(i + float3(1.0f, 0.0f, 1.0f));
    float n011 = Hash13Common(i + float3(0.0f, 1.0f, 1.0f));
    float n111 = Hash13Common(i + float3(1.0f, 1.0f, 1.0f));

    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);
    float nxy0 = lerp(nx00, nx10, f.y);
    float nxy1 = lerp(nx01, nx11, f.y);
    return lerp(nxy0, nxy1, f.z);
}

float RayleighPhaseCommon(float cosTheta)
{
    return 0.0596831f * (1.0f + cosTheta * cosTheta);
}

float HenyeyGreensteinCommon(float cosTheta, float g)
{
    g = clamp(g, -0.95f, 0.95f);
    float g2 = g * g;
    float denom = max(1.0f + g2 - 2.0f * g * cosTheta, 0.0001f);
    return 0.0795775f * (1.0f - g2) / max(pow(denom, 1.5f), 0.0001f);
}

float AtmosphereDensityCommon(float3 worldPos)
{
    float density = max(AtmosphereParams0.w, 0.0f);
    float heightFalloff = max(AtmosphereParams1.x, 0.0f);
    float heightDensity = exp(-max(worldPos.y, 0.0f) * heightFalloff);
    float noise = lerp(0.92f, 1.08f, ValueNoiseCommon(worldPos * 0.065f + AtmosphereCamera.xyz * 0.011f));
    return density * heightDensity * noise;
}

float3 ApplyAtmosphereToLightCommon(
    float3 worldPos,
    float3 lightDir,
    float3 lightColor,
    inout float volumeScatter)
{
    float atmosphereEnabled = step(0.5f, AtmosphereParams0.x);
    float3 toCamera = AtmosphereCamera.xyz - worldPos;
    float viewDistance = length(toCamera);
    float3 viewToCamera = SafeNormalizeCommon(toCamera, -lightDir);
    float3 toLight = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
    float cosTheta = clamp(dot(toLight, viewToCamera), -1.0f, 1.0f);

    float opticalDepth = AtmosphereDensityCommon(worldPos) *
        (1.0f - exp(-viewDistance * max(AtmosphereParams1.w, 0.0001f)));
    float transmittance = exp(-max(AtmosphereParams1.y, 0.0f) * opticalDepth);

    float rayleighPhase = RayleighPhaseCommon(cosTheta);
    float miePhase = HenyeyGreensteinCommon(cosTheta, AtmosphereParams1.z);
    float3 rayleigh = AtmosphereColor0.rgb * max(AtmosphereParams0.y, 0.0f) * rayleighPhase;
    float3 mie = AtmosphereColor1.rgb * max(AtmosphereParams0.z, 0.0f) * miePhase;
    float3 inScatter = (rayleigh + mie) * opticalDepth * lightColor;

    float shaftStrength = max(AtmosphereColor0.a, 0.0f);
    volumeScatter += dot((rayleigh + mie) * opticalDepth, float3(0.299f, 0.587f, 0.114f)) * shaftStrength * atmosphereEnabled;
    return lerp(lightColor, lightColor * transmittance + inScatter, atmosphereEnabled);
}

float3 AtmosphereAmbientCommon(float3 worldPos, float3 lightDir)
{
    float atmosphereEnabled = step(0.5f, AtmosphereParams0.x);
    float upScatter = saturate(SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f)).y * 0.5f + 0.5f);
    float density = AtmosphereDensityCommon(worldPos);
    return AtmosphereColor0.rgb * AtmosphereColor1.a * density * lerp(0.35f, 1.0f, upScatter) * atmosphereEnabled;
}

void ResolveSingleLightCommon(
    float3 worldPos,
    float4 lightDirectionData,
    float4 lightPositionTypeData,
    float4 lightExtraData,
    out float3 lightDir,
    out float attenuation,
    out float volumeScatter)
{
    int lightType = (int)round(lightPositionTypeData.w);
    float lightRange = max(lightDirectionData.w, 0.01f);
    lightDir = SafeNormalizeCommon(lightDirectionData.xyz, float3(0.0f, 1.0f, 0.0f));
    attenuation = 1.0f;
    volumeScatter = 0.0f;

    if (lightType == 1 || lightType == 2 || lightType == 3)
    {
        float3 toLight = lightPositionTypeData.xyz - worldPos;
        float distanceToLight = length(toLight);
        lightDir = SafeNormalizeCommon(toLight, lightDir);
        float rangeFade = saturate(1.0f - distanceToLight / lightRange);
        attenuation = rangeFade * rangeFade;
        float3 fromLight = SafeNormalizeCommon(worldPos - lightPositionTypeData.xyz, float3(0.0f, -1.0f, 0.0f));
        float3 spotForward = SafeNormalizeCommon(lightDirectionData.xyz, fromLight);

        if (lightType == 2 || lightType == 3)
        {
            float spotCos = dot(fromLight, spotForward);
            float spotMask = smoothstep(lightExtraData.y, lightExtraData.x, spotCos);
            attenuation *= spotMask;
        }

        if (lightType == 3)
        {
            float density = max(lightExtraData.z, 0.0f);
            float volumeShape = round(lightExtraData.w);
            float3 fromLightOffset = worldPos - lightPositionTypeData.xyz;
            float axialDistance = max(dot(fromLightOffset, spotForward), 0.0f);
            float radialDistance = length(fromLightOffset - spotForward * axialDistance);
            float outerCos = clamp(lightExtraData.y, 0.001f, 0.999f);
            float outerSin = sqrt(saturate(1.0f - outerCos * outerCos));
            float outerTan = outerSin / outerCos;
            float coneRadius = max(axialDistance * outerTan, 0.001f);
            float cylinderRadius = max(lightRange * outerTan * 0.35f, 0.001f);
            float coneMask = saturate(1.0f - radialDistance / coneRadius) * step(0.0f, axialDistance) * saturate(1.0f - axialDistance / lightRange);
            float cylinderMask = saturate(1.0f - radialDistance / cylinderRadius) * step(0.0f, axialDistance) * saturate(1.0f - axialDistance / lightRange);
            float shapeMask = lerp(coneMask, cylinderMask, saturate(volumeShape));
            float distanceFade = pow(saturate(1.0f - distanceToLight / lightRange), 1.35f);
            float noise = lerp(0.86f, 1.14f, ValueNoiseCommon(worldPos * 1.65f + lightPositionTypeData.xyz * 0.17f));
            attenuation = lerp(attenuation, rangeFade * rangeFade * cylinderMask, saturate(volumeShape));
            volumeScatter = shapeMask * distanceFade * density * noise * 0.42f;
        }
    }
}

void ResolveLightCommon(float3 worldPos, out float3 lightDir, out float attenuation, out float volumeScatter)
{
    ResolveSingleLightCommon(worldPos, LightDirection, LightPositionType, LightExtra, lightDir, attenuation, volumeScatter);
}

void ResolveLightAggregate(
    float3 worldPos,
    out float3 lightDir,
    out float3 lightColor,
    out float attenuation,
    out float volumeScatter,
    out float rangeBlend)
{
    lightDir = SafeNormalizeCommon(LightDirection.xyz, float3(0.0f, 1.0f, 0.0f));
    lightColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f);
    attenuation = 1.0f;
    volumeScatter = 0.0f;
    rangeBlend = saturate((max(LightDirection.w, 1.0f) - 1.0f) / 7.0f);

    int count = min((int)round(LightCount.x), MAX_SHADER_LIGHTS);
    float3 dirSum = float3(0.0f, 0.0f, 0.0f);
    float3 colorSum = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;
    float maxAttenuation = 0.0f;
    float volumeSum = 0.0f;
    float rangeSum = 0.0f;

    [loop]
    for (int i = 0; i < MAX_SHADER_LIGHTS; ++i)
    {
        if (i >= count)
        {
            break;
        }

        float3 singleDir;
        float singleAttenuation;
        float singleVolume;
        ResolveSingleLightCommon(worldPos, LightDirections[i], LightPositionTypes[i], LightExtras[i], singleDir, singleAttenuation, singleVolume);

        float3 singleColor = max(LightColors[i].rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColors[i].a, 0.0f) * singleAttenuation;
        singleColor = ApplyAtmosphereToLightCommon(worldPos, singleDir, singleColor, singleVolume);
        float weight = max(dot(singleColor, float3(0.299f, 0.587f, 0.114f)), 0.0001f) * singleAttenuation;
        dirSum += singleDir * weight;
        colorSum += singleColor;
        weightSum += weight;
        maxAttenuation = max(maxAttenuation, singleAttenuation);
        volumeSum += singleVolume * max(LightColors[i].a, 0.0f);
        rangeSum += saturate((max(LightDirections[i].w, 1.0f) - 1.0f) / 7.0f) * weight;
    }

    if (count <= 0 || weightSum <= 0.000001f)
    {
        float legacyVolume = 0.0f;
        ResolveLightCommon(worldPos, lightDir, attenuation, legacyVolume);
        lightColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f) * attenuation;
        lightColor = ApplyAtmosphereToLightCommon(worldPos, lightDir, lightColor, legacyVolume);
        volumeScatter = legacyVolume * max(LightColor.a, 0.0f);
        rangeBlend = saturate((max(LightDirection.w, 1.0f) - 1.0f) / 7.0f);
        return;
    }

    lightDir = SafeNormalizeCommon(dirSum, SafeNormalizeCommon(LightDirection.xyz, float3(0.0f, 1.0f, 0.0f)));
    lightColor = colorSum + AtmosphereAmbientCommon(worldPos, lightDir);
    attenuation = saturate(maxAttenuation);
    volumeScatter = volumeSum;
    rangeBlend = saturate(rangeSum / weightSum);
}

struct MaterialPartShaderParams
{
    float4 Basic;       // x: metallic, y: roughness, z: fresnel, w: normal blend
    float4 Base;        // x: normal bias, y: saturation, z: brightness, w: kawaii blend
    float4 Shadow0;     // x: threshold, y: softness, z: strength, w: mid strength
    float4 Shadow1;     // x: lit strength, y: cast threshold, z: cast softness, w: unused
    float4 Highlight;   // x: rim strength, y: rim threshold, z: specular strength, w: specular threshold
    float4 Skin0;       // x: scatter strength, y: scatter wrap, z: backlight strength, w: rim scatter strength
    float4 Skin1;       // x: oil specular strength, y: shadow scatter, z: unused, w: unused
};

cbuffer PBRParams : register(b2)
{
    float Metallic;
    float Roughness;
    float Fresnel;
    float NormalBlend;
    float NormalBias;
    float BaseSaturation;
    float BaseBrightness;
    float ShadowThreshold;
    float ShadowSoftness;
    float ShadowStrength;
    float MidStrength;
    float LitStrength;
    float RimStrength;
    float RimThreshold;
    float SpecularStrength;
    float SpecularThreshold;
    float KawaiiBlend;
    float SkinScatterStrength;
    float SkinScatterWrap;
    float SkinBacklightStrength;
    float SkinRimScatterStrength;
    float SkinOilSpecularStrength;
    float SkinShadowScatter;
    float CastShadowThreshold;
    float CastShadowSoftness;
    float3 PBRPadding;
    MaterialPartShaderParams PartParams[15];
};

MaterialPartShaderParams ResolveMaterialPartParams(float materialPartId)
{
    int index = clamp((int)round(materialPartId), 0, 11);
    return PartParams[index];
}

#endif


#ifdef SHADER_2D

// ----------------------------------------------------------
// 2D 
// ----------------------------------------------------------

float3 SafeNormalizeCommon(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return (lenSq > 0.000001f) ? value * rsqrt(lenSq) : fallback;
}

float3 ApplyNormalMapCommon(float3 meshNormal, float3 worldPos, float2 texCoord, float3 tangentNormal)
{
    float3 n = SafeNormalizeCommon(meshNormal, float3(0.0f, 0.0f, -1.0f));
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(texCoord);
    float2 duv2 = ddy(texCoord);

    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    float useTbn = (abs(det) >= 0.0000000000000000001f) ? 1.0f : 0.0f;
    float invDet = rcp(lerp(1.0f, det, useTbn));
    float3 tangent = (dp1 * duv2.y - dp2 * duv1.y) * invDet;
    tangent = SafeNormalizeCommon(tangent - n * dot(n, tangent), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = SafeNormalizeCommon(cross(n, tangent), float3(0.0f, 1.0f, 0.0f));
    float3 mappedNormal = SafeNormalizeCommon(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * n, n);
    return SafeNormalizeCommon(lerp(n, mappedNormal, useTbn), n);
}

cbuffer ConstantBuffer2D : register(b0)
{
    float2 Offset;
    int UseTexture;
    float AspectRatio;
    float2 Scale;
    int UseNormalMap;
    int MaterialMode;
    int ShaderClass;
    float MaterialAlpha2D;
    float2 Padding2D;
};


struct VSInput2D
{
    float3 Position : POSITION;
    float4 Color    : COLOR;
};

struct PSInput2D
{
    float4 Position : SV_POSITION;
    float3 Normal   : NORMAL;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD;
    float3 WorldPos : TEXCOORD1;
};

#endif // SHADER_2D


#ifdef SHADER_DEBUG_LINE

// ----------------------------------------------------------
// Debug Line 
// ----------------------------------------------------------

cbuffer cbModelDebugLine : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Projection;
    int UseTexture;
};

struct VSInputDebugLine
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Color    : COLOR;
};

struct PSInputDebugLine
{
    float4 Position : SV_POSITION;
    float4 Color    : COLOR;
    float3 WorldPos : TEXCOORD0;
};

#endif // SHADER_DEBUG_LINE
 
 
#ifdef SHADER_POSTPROCESS
 
// ----------------------------------------------------------
// PostProcess 
// ----------------------------------------------------------

float3 ACESToneMap(float3 color)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    color = saturate((color * (a * color + b)) / (color * (c * color + d) + e));
    return color;
}

cbuffer PostProcessParams : register(b0)
{
    float4 Flags;       // x=Exposure, y=Intensity, z=RenderModeFlag, w=unused
    float4 PPCameraPos; // xyz=CameraPosition, w=unused
    float4 HdrFlags;    // x=HdrEnabled, y=ToneMapEnabled, zw=unused
};

float3 ApplyHdrOutput(float3 color)
{
    float exposure = Flags.x;
    float enableACES = HdrFlags.y;
    if (exposure > 0.001f)
    {
        color *= exposure;
    }
    if (enableACES > 0.5f)
    {
        color = ACESToneMap(color);
    }
    return color;
}

struct PSInputPostProcess
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};
 
#endif
