// ==========================================================
// common.hlsl
// ==========================================================
#pragma warning(disable: 4000)

struct GBufferOutput
{
    float4 Color : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Depth : SV_Target2;
    float4 Material : SV_Target3;
    float4 Shadow : SV_Target4;
};

struct PS_OUTPUT_GEOMETRY
{
    float4 Color : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Depth : SV_Target2;
    float4 Material : SV_Target3;
    float4 Shadow : SV_Target4;
};

struct PS_OUTPUT
{
    float4 Color : SV_Target;
};



float4 MakeGBufferNormal(float3 normal)
{
    return float4(normalize(normal) * 0.5f + 0.5f, 1.0f);
}

float3 DecodeGBufferNormal(float3 normal)
{
    return normalize(normal * 2.0f - 1.0f);
}

#ifdef SHADER_3D

// ----------------------------------------------------------
// 3D 
// ----------------------------------------------------------

#ifndef SHADER_POSTPROCESS
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
    int MaterialIsTransparent;
    float ConstantPadding;
    float4x4 PreviousWorld;
    float4x4 PreviousViewProjection;
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

Texture2DArray<float> g_ShadowMap : register(t1);
SamplerComparisonState g_ShadowSampler : register(s1);
#endif

cbuffer ShadowParams : register(b3)
{
    float4x4 LightViewProjection;
    float4 ShadowMapParams; // x: texel size, y: depth bias, z: normal bias, w: strength
    float4 ShadowFilterParams; // x: PCF radius (0-3)
};

#ifndef SHADER_POSTPROCESS
float SampleShadowMapPcf9(float2 shadowUv, float shadowLayer, float texelSize, float currentDepth, int filterRadius)
{
    const float2 kernel[9] =
    {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f), float2(-1.0f, 0.0f),
        float2(0.0f, 1.0f), float2(0.0f, -1.0f),
        float2(0.7071068f, 0.7071068f), float2(-0.7071068f, 0.7071068f),
        float2(0.7071068f, -0.7071068f), float2(-0.7071068f, -0.7071068f)
    };
    int tapCount = filterRadius <= 0 ? 1 : 9;
    float visibility = 0.0f;
    [unroll]
    for (int tap = 0; tap < 9; ++tap)
    {
        if (tap >= tapCount) break;
        float2 offset = kernel[tap] * (float)max(filterRadius, 1) * texelSize;
        visibility += g_ShadowMap.SampleCmpLevelZero(
            g_ShadowSampler,
            float3(shadowUv + offset, shadowLayer),
            currentDepth);
    }
    return visibility / (float)tapCount;
}

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

    int filterRadius = clamp((int)round(ShadowFilterParams.x), 0, 3);
    float visibility = SampleShadowMapPcf9(
        shadowUv, 0.0f, texelSize, currentDepth, filterRadius);
    float shadowStrength = saturate(abs(ShadowMapParams.w));
    float outOfBoundsVisibility = 1.0f;
    return lerp(1.0f, lerp(outOfBoundsVisibility, visibility, inBounds), shadowStrength);
}
#endif

struct Light
{
    float3 Position;
    float3 Color;
    float3 Direction;
    float Intensity;
};

#endif // SHADER_3D

#if defined(SHADER_3D) || defined(SHADER_POSTPROCESS)

#define MAX_SHADER_LIGHTS 160
#define LIGHT_TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 8

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
    float4x4 LightViewProjections[MAX_SHADER_LIGHTS];
    float4 LightShadowData[MAX_SHADER_LIGHTS]; // x: shadow layer, y: texel size, z: depth bias, w: normal bias
    float4 LightFlags[MAX_SHADER_LIGHTS]; // x: opaque, y: forward, z: volumetric, w: render mode
    float4x4 VirtualShadowViewProjections[4];
    float4 VirtualShadowParams[4]; // x: physical layer, y: texel size, z: depth bias, w: normal bias
    float4 VirtualShadowPageOrigins[4]; // xy: global page origin, z: page grid, w: page world size
    uint4 VirtualShadowResidency[16]; // four packed 16-row masks per clipmap level
    float4 VirtualShadowGlobal; // x: mode, y: level count, z: PCF radius, w: light-space overlap fraction
    float4 ShadowRuntimeGlobal; // x: contact enabled, y: length, z: steps, w: method
    float4 ShadowDebugGlobal; // x: mode, y: cache hit, z: resident pages per dimension, w: pages per dimension
    float4 DistanceFieldData0[16]; // xyz: world AABB center, w: active
    float4 DistanceFieldData1[16]; // xyz: world AABB extents
    float4 DistanceFieldGlobal; // x: object count, y: ray distance, z: steps
    float4 LocalFogData0[16]; // xyz: center, w: radius
    float4 LocalFogData1[16]; // x: height falloff, y: density, z: shape, w: enabled
    float4 LocalFogColors[16];
    float4 LocalFogGlobal; // x: active volume count
    float4 AtmosphereParams0; // x: enabled, y: rayleigh, z: mie, w: density
    float4 AtmosphereParams1; // x: height falloff, y: extinction, z: mie g, w: distance scale
    float4 AtmosphereColor0;  // rgb: rayleigh color, a: light shaft strength
    float4 AtmosphereColor1;  // rgb: mie color, a: ambient strength
    float4 AtmosphereCamera;  // xyz: camera position
};

