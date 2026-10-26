Texture2D<float4> MaterialEmissionAO : register(t33);
#define SHADER_POSTPROCESS
#define SHADER_3D
#include "common.hlsl"
// 大気 LUT の物理的な空（ルート引数 26、t40/t41、クランプサンプラー s4 にバインド）。
#define SKY_LUT 1
#define SKY_LUT_SCATTER_REGISTER t40
#define SKY_LUT_TRANSMISSION_REGISTER t41
#define SKY_LUT_SAMPLER_REGISTER s4
#include "SkyAtmosphere.hlsli"
#include "CloudMedium.hlsli"
#pragma warning(disable : 4000)

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

// MegaLights（ルート引数 28〜34）
//   M1: t44 ライト / t45 セル / t46 セル内ライト / b10 定数
//   M2: t47 サンプル / u1 累積（検証）/ t48 リファレンス（検証）
//   M3: u2 ML6 の生の結果 / t49 デノイズ済み（ルート引数 35, 36）
#define MEGALIGHTS_GRID 1
#define MEGALIGHTS_CB_REGISTER b10
#include "MegaLightsCommon.hlsli"
StructuredBuffer<MegaLightData> MegaLightBuffer : register(t44);
StructuredBuffer<uint2> MegaLightCells : register(t45);
StructuredBuffer<uint> MegaLightIndices : register(t46);
StructuredBuffer<uint4> MegaLightSamples : register(t47);
RWStructuredBuffer<float4> MegaLightAccumulation : register(u1);
StructuredBuffer<float4> MegaLightReference : register(t48);
//   M3: u2 ML6 の結果（デモジュレート済み・MegaLightsShadePS が書く）/ t49 デノイズ済み
RWStructuredBuffer<float4> MegaLightRaw : register(u2);
StructuredBuffer<float4> MegaLightDenoised : register(t49);
//   M4: t50 可視ライトハッシュ（このフレームに作ったもの。デバッグ表示用。ルート引数 37）
StructuredBuffer<uint4> MegaLightHash : register(t50);
static uint g_MLCellIndex = ML_INVALID_CELL;

// リファレンス：ライト全体の平均可視率を、面上の層別点への影レイで求める（毎フレーム回転 → 累積で収束）
float MLReferenceVisibility(MegaLightData light, float3 worldPos, float3 normal, uint2 pixel, uint lightOrdinal)
{
    if (!MLLightCastsShadow(light))
    {
        return 1.0f;
    }
#ifdef SHADER_RAYTRACING
    const bool area = (int)round(light.PositionType.w) == 4 || light.Flags.y > 1.0e-4f;
    const uint requested = area ? clamp((uint)MLStochasticParams.w, 1u, 64u) : 1u;
    const uint side = max((uint)floor(sqrt((float)requested) + 0.001f), 1u);
    const uint seed = MLSeed(pixel, MLStochastic.z, 0x5245u + lightOrdinal * 7919u);
    const float2 jitter = float2(MLRandom(seed), MLRandom(seed ^ 0x9e3779b9u));
    float visible = 0.0f;
    [loop] for (uint index = 0u; index < side * side; ++index)
    {
        const float2 uv = (float2(index % side, index / side) + jitter) / (float)side;
        visible += TraceSceneSegment(worldPos, normal, MLLightSamplePoint(light, worldPos, uv));
    }
    return visible / (float)(side * side);
#else
    return -1.0f;
#endif
}

// M7：テクスチャ付き矩形ライト（ルート引数 38：エンジンのテクスチャ SRV 512 個, t0 space5）
#define MEGALIGHTS_LIGHT_TEXTURES 1

// DFG / LTC テーブル（ルート引数 27：t42、t43）。
#define SURFACE_PBR_LUTS 1
#include "SurfaceLighting.hlsli"
#include "WeatherSurface.hlsli"