// GPU-built 16x16 screen tiles keep the cost proportional to local overlap,
// rather than to the number of authored lights. Each tile is a fixed four-
// index block ordered by light priority; 0xffffffff marks an unused slot.
StructuredBuffer<uint> LightTileIndices : register(t11);
StructuredBuffer<uint> VolumetricLightIndices : register(t24);

bool HasLightTileGridCommon()
{
    return LightCount.y >= 1.0f && LightCount.z >= 1.0f && LightCount.w >= 1.0f;
}

uint LightTileBaseCommon(float2 pixelPosition)
{
    uint tileCountX = max((uint)round(LightCount.y), 1u);
    uint tileCountY = max((uint)round(LightCount.z), 1u);
    uint2 tile = min(
        (uint2)max(pixelPosition, float2(0.0f, 0.0f)) / LIGHT_TILE_SIZE,
        uint2(tileCountX - 1u, tileCountY - 1u));
    return (tile.y * tileCountX + tile.x) * MAX_LIGHTS_PER_TILE;
}

uint VirtualShadowResidencyRowCommon(int level, int row)
{
    uint4 packedRows = VirtualShadowResidency[level * 4 + (row >> 2)];
    return packedRows[row & 3];
}

bool VirtualShadowPageResidentCommon(int level, int2 localPage)
{
    int pageGrid = max((int)round(VirtualShadowPageOrigins[level].z), 1);
    if (level < 0 || level >= 4 ||
        localPage.x < 0 || localPage.x >= pageGrid ||
        localPage.y < 0 || localPage.y >= pageGrid)
    {
        return false;
    }
    uint rowMask = VirtualShadowResidencyRowCommon(level, localPage.y);
    return (rowMask & (1u << localPage.x)) != 0u;
}

int VirtualShadowPositiveModuloCommon(int value, int modulus)
{
	// The physical VSM page grid is fixed at 16x16.  Its power-of-two mask is
	// equivalent to positive modulo for both positive and negative page IDs and
	// avoids a costly signed integer divide in every shadow tap.
	return value & (modulus - 1);
}

float2 VirtualShadowMapUvCommon(
    int level,
    float2 virtualUv,
    out bool resident,
    out int2 localPage)
{
    int pageGrid = max((int)round(VirtualShadowPageOrigins[level].z), 1);
    float2 pagePosition = virtualUv * (float)pageGrid;
    localPage = (int2)floor(pagePosition);
    resident =
        all(virtualUv >= 0.0f) && all(virtualUv < 1.0f) &&
        VirtualShadowPageResidentCommon(level, localPage);
    if (!resident)
    {
        return float2(0.0f, 0.0f);
    }

    int2 globalPage = (int2)round(VirtualShadowPageOrigins[level].xy) + localPage;
    int2 physicalPage = int2(
        VirtualShadowPositiveModuloCommon(globalPage.x, pageGrid),
        VirtualShadowPositiveModuloCommon(globalPage.y, pageGrid));
    float2 inPageUv = frac(pagePosition);
    float halfLocalTexel = VirtualShadowParams[level].y * (float)pageGrid * 0.5f;
    inPageUv = clamp(
        inPageUv,
        float2(halfLocalTexel, halfLocalTexel),
        float2(1.0f - halfLocalTexel, 1.0f - halfLocalTexel));
    return ((float2)physicalPage + inPageUv) / (float)pageGrid;
}

float VirtualShadowLevelInteriorCommon(int level, float2 virtualUv, float3 worldPos)
{
    float pageGrid = max(VirtualShadowPageOrigins[level].z, 1.0f);
    float residentGrid = max(ShadowDebugGlobal.z, 1.0f);
    float firstResident = (pageGrid - residentGrid) * 0.5f;
    float2 pagePosition = virtualUv * pageGrid;
    float2 edgeDistance = min(
        pagePosition - firstResident,
        firstResident + residentGrid - pagePosition);
    // Clipmap coverage is a square in light space.  Basing the blend on
    // Euclidean camera distance creates a circular, moving LOD contour that
    // cuts through this square and makes the shadow silhouette "swim".
    // Blend only in the real overlap of two clipmap levels.  The coordinates
    // are snapped with the clipmap, so this weight is stable while the camera
    // moves inside a page.
    float overlapPages =
        max(residentGrid * 0.5f * clamp(VirtualShadowGlobal.w, 0.05f, 0.40f),
            0.25f);
    return smoothstep(
        0.0f,
        overlapPages,
        min(edgeDistance.x, edgeDistance.y));
}

float CascadedShadowLevelInteriorCommon(int level, float2 shadowUv, float3 worldPos)
{
    float2 edgeDistance = min(shadowUv, 1.0f - shadowUv);
    float overlapUv =
        max(0.5f * clamp(VirtualShadowGlobal.w, 0.05f, 0.40f),
            2.0f / 2048.0f);
    return smoothstep(0.0f, overlapUv, min(edgeDistance.x, edgeDistance.y));
}

float ShadowWorldFilterScaleCommon(int level)
{
    float finestWorldTexel = max(VirtualShadowPageOrigins[0].w, 0.000001f);
    float levelWorldTexel = max(VirtualShadowPageOrigins[level].w, finestWorldTexel);
    return clamp(finestWorldTexel / levelWorldTexel, 0.125f, 1.0f);
}

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
    // Equivalent to denom^-1.5 without the relatively expensive generic pow.
    float invSqrtDenom = rsqrt(denom);
    return 0.0795775f * (1.0f - g2) * invSqrtDenom * invSqrtDenom * invSqrtDenom;
}

float AtmosphereDensityCommon(float3 worldPos)
{
    float density = max(AtmosphereParams0.w, 0.0f);
    float heightFalloff = max(AtmosphereParams1.x, 0.0f);
    float heightDensity = exp(-max(worldPos.y, 0.0f) * heightFalloff);
    return density * heightDensity;
}

float3 ApplyAtmosphereToLightCommon(
    float3 worldPos,
    float3 lightDir,
    float3 lightColor,
    inout float volumeScatter)
{
    [branch]
    if (AtmosphereParams0.x < 0.5f)
    {
        return lightColor;
    }

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
    volumeScatter += dot((rayleigh + mie) * opticalDepth, float3(0.299f, 0.587f, 0.114f)) * shaftStrength;
    return lightColor * transmittance + inScatter;
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
    out float volumeScatter);

float3 AtmosphereSingleScatterFromDensityCommon(float sampleDensity, float3 viewToCamera, float3 lightDir, float3 lightColor)
{
    float3 toLight = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
    float cosTheta = clamp(dot(toLight, viewToCamera), -1.0f, 1.0f);
    float rayleighPhase = RayleighPhaseCommon(cosTheta);
    float miePhase = HenyeyGreensteinCommon(cosTheta, AtmosphereParams1.z);
    float3 rayleigh = AtmosphereColor0.rgb * max(AtmosphereParams0.y, 0.0f) * rayleighPhase;
    float3 mie = AtmosphereColor1.rgb * max(AtmosphereParams0.z, 0.0f) * miePhase;
    return (rayleigh + mie) * sampleDensity * lightColor;
}

float3 AtmosphereSingleScatterCommon(float3 samplePos, float3 viewToCamera, float3 lightDir, float3 lightColor)
{
    return AtmosphereSingleScatterFromDensityCommon(AtmosphereDensityCommon(samplePos), viewToCamera, lightDir, lightColor);
}

float SampleAtmosphereShadowMap(
    float3 worldPos,
    float3 lightDir,
    Texture2DArray<float> shadowMap,
    SamplerComparisonState shadowSampler,
    float4x4 lightViewProjection,
    float4 shadowMapParams,
    float shadowLayer)
{
    float3 l = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));

    float4 lightClip = mul(float4(worldPos, 1.0f), lightViewProjection);
    float safeW = max(lightClip.w, 0.000001f);
    float3 lightNdc = lightClip.xyz / safeW;
    float2 shadowUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);

    float inBounds =
        (lightClip.w > 0.0f &&
         shadowUv.x >= 0.0f && shadowUv.x <= 1.0f &&
         shadowUv.y >= 0.0f && shadowUv.y <= 1.0f &&
         lightNdc.z >= 0.0f && lightNdc.z <= 1.0f) ? 1.0f : 0.0f;

    float blur = saturate(AtmosphereCamera.w);
    float texelSize = shadowMapParams.x * (1.0f + blur * 4.0f);
    float bias = max(shadowMapParams.y, shadowMapParams.z);
    float currentDepth = lightNdc.z - bias;

    float visibility = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float2 offset = float2((float)x, (float)y) * texelSize;
            visibility += shadowMap.SampleCmpLevelZero(
                shadowSampler,
                float3(shadowUv + offset, shadowLayer),
                currentDepth);
        }
    }

    visibility /= 25.0f;
    float shadowStrength = saturate(abs(shadowMapParams.w));
    float outOfBoundsVisibility = 1.0f;
    return lerp(1.0f, lerp(outOfBoundsVisibility, visibility, inBounds), shadowStrength);
}