float4 ShadeDeferred(PSInputPostProcess input)
{

    const float PI = 3.14159265358979323846f;
    // ライティングは GBuffer の解像度で行う。整数座標の読み取りでマテリアルの
    // 分類を保ち、ピクセルごとの属性のフィルタリングを省く。
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

    float4 atmosphereMedia = AtmosphereTexture.SampleLevel(TextureSampler, input.TexCoord, 0);
    float fogTransmittance = saturate(atmosphereMedia.a);
    if (background)
    {
        if (SkyParameters.x > 0.5f)
        {
            // 空の積分値には大気の消散と散乱がすでに含まれる。
            return float4(EvaluateSkyAtmosphere(ReconstructPostProcessViewRayCommon(input.TexCoord)), 1.0f);
        }
        baseColor.rgb =
            (baseColor.rgb + AtmosphereBackgroundCommon(input.TexCoord)) * fogTransmittance + atmosphereMedia.rgb;
        return baseColor;
    }

    bool toon = IsMaterialClass(shaderClass, 4.0f);
    bool shadow = IsMaterialClass(shaderClass, 5.0f);
    bool lit = IsMaterialClass(shaderClass, 8.0f);
    bool pbr = IsMaterialClass(shaderClass, 11.0f);
    const int shadowDebugMode = (int)round(ShadowDebugGlobal.x);
    const bool needsDeferredLighting = toon || shadow || lit || pbr || shadowDebugMode > 0;
    [branch] if (!needsDeferredLighting)
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
        // 拡張マテリアルモデル（影の GBuffer の alpha）と、PBR の面に適用する天候。
        g_ShadingModel = DecodeShadingModel(shadowParams.a);
        // POM セルフシャドウ（MaterialSurface.hlsli: WriteAssetParallaxSelfShadow）。r = 1 が印、b = 可視率。
        if (shadowParams.r > 0.999f)
        {
            g_PomSelfShadow = saturate(shadowParams.b);
            g_PomSelfShadowLight = LightPositionType.w < 0.5f
                                       ? SafeNormalizeCommon(LightDirection.xyz, float3(0.0f, 1.0f, 0.0f))
                                       : SafeNormalizeCommon(LightPositionType.xyz - position, float3(0.0f, 1.0f, 0.0f));
        }
        ApplyWeatherToSurface(position, surfaceNormal, baseColor.rgb, material.rgb);
    }
    float3 viewDir = CameraViewDirection(position, PPCameraPos.xyz, PPInvViewProjection);
    float3 pbrDirectLight;
    int materialShadowFilterRadius =
        pbr ? clamp((int)round(lerp(max(VirtualShadowGlobal.z, 1.0f), 3.0f, saturate(shadowSoftness * 2.0f))), 1, 3)
            : clamp((int)round(VirtualShadowGlobal.z), 0, 3);
    ResolveSurfaceLightAggregateShadowed(position, positionDx, positionDy, surfaceNormal, input.Position.xy,
                                         materialShadowFilterRadius, pbr, viewDir, baseColor.rgb, material.rgb,
                                         shadowSoftness, pbrDirectLight, lightDir, lightColor, lightAttenuation,
                                         volumeScatter, rangeBlend, aggregateShadowVisibility);

    if (shadowDebugMode == 1 && VirtualShadowGlobal.x > 0.5f)
    {

        return float4(aggregateShadowVisibility.xxx, 1.0f);
    }
    if (shadowDebugMode > 1 && VirtualShadowGlobal.x > 0.5f && VirtualShadowGlobal.x < 1.5f)
    {
        int selectedLevel = max((int)round(VirtualShadowGlobal.y) - 1, 0);
        float2 selectedUv = float2(0.5f, 0.5f);
        [unroll] for (int level = 0; level < 4; ++level)
        {
            if (level >= (int)round(VirtualShadowGlobal.y))
            {
                break;
            }
            float4 clip = mul(float4(position.xyz, 1.0f), VirtualShadowViewProjections[level]);
            float3 ndc = clip.xyz / max(clip.w, 0.000001f);
            float2 uv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
            selectedUv = uv;
            float pageGrid = max(ShadowDebugGlobal.w, 1.0f);
            float residentGrid = max(ShadowDebugGlobal.z, 1.0f);
            float firstResident = (pageGrid - residentGrid) * 0.5f;
            float2 pageCoord = floor(saturate(uv) * pageGrid);
            if (all(uv >= 0.0f) && all(uv < 1.0f) && all(pageCoord >= firstResident) &&
                all(pageCoord < firstResident + residentGrid))
            {
                selectedLevel = level;
                break;
            }
        }
        if (shadowDebugMode == 2)
        {
            const float3 levelColors[4] = {float3(0.10f, 0.85f, 0.30f), float3(0.15f, 0.45f, 1.00f),
                                           float3(1.00f, 0.70f, 0.10f), float3(0.95f, 0.18f, 0.25f)};
            return float4(levelColors[selectedLevel], 1.0f);
        }
        float2 page = floor(saturate(selectedUv) * max(ShadowDebugGlobal.w, 1.0f));
        float pageHash = frac(dot(page, float2(0.06711056f, 0.00583715f)));
        float3 pageColor = frac(pageHash + float3(0.0f, 0.333f, 0.667f));
        return float4(pageColor, 1.0f);
    }
    [branch] if (shadow || toon || lit)
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
            light += EnvironmentTexture.SampleLevel(TextureSampler, iblTexcoord, Roughness * 10.0f).rgb * 6;
            diffuse = light * baseColor.rgb / PI;
        }

        if (shadow)
        {
            float castShadow = saturate(1.0f - aggregateShadowVisibility);
            float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
            float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
            float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
            float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
            float3 shadowTint =
                saturate(float3(0.015f, 0.014f, 0.017f) + environmentColor * 0.16f + lightTint * 0.025f);
            float3 litColor = baseColor.rgb * (0.12f + NdotL * (0.22f + lightColor.rgb * 0.72f));
            baseColor.rgb = lerp(litColor, baseColor.rgb * shadowTint - 0.08f, shadowDensity);
        }

        if (toon)
        {
            float castShadow = saturate(1.0f - aggregateShadowVisibility);
            float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
            float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
            float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
            float selfShadowMask =
                smoothstep(shadowThreshold - shadowSoftness, shadowThreshold + shadowSoftness, castShadow);
            shadowDensity =
                saturate(max(shadowDensity, selfShadowMask * shadowStrength * lerp(0.20f, 0.85f, lightShadowPower)));
            float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
            float3 shadowTint = saturate(float3(0.018f, 0.016f, 0.020f) + environmentColor + lightTint * 0.025f);
            float3 litColor = baseColor.rgb * (lightColor.rgb * 0.72f + NdotL);
            baseColor.rgb = diffuse * lerp(litColor, litColor * shadowTint, shadowDensity);
        }

        if (lit)
        {
            float castShadow = saturate(1.0f - aggregateShadowVisibility);
            float lightLuminance = dot(lightColor.rgb, float3(0.299f, 0.587f, 0.114f));
            float lightShadowPower = saturate(lightLuminance / (lightLuminance + 1.0f));
            float shadowDensity = castShadow * shadowStrength * lerp(0.25f, 1.0f, lightShadowPower);
            float selfShadowMask =
                smoothstep(shadowThreshold - shadowSoftness, shadowThreshold + shadowSoftness, castShadow);
            shadowDensity =
                saturate(max(shadowDensity, selfShadowMask * shadowStrength * lerp(0.20f, 0.85f, lightShadowPower)));
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
        // 発光は、この後の画面空間エフェクトを適用した後で加える。
        surface.Emissive = 0.0f;
        baseColor.rgb =
            EvaluateSurfaceEnvironment(surface, viewDir, position, input.TexCoord, shadowSoftness, pbrDirectLight);
    }

    baseColor.rgb = ApplySurfaceScreenEffects(baseColor.rgb, authoredEmissionAO.rgb, input.TexCoord, giReflectance);
    baseColor.a = 1.0f;
    return baseColor;
}