float SampleVirtualAtmosphereShadowMapCommon(
    float3 worldPos,
    Texture2DArray<float> shadowMap,
    SamplerComparisonState shadowSampler)
{
    int levelCount = clamp((int)round(VirtualShadowGlobal.y), 1, 4);
    float fineVisibility = 1.0f;
    float fineInterior = 1.0f;
    bool hasFine = false;
    [unroll]
    for (int level = 0; level < 4; ++level)
    {
        if (level >= levelCount) break;
        float4 clip = mul(float4(worldPos, 1.0f), VirtualShadowViewProjections[level]);
        if (clip.w <= 0.000001f) continue;
        float3 ndc = clip.xyz / clip.w;
        if (ndc.z < 0.0f || ndc.z > 1.0f) continue;
        float2 virtualUv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        bool resident;
        int2 localPage;
        float2 physicalUv = VirtualShadowMapUvCommon(level, virtualUv, resident, localPage);
        if (!resident) continue;

        float4 params = VirtualShadowParams[level];
        float currentDepth = ndc.z - max(params.z, params.w) * (1.0f + level * 0.45f);
        float levelVisibility = shadowMap.SampleCmpLevelZero(
            shadowSampler,
            float3(physicalUv, max(params.x, 0.0f)),
            currentDepth);
        float levelInterior = VirtualShadowLevelInteriorCommon(level, virtualUv, worldPos);
        if (!hasFine)
        {
            fineVisibility = levelVisibility;
            fineInterior = levelInterior;
            hasFine = true;
            if (fineInterior >= 0.999f || level == levelCount - 1)
            {
                return fineVisibility;
            }
            continue;
        }
        return lerp(levelVisibility, fineVisibility, fineInterior);
    }
    return hasFine ? fineVisibility : 1.0f;
}

float SampleCascadedAtmosphereShadowMapCommon(
    float3 worldPos,
    Texture2DArray<float> shadowMap,
    SamplerComparisonState shadowSampler)
{
    int levelCount = clamp((int)round(VirtualShadowGlobal.y), 1, 4);
    float fineVisibility = 1.0f;
    float fineInterior = 1.0f;
    bool hasFine = false;
    [unroll]
    for (int level = 0; level < 4; ++level)
    {
        if (level >= levelCount) break;
        float4 clip = mul(float4(worldPos, 1.0f), VirtualShadowViewProjections[level]);
        if (clip.w <= 0.000001f) continue;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        if (any(uv < 0.0f) || any(uv > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f) continue;

        float4 params = VirtualShadowParams[level];
        float levelVisibility = SampleAtmosphereShadowMap(
            worldPos,
            float3(0.0f, 1.0f, 0.0f),
            shadowMap,
            shadowSampler,
            VirtualShadowViewProjections[level],
            float4(params.y * ShadowWorldFilterScaleCommon(level), params.z, params.w, 1.0f),
            params.x);
        float levelInterior = CascadedShadowLevelInteriorCommon(level, uv, worldPos);
        if (!hasFine)
        {
            fineVisibility = levelVisibility;
            fineInterior = levelInterior;
            hasFine = true;
            if (fineInterior >= 0.999f || level == levelCount - 1)
            {
                return fineVisibility;
            }
            continue;
        }
        return lerp(levelVisibility, fineVisibility, fineInterior);
    }
    return hasFine ? fineVisibility : 1.0f;
}


float3 WorldToScreenUV(float3 worldPos);
float3 ReconstructPostProcessViewRayCommon(float2 uv);
float3 ReconstructPostProcessWorldPositionCommon(float2 uv, float depth);
bool ProjectWorldToScreenCommon(float3 worldPos, out float2 screenUv);
void ResolveSingleLightCommon(
    float3 worldPos,
    float4 lightDirectionData,
    float4 lightPositionTypeData,
    float4 lightExtraData,
    out float3 lightDir,
    out float attenuation,
    out float volumeScatter);

float ScreenSpaceShaftVisibilityCommon(
    float2 screenUv,
    float2 lightUv,
    Texture2D<float> depthTexture,
    SamplerState depthSampler)
{
    const int stepCount = 8;
    float occlusion = 0.0f;
    float weightSum = 0.0f;
    float blurScale = lerp(0.70f, 1.35f, saturate(AtmosphereCamera.w));
    float2 rayToLight = lightUv - screenUv;
    [unroll]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float t = ((float)stepIndex + 1.0f) / ((float)stepCount + 1.0f);
        float sampleT = saturate(t * blurScale);
        float sampleDepth = depthTexture.SampleLevel(depthSampler, screenUv + rayToLight * sampleT, 0);
        float skyVisibility = smoothstep(0.96f, 0.999f, sampleDepth);
        float weight = 1.0f - sampleT * 0.55f;
        occlusion += skyVisibility * weight;
        weightSum += weight;
    }

    return lerp(0.35f, 1.0f, occlusion / max(weightSum, 0.0001f));
}