// ---------------------------------------------------------------------------------------------
// MegaLights：セルの決定 → ライティング → 検証用の累積 → デバッグ表示
// ---------------------------------------------------------------------------------------------
float MLDebugLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float4 main(PSInputPostProcess input) : SV_Target
{
    const uint2 pixel = uint2(input.Position.xy);
    // グリッド構築（MegaLightsGridCS）・M2 のサンプリングと同じ深度・同じ式でセルを決める
    g_MLCellIndex = MLCellIndexFromDepth(pixel, DepthTexture.Load(int3(pixel, 0)));
    float4 color = ShadeDeferred(input);
    [branch] if (!MLGridActive() || any(pixel >= MLScreen.xy))
    {
        return color;
    }

    // 検証：静止時の累積平均（前フレームまでの平均 + このフレーム）
    const uint linearIndex = pixel.y * MLScreen.x + pixel.x;
    float4 mean = color;
    [branch] if ((MLStochastic.x & ML_FLAG_ACCUMULATE) != 0u)
    {
        const float frames = (float)MLStochastic.w;
        const float4 previous = MegaLightAccumulation[linearIndex];
        mean = frames < 0.5f ? color : previous + (color - previous) / (frames + 1.0f);
        MegaLightAccumulation[linearIndex] = mean;
    }

    const uint debugView = MLCounts.y;
    [branch] if (debugView == 0u)
    {
        return color;
    }
    float3 debugColor = color.rgb;
    bool overlay = true;
    if (debugView == 1u || debugView == 2u)
    {
        if (g_MLCellIndex == ML_INVALID_CELL)
        {
            return color;
        }
        if (debugView == 1u)
        {
            const uint cellLights = MegaLightCells[g_MLCellIndex].y;
            debugColor =
                cellLights == 0u ? float3(0.02f, 0.02f, 0.04f) : MLHeatmap((float)cellLights / max(MLDepth2.z, 1.0f));
        }
        else
        {
            const uint slice = g_MLCellIndex / (MLGrid.x * MLGrid.y);
            debugColor = frac((float)slice * float3(0.61803f, 0.41421f, 0.27183f) + float3(0.1f, 0.4f, 0.7f));
        }
        const uint2 tilePixel = pixel % ML_TILE_SIZE;
        const float gridLine = (tilePixel.x == 0u || tilePixel.y == 0u) ? 0.35f : 0.0f;
        return float4(lerp(color.rgb, debugColor, 0.75f) * (1.0f - gridLine), color.a);
    }
    if (debugView >= 3u && debugView <= 5u)
    {
        // M2 のサンプル：3 = 選ばれたライト, 4 = 可視率, 5 = 1/p
        if (!MLStochasticActive())
        {
            return color;
        }
        const uint sampleBase = MLSampleBase(MLScreenToSample(pixel));
        uint valid = 0u;
        uint traced = 0u;
        uint visible = 0u;
        uint firstLight = 0xffffffffu;
        float maxInvPdf = 0.0f;
        for (uint k = 0u; k < MLStochastic.y; ++k)
        {
            const uint4 lightSample = MegaLightSamples[sampleBase + k];
            if ((lightSample.x & ML_SAMPLE_VALID) == 0u)
            {
                continue;
            }
            ++valid;
            if (firstLight == 0xffffffffu)
            {
                firstLight = lightSample.x & ML_SAMPLE_INDEX;
            }
            if ((lightSample.x & ML_SAMPLE_TRACE) != 0u)
            {
                ++traced;
                visible += (lightSample.x & ML_SAMPLE_VISIBLE) != 0u ? 1u : 0u;
            }
            maxInvPdf = max(maxInvPdf, asfloat(lightSample.y));
        }
        if (valid == 0u)
        {
            return color;
        }
        if (debugView == 3u)
        {
            debugColor = frac((float)MLHash(firstLight) * (1.0f / 4294967296.0f) * float3(1.0f, 7.0f, 31.0f) +
                              float3(0.1f, 0.35f, 0.6f));
        }
        else if (debugView == 4u)
        {
            debugColor = traced == 0u ? float3(0.2f, 0.4f, 1.0f) : ((float)visible / (float)traced).xxx;
        }
        else
        {
            debugColor = MLHeatmap(log2(max(maxInvPdf, 1.0f)) / log2(max(MLStochasticParams.z, 2.0f)));
        }
    }
    else if (debugView == 9u)
    {
        // M5：影レイの処理の内訳（緑 = スクリーントレースで遮蔽を確定, 橙 = ワールドトレース, 青 = 影レイ不要）
        if (!MLStochasticActive())
        {
            return color;
        }
        const uint sampleBase = MLSampleBase(MLScreenToSample(pixel));
        float3 mix = 0.0f;
        uint valid = 0u;
        for (uint k = 0u; k < MLStochastic.y; ++k)
        {
            const uint flags = MegaLightSamples[sampleBase + k].x;
            if ((flags & ML_SAMPLE_VALID) == 0u)
            {
                continue;
            }
            ++valid;
            if ((flags & ML_SAMPLE_SCREEN) != 0u)
            {
                mix += float3(0.15f, 0.95f, 0.3f);
            }
            else if ((flags & ML_SAMPLE_TRACE) != 0u)
            {
                mix += float3(1.0f, 0.55f, 0.1f);
            }
            else
            {
                mix += float3(0.2f, 0.4f, 1.0f);
            }
        }
        if (valid == 0u)
        {
            return color;
        }
        debugColor = mix / (float)valid;
    }
    else if (debugView == 8u)
    {
        // M4：可視ライトハッシュの埋まり具合（128bit 中の立っているビット数。8x8 タイル）
        if (!MLStochasticActive())
        {
            return color;
        }
        const uint occupancy = MLHashOccupancy(MegaLightHash[MLHashTileIndex(pixel)]);
        debugColor = occupancy == 0u ? float3(0.02f, 0.02f, 0.04f) : MLHeatmap((float)occupancy / 64.0f);
        const uint2 tilePixel = pixel % ML_HASH_TILE;
        if (tilePixel.x == 0u || tilePixel.y == 0u)
        {
            debugColor *= 0.6f;
        }
    }
    else if (debugView == 6u)
    {
        // 累積平均そのもの
        if ((MLStochastic.x & ML_FLAG_ACCUMULATE) == 0u)
        {
            return color;
        }
        return float4(mean.rgb, color.a);
    }
    else if (debugView == 7u)
    {
        // リファレンスとの差（輝度の相対誤差 × 10 をヒートマップ）
        if ((MLStochastic.x & ML_FLAG_HAS_REF) == 0u)
        {
            return color;
        }
        const float reference = MLDebugLuminance(MegaLightReference[linearIndex].rgb);
        const float difference = abs(MLDebugLuminance(mean.rgb) - reference);
        debugColor = MLHeatmap(10.0f * difference / max(reference, 0.05f));
        overlay = false;
    }
    else
    {
        return color;
    }
    return float4(overlay ? lerp(color.rgb, debugColor, 0.8f) : debugColor, color.a);
}