float3 RayMarchAtmosphereViewCommon(
    float3 worldPos,
    Texture2DArray<float> shadowMap,
    SamplerComparisonState shadowSampler,
    float4x4 lightViewProjection,
    float4 shadowMapParams)
{
    float atmosphereActive =
        step(0.5f, AtmosphereParams0.x) *
        step(0.0001f, AtmosphereColor0.a) *
        step(0.0001f, AtmosphereParams0.w);
    float3 cameraPos = AtmosphereCamera.xyz;
    float3 viewDelta = worldPos - cameraPos;
    float rawViewDistance = length(viewDelta);
    float validDistance = step(0.0001f, rawViewDistance);
    float viewDistance = clamp(rawViewDistance, 0.0001f, 80.0f);

    int stepCount = 50;
    float3 viewDir = viewDelta / max(rawViewDistance, 0.0001f);
    float3 viewToCamera = -viewDir;
    float stepLength = viewDistance / (float)max(stepCount, 1);
    float scaledStep = stepLength * max(AtmosphereParams1.w, 0.0001f);
    float transmittance = 1.0f;
    float3 result = float3(0.0f, 0.0f, 0.0f);
    int count = min((int)round(LightCount.x), 5);

    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float t = ((float)stepIndex + 0.5f) / (float)stepCount;
        float3 samplePos = cameraPos + viewDir * viewDistance * t;
        float3 stepScatter = float3(0.0f, 0.0f, 0.0f);
        float sampleDensity = AtmosphereDensityCommon(samplePos);

        [loop]
        for (int lightIndex = 0; lightIndex < count; ++lightIndex)
        {
            float3 singleDir;
            float singleAttenuation;
            float singleVolume;
            ResolveSingleLightCommon(samplePos, LightDirections[lightIndex], LightPositionTypes[lightIndex], LightExtras[lightIndex], singleDir, singleAttenuation, singleVolume);
            float3 rawSingleColor = max(LightColors[lightIndex].rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColors[lightIndex].a, 0.0f);
            float3 singleColor = rawSingleColor * singleAttenuation;
            float shadowVisibility = 1.0f;
            if (LightShadowData[lightIndex].x >= -0.5f)
            {
                float4 perLightShadowParams = float4(
                    LightShadowData[lightIndex].y,
                    LightShadowData[lightIndex].z,
                    LightShadowData[lightIndex].w,
                    1.0f);
                shadowVisibility = SampleAtmosphereShadowMap(
                    samplePos,
                    singleDir,
                    shadowMap,
                    shadowSampler,
                    LightViewProjections[lightIndex],
                    perLightShadowParams,
                    LightShadowData[lightIndex].x);
            }
            float3 shadowedColor = singleColor * shadowVisibility;
            float localLight = step(0.5f, LightPositionTypes[lightIndex].w);
            float volumeVisibility = lerp(shadowVisibility, max(shadowVisibility, 0.45f), localLight);
            float3 volumeColor = rawSingleColor * sqrt(saturate(singleAttenuation)) * volumeVisibility;
            float volumeLight = step(2.5f, LightPositionTypes[lightIndex].w) * step(LightPositionTypes[lightIndex].w, 3.5f);
            float localVolumeStepBoost = lerp(1.0f, 0.35f / max(AtmosphereParams1.w, 0.0001f), localLight);
            float phase = saturate(HenyeyGreensteinCommon(clamp(dot(singleDir, viewToCamera), -1.0f, 1.0f), AtmosphereParams1.z) * 6.0f);
            float shaftBoost = lerp(0.55f, 1.75f, phase) * lerp(1.0f, 2.35f, volumeLight);
            float3 singleAtmosphere = AtmosphereSingleScatterFromDensityCommon(sampleDensity, viewToCamera, singleDir, shadowedColor);
            stepScatter += singleAtmosphere * lerp(1.0f, 0.45f, volumeLight);
            stepScatter += volumeColor * singleVolume * sampleDensity * shaftBoost * localVolumeStepBoost * lerp(0.22f, lerp(0.82f, 1.05f, volumeLight), localLight);
        }

        if (count <= 0)
        {
            float3 singleDir;
            float singleAttenuation;
            float singleVolume;
            ResolveSingleLightCommon(samplePos, LightDirection, LightPositionType, LightExtra, singleDir, singleAttenuation, singleVolume);
            float3 rawSingleColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f);
            float3 singleColor = rawSingleColor * singleAttenuation;
            float shadowVisibility = SampleAtmosphereShadowMap(samplePos, singleDir, shadowMap, shadowSampler, lightViewProjection, shadowMapParams, 0.0f);
            float3 shadowedColor = singleColor * shadowVisibility;
            float localLight = step(0.5f, LightPositionType.w);
            float volumeVisibility = lerp(shadowVisibility, max(shadowVisibility, 0.45f), localLight);
            float3 volumeColor = rawSingleColor * sqrt(saturate(singleAttenuation)) * volumeVisibility;
            float volumeLight = step(2.5f, LightPositionType.w) * step(LightPositionType.w, 3.5f);
            float localVolumeStepBoost = lerp(1.0f, 0.35f / max(AtmosphereParams1.w, 0.0001f), localLight);
            float phase = saturate(HenyeyGreensteinCommon(clamp(dot(singleDir, viewToCamera), -1.0f, 1.0f), AtmosphereParams1.z) * 6.0f);
            float shaftBoost = lerp(0.55f, 1.75f, phase) * lerp(1.0f, 2.35f, volumeLight);
            float3 singleAtmosphere = AtmosphereSingleScatterFromDensityCommon(sampleDensity, viewToCamera, singleDir, shadowedColor);
            stepScatter += singleAtmosphere * lerp(1.0f, 0.45f, volumeLight);
            stepScatter += volumeColor * singleVolume * sampleDensity * shaftBoost * localVolumeStepBoost * lerp(0.22f, lerp(0.82f, 1.05f, volumeLight), localLight);
        }

        result += stepScatter * transmittance * scaledStep;
        transmittance *= exp(-max(AtmosphereParams1.y, 0.0f) * sampleDensity * scaledStep);
        if (transmittance <= 0.01f)
        {
            break;
        }
    }

    return result * max(AtmosphereColor0.a, 0.0f) * atmosphereActive * validDistance;
}

float3 AtmosphereBackgroundCommon(float2 uv)
{
    float atmosphereEnabled = step(0.5f, AtmosphereParams0.x);
    float3 viewDir = SafeNormalizeCommon(float3((uv.x - 0.5f) * 1.45f, (0.5f - uv.y) * 0.9f + 0.25f, 1.0f), float3(0.0f, 0.2f, 1.0f));
    float3 lightDir = SafeNormalizeCommon(LightDirection.xyz, float3(0.0f, 1.0f, 0.0f));
    float3 lightColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f);

    int count = min((int)round(LightCount.x), MAX_SHADER_LIGHTS);
    if (count > 0)
    {
        lightDir = SafeNormalizeCommon(LightDirections[0].xyz, lightDir);
        lightColor = max(LightColors[0].rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColors[0].a, 0.0f);
    }

    float cosTheta = clamp(dot(lightDir, -viewDir), -1.0f, 1.0f);
    float rayleighPhase = RayleighPhaseCommon(cosTheta);
    float miePhase = HenyeyGreensteinCommon(cosTheta, AtmosphereParams1.z);
    float skyDensity = max(AtmosphereParams0.w, 0.0f) * lerp(1.15f, 0.35f, saturate(uv.y));
    float horizon = pow(saturate(1.0f - abs(uv.y - 0.55f) * 1.35f), 2.0f);
    float3 rayleigh = AtmosphereColor0.rgb * max(AtmosphereParams0.y, 0.0f) * rayleighPhase;
    float3 mie = AtmosphereColor1.rgb * max(AtmosphereParams0.z, 0.0f) * miePhase * (0.35f + horizon * 1.25f);
    return (rayleigh + mie) * lightColor * skyDensity * max(AtmosphereColor0.a, 0.0f) * atmosphereEnabled;
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

    if (lightType == 0)
    {
        float density = max(lightExtraData.z, 0.0f);
        volumeScatter = density * 0.025f;
    }

    if (lightType == 1 || lightType == 2 || lightType == 3)
    {
        float3 toLight = lightPositionTypeData.xyz - worldPos;
        float distanceToLight = length(toLight);
        lightDir = SafeNormalizeCommon(toLight, lightDir);
        float rangeFade = saturate(1.0f - distanceToLight / lightRange);
        attenuation = rangeFade * rangeFade;
        float3 fromLight = SafeNormalizeCommon(worldPos - lightPositionTypeData.xyz, float3(0.0f, -1.0f, 0.0f));
        float3 spotForward = SafeNormalizeCommon(lightDirectionData.xyz, fromLight);
        float density = max(lightExtraData.z, 0.0f);

        if (lightType == 1)
        {
            float radialGlow = pow(rangeFade, 2.2f);
            volumeScatter = radialGlow * density * 0.16f;
        }

        if (lightType == 2 || lightType == 3)
        {
            float3 fromLightOffset = worldPos - lightPositionTypeData.xyz;
            float axialDistance = max(dot(fromLightOffset, spotForward), 0.0f);
            float radialDistance = length(fromLightOffset - spotForward * axialDistance);
            float outerCos = clamp(lightExtraData.y, 0.001f, 0.999f);
            float outerSin = sqrt(saturate(1.0f - outerCos * outerCos));
            float outerTan = outerSin / outerCos;
            float coneRadius = max(axialDistance * outerTan, 0.001f);
            float cylinderRadius = max(lightRange * outerTan * 0.35f, 0.001f);
            float spotCos = dot(fromLight, spotForward);
            float spotMask = smoothstep(lightExtraData.y, lightExtraData.x, spotCos);
            attenuation *= spotMask;

            float coneRadialAlpha = saturate(radialDistance / coneRadius);
            float axialAlpha = saturate(axialDistance / lightRange);
            float coneBodyMask = exp(-coneRadialAlpha * coneRadialAlpha * 2.0f)
                               * exp(-axialAlpha * 2.5f)
                               * spotMask
                               * step(0.0f, axialDistance);

            if (lightType == 2)
            {
                volumeScatter = coneBodyMask * density * 0.32f;
            }
        }

        if (lightType == 3)
        {
            float volumeShape = saturate(lightExtraData.w);
            float3 fromLightOffset = worldPos - lightPositionTypeData.xyz;
            float axialDistance = dot(fromLightOffset, spotForward);
            float radialDistance = length(fromLightOffset - spotForward * axialDistance);
            float outerCos = clamp(lightExtraData.y, 0.001f, 0.999f);
            float outerSin = sqrt(saturate(1.0f - outerCos * outerCos));
            float outerTan = outerSin / outerCos;
            float coneEndRadius = max(lightRange * outerTan, 0.001f);
            float coneRootRadius = max(coneEndRadius * 0.04f, 0.05f);
            float coneRadiusSlope =
                max(coneEndRadius - coneRootRadius, 0.0f) /
                lightRange;
            float coneRadius = max(
                coneRootRadius + axialDistance * coneRadiusSlope,
                coneRootRadius);
            float cylinderRadius = max(coneEndRadius * 0.45f, 0.001f);

            // A volume light is a finite, forward-facing cone/cylinder.  The
            // old abs(axialDistance) made a second lobe behind the emitter and
            // saturating radial alpha left exp(-1.6) outside the authored
            // boundary.  Both errors showed up as a rounded extra layer near
            // the shaft root.
            float insideAxial =
                step(0.0f, axialDistance) *
                step(axialDistance, lightRange);
            float axialAlpha = saturate(axialDistance / lightRange);
            float coneRadialAlpha = radialDistance / coneRadius;
            float cylinderRadialAlpha = radialDistance / cylinderRadius;

            float coneEdge =
                1.0f - smoothstep(0.78f, 1.0f, coneRadialAlpha);
            float cylinderEdge =
                1.0f - smoothstep(0.78f, 1.0f, cylinderRadialAlpha);
            float endFade =
                1.0f - smoothstep(0.72f, 1.0f, axialAlpha);

            float coneMask =
                exp(-coneRadialAlpha * coneRadialAlpha * 1.35f) *
                coneEdge *
                endFade *
                insideAxial;
            float cylinderMask =
                exp(-cylinderRadialAlpha * cylinderRadialAlpha * 1.7f) *
                cylinderEdge *
                endFade *
                insideAxial;

            float shapeMask = lerp(coneMask, cylinderMask, volumeShape);
            // Volume lights use the same finite shape for both direct
            // attenuation and participating-media density. Reusing the old
            // apex-based spotMask here zeroed the new emitter cap and left a
            // camera-dependent dark gap below the transform position.
            attenuation =
                rangeFade *
                rangeFade *
                shapeMask;
            volumeScatter = shapeMask * density * 0.55f;
        }
    }
}