// ---------------------------------------------------------------------------------------------
// MegaLights M3：ML6（サンプルのシェーディング）だけを行い、デモジュレートして MegaLightRaw に書く
//   ディファードライティングの直前に、同じルートシグネチャ・同じ定数で描画する（カラー書き込みなし）。
//   材質の準備（シェーディングモデル・天候）は ShadeDeferred と同じ順序で行うこと。
//   a = 1: デノイズ対象のピクセル（PBR 面でセルが有効）, a = 0: 対象外
// ---------------------------------------------------------------------------------------------
float4 MegaLightsShadePS(PSInputPostProcess input) : SV_Target
{
    const uint2 pixel = uint2(input.Position.xy);
    if (!MLStochasticActive() || any(pixel >= MLScreen.xy))
    {
        return 0.0f;
    }
    const uint linearIndex = pixel.y * MLScreen.x + pixel.x;
    const int3 gbufferPixel = int3(pixel, 0);
    float4 material = MaterialTexture.Load(gbufferPixel);
    const float depth = DepthTexture.Load(gbufferPixel);
    g_MLCellIndex = MLCellIndexFromDepth(pixel, depth);
    float4 result = 0.0f;
    [branch] if (g_MLCellIndex != ML_INVALID_CELL && IsMaterialClass(material.a, 11.0f))
    {
        float4 baseColor = BaseColorTexture.Load(gbufferPixel);
        const float3 position = ReconstructPostProcessWorldPositionCommon(input.TexCoord, depth);
        float3 surfaceNormal = DecodeGBufferNormal(NormalTexture.Load(gbufferPixel).xyz);
        const float4 shadowParams = ShadowGBufferTexture.Load(gbufferPixel);
        g_ShadingModel = DecodeShadingModel(shadowParams.a);
        ApplyWeatherToSurface(position, surfaceNormal, baseColor.rgb, material.rgb);
        const float3 viewDir = CameraViewDirection(position, PPCameraPos.xyz, PPInvViewProjection);
        SurfaceLightAccumulator acc = (SurfaceLightAccumulator)0;
        MLAccumulateStochasticSamples(position, 0.0f, 0.0f, surfaceNormal, input.Position.xy, 1, viewDir, baseColor.rgb,
                                      material.rgb, max(shadowParams.g, 0.0001f), float3(1.0f, 1.0f, 1.0f), acc);
        const float3 lighting = acc.PbrDirect / MLRemodulation(surfaceNormal, viewDir, baseColor.rgb, material.rgb);
        // a = 1 + ヒット距離（最も近いサンプルのライトまで）。M3 は a >= 0.5 を対象の印に、M6 の NRD は距離に使う
        float hitDistance = 1.0e4f;
        const uint sampleBase = MLSampleBase(MLScreenToSample(pixel));
        for (uint k = 0u; k < MLStochastic.y; ++k)
        {
            const uint4 lightSample = MegaLightSamples[sampleBase + k];
            if ((lightSample.x & ML_SAMPLE_VALID) == 0u)
            {
                continue;
            }
            hitDistance =
                min(hitDistance, length(MegaLightBuffer[lightSample.x & ML_SAMPLE_INDEX].PositionType.xyz - position));
        }
        result = float4(all(isfinite(lighting)) ? lighting : 0.0f, 1.0f + hitDistance);
    }
    MegaLightRaw[linearIndex] = result;
    return 0.0f;
}