void ResolveLightCommon(float3 worldPos, out float3 lightDir, out float attenuation, out float volumeScatter)
{
    ResolveSingleLightCommon(worldPos, LightDirection, LightPositionType, LightExtra, lightDir, attenuation, volumeScatter);
}

void ResolveLightAggregate(
    float3 worldPos,
    float2 pixelPosition,
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

    bool useLightGrid = HasLightTileGridCommon();
    uint tileBase = useLightGrid ? LightTileBaseCommon(pixelPosition) : 0u;
    int iterationCount = useLightGrid
        ? min(MAX_LIGHTS_PER_TILE, max((int)round(LightCount.w), 1))
        : count;
    [loop]
    for (int iteration = 0; iteration < MAX_SHADER_LIGHTS; ++iteration)
    {
        if (iteration >= iterationCount)
        {
            break;
        }

        uint lightIndex = useLightGrid
            ? LightTileIndices[tileBase + (uint)iteration]
            : (uint)iteration;
        if (lightIndex == 0xffffffffu || lightIndex >= (uint)count ||
            LightFlags[lightIndex].y < 0.5f || LightFlags[lightIndex].w >= 0.5f)
        {
            continue;
        }

        int i = (int)lightIndex;

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

    if (count <= 0)
    {
        float legacyVolume = 0.0f;
        ResolveLightCommon(worldPos, lightDir, attenuation, legacyVolume);
        lightColor = max(LightColor.rgb, float3(0.0f, 0.0f, 0.0f)) * max(LightColor.a, 0.0f) * attenuation;
        lightColor = ApplyAtmosphereToLightCommon(worldPos, lightDir, lightColor, legacyVolume);
        volumeScatter = legacyVolume * max(LightColor.a, 0.0f);
        rangeBlend = saturate((max(LightDirection.w, 1.0f) - 1.0f) / 7.0f);
        return;
    }

    if (weightSum <= 0.000001f)
    {
        lightColor = float3(0.0f, 0.0f, 0.0f);
        attenuation = 0.0f;
        volumeScatter = 0.0f;
        rangeBlend = 0.0f;
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
    float4 RimStyle;    // x: softness, y: power, z: albedo blend, w: light blend
    float4 RimLight;    // rgb: rim color, a: unused
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
    float RimSoftness;
    float RimPower;
    float3 RimColor;
    float RimAlbedoBlend;
    float RimLightBlend;
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
    float4 Transparent0; // x: IOR, y: transmission, z: transmission roughness, w: refraction strength
    float4 Transparent1; // x: thickness, yzw: absorption coefficient
    MaterialPartShaderParams PartParams[15];
};

MaterialPartShaderParams ResolveMaterialPartParams(float materialPartId)
{
    int index = clamp((int)round(materialPartId), 0, 14);
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
    float4x4 PPInvViewProjection;
    float4x4 PPViewProjection;
};

float3 ReconstructPostProcessViewRayCommon(float2 uv)
{
    float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 worldFar = mul(float4(ndc, 1.0f, 1.0f), PPInvViewProjection);
    worldFar.xyz /= max(abs(worldFar.w), 0.000001f);
    return SafeNormalizeCommon(worldFar.xyz - PPCameraPos.xyz, float3(0.0f, 0.0f, 1.0f));
}

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

float3 WorldToScreenUV(float3 worldPos)
{
    float4 clipPos = mul(float4(worldPos, 1.0f), PPViewProjection);
    clipPos.xyz /= max(abs(clipPos.w), 0.000001f);
    float2 screenUV = float2(clipPos.x * 0.5f + 0.5f, -clipPos.y * 0.5f + 0.5f);
    return float3(screenUV, clipPos.z);
}

bool ProjectWorldToScreenCommon(float3 worldPos, out float2 screenUv)
{
    float4 clipPos = mul(float4(worldPos, 1.0f), PPViewProjection);
    if (clipPos.w <= 0.000001f)
    {
        screenUv = float2(0.0f, 0.0f);
        return false;
    }

    float3 ndc = clipPos.xyz / clipPos.w;
    screenUv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
    return ndc.z >= 0.0f && ndc.z <= 1.0f;
}

// Returns hit UV in xy and the world-space distance along the reflection ray in z.
// A negative x/y means no reliable hit. D3D depth is already in [0, 1], so it
// must not be remapped to the OpenGL [-1, 1] range here.
float3 ScreenSpaceRayMarch(float3 origin, float3 direction, Texture2D<float> depthTex, SamplerState depthSampler)
{
    const float maxDist = 15.0f;
    const int maxSteps = 64;
    const int refineSteps = 6;
    const float stepSize = maxDist / (float)maxSteps;

    float previousT = 0.0f;
    float previousDepthDiff = -1.0f;
    bool hasFrontSample = false;

    [loop]
    for (int i = 1; i <= maxSteps; ++i)
    {
        float currentT = stepSize * (float)i;
        float3 rayPos = origin + direction * currentT;
        float4 clipPos = mul(float4(rayPos, 1.0f), PPViewProjection);
        if (clipPos.w <= 0.000001f)
            break;

        float3 rayNdc = clipPos.xyz / clipPos.w;
        float3 rayUVZ = float3(
            rayNdc.x * 0.5f + 0.5f,
            -rayNdc.y * 0.5f + 0.5f,
            rayNdc.z);

        if (rayUVZ.x < 0.0f || rayUVZ.x > 1.0f || rayUVZ.y < 0.0f || rayUVZ.y > 1.0f)
            break;

        if (rayUVZ.z < 0.0f || rayUVZ.z > 1.0f)
            break;

        float sceneDepth = depthTex.SampleLevel(depthSampler, rayUVZ.xy, 0).r;
        if (sceneDepth >= 1.0f - 0.001f)
        {
            previousT = currentT;
            continue;
        }

        float depthDiff = rayUVZ.z - sceneDepth;
        if (depthDiff <= 0.0f)
        {
            hasFrontSample = true;
            previousT = currentT;
            previousDepthDiff = depthDiff;
            continue;
        }

        // Only accept an actual front-to-back crossing. This prevents the ray
        // from immediately selecting its own surface or a foreground silhouette.
        if (hasFrontSample && previousDepthDiff <= 0.0f)
        {
            float lowT = previousT;
            float highT = currentT;

            [unroll]
            for (int refine = 0; refine < refineSteps; ++refine)
            {
                float midT = (lowT + highT) * 0.5f;
                float3 midPos = origin + direction * midT;
                float4 midClip = mul(float4(midPos, 1.0f), PPViewProjection);
                float3 midNdc = midClip.xyz / max(midClip.w, 0.000001f);
                float2 midUv = float2(midNdc.x * 0.5f + 0.5f, -midNdc.y * 0.5f + 0.5f);
                float midSceneDepth = depthTex.SampleLevel(depthSampler, midUv, 0).r;

                if (midNdc.z > midSceneDepth)
                    highT = midT;
                else
                    lowT = midT;
            }

            float hitT = (lowT + highT) * 0.5f;
            float3 hitPos = origin + direction * hitT;
            float4 hitClip = mul(float4(hitPos, 1.0f), PPViewProjection);
            float3 hitNdc = hitClip.xyz / max(hitClip.w, 0.000001f);
            float2 hitUv = float2(hitNdc.x * 0.5f + 0.5f, -hitNdc.y * 0.5f + 0.5f);
            return float3(hitUv, hitT);
        }

        previousT = currentT;
        previousDepthDiff = depthDiff;
    }

    return float3(-1.0f, -1.0f, -1.0f);
}

float3 ReconstructPostProcessWorldPositionCommon(float2 uv, float depth)
{
    float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 world = mul(float4(ndc, depth, 1.0f), PPInvViewProjection);
    return world.xyz / max(abs(world.w), 0.000001f);
}
 
#endif
