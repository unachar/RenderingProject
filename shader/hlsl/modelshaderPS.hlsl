#define SHADER_3D
#define SHADER_FORWARD_LIGHTING
#define SHADER_MATERIAL_OPTICS_PREVIEW
#define POM_PIXEL_DEPTH_OUTPUT
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
    float2 uv = float2(frac(atan2(dir.z, dir.x) / (2.0f * PI) + 0.5f), acos(clamp(dir.y, -1.0f, 1.0f)) / PI);
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
// MegaLights M7：Forward / 半透明もクラスターライトグリッドで評価（ルート引数 34：t44〜t46 +
// b10、35：ライトのテクスチャ）
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
#include "PhysicalGlass.hlsli"

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
    ResolveSurfaceLightAggregateShadowed(
        worldPos, ddx(worldPos), ddy(worldPos), normal, WorldToScreenUV(worldPos).xy * ViewportSize,
        clamp((int)round(VirtualShadowGlobal.z), 0, 3), false, normal, 0.0f, 0.0f, 0.0f, directLight,
        aggregateDirection, aggregateColor, attenuation, volume, rangeBlend, visibility);
    return visibility;
}

// ---------------------------------------------------------------------------------------------
// 屈折（RenderQuality: RaytracedRefraction）。屈折レイをシーンの
// TLAS に対して追跡し（インライン RayQuery）、透明物体の奥の面を求める。その色は、
// 投影したヒット位置の不透明シーンのコピー（t3）から取得する。DXR がない場合、ヒット距離を
// 不透明深度バッファーから推定する。
// ---------------------------------------------------------------------------------------------
float3 ReconstructForwardWorld(float2 uv, float depth)
{
    float4 world = mul(float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f), PPInvViewProjection);
    float safeW = world.w;
    if (!(abs(world.w) > 1e-6f))
    {
        safeW = 1e-6f;
        if (world.w < 0)
        {
            safeW = -1e-6f;
        }
    }
    return world.xyz / safeW;
}

// 深度で検証する画面空間のフォールバック。不透明シーンのコピーは、
// 隠れた面、画面外の面、追加の透明レイヤーを表現できない。
float3 SampleGlassBackground(float2 uv, float roughness)
{
    uint sceneWidth, sceneHeight;
    SceneColorTexture.GetDimensions(sceneWidth, sceneHeight);
    float2 texel = 1.0f / float2(max(sceneWidth, 1u), max(sceneHeight, 1u));
    float2 radius = texel * (saturate(roughness) * saturate(roughness) * 24.0f);
    float2 lo = texel * 0.5f;
    float2 hi = 1.0f - lo;
    float3 c = SceneColorTexture.SampleLevel(g_SamplerState, clamp(uv, lo, hi), 0).rgb * 0.5f;
    c += SceneColorTexture.SampleLevel(g_SamplerState, clamp(uv + float2(radius.x, 0), lo, hi), 0).rgb * 0.125f;
    c += SceneColorTexture.SampleLevel(g_SamplerState, clamp(uv - float2(radius.x, 0), lo, hi), 0).rgb * 0.125f;
    c += SceneColorTexture.SampleLevel(g_SamplerState, clamp(uv + float2(0, radius.y), lo, hi), 0).rgb * 0.125f;
    c += SceneColorTexture.SampleLevel(g_SamplerState, clamp(uv - float2(0, radius.y), lo, hi), 0).rgb * 0.125f;
    return c;
}

// シーンに描画済みの可視の空や背景は、IBL の照明項ではない。
// このレイが背景深度へ投影される場合だけ再利用する。
// 画面外でヒットしなかった場合は黒を保ち、環境マップで代用しない。
float3 SampleGlassVisibleBackground(float3 origin, float3 direction, float roughness)
{
    float2 uv;
    if (!ProjectWorldToScreenCommon(origin + direction * 2000.0f, uv) || any(uv <= 0) || any(uv >= 1))
    {
        return 0.0f;
    }
    uint width, height, depthWidth, depthHeight;
    SceneColorTexture.GetDimensions(width, height);
    DepthTexture.GetDimensions(depthWidth, depthHeight);
    float2 extent = float2(max(width, 1u), max(height, 1u));
    float2 pixel = uv * extent - 0.5f;
    int2 base = (int2)floor(pixel);
    float2 fraction = frac(pixel);
    float3 sum = 0;
    float weightSum = 0;
    [unroll] for (uint y = 0; y < 2; ++y)
    {
        [unroll] for (uint x = 0; x < 2; ++x)
        {
            int2 p = clamp(base + int2(x, y), int2(0, 0), int2(width, height) - 1);
            float2 tapUv = (float2(p) + 0.5f) / extent;
            int2 dp =
                clamp((int2)(tapUv * float2(depthWidth, depthHeight)), int2(0, 0), int2(depthWidth, depthHeight) - 1);
            float depth = DepthTexture.Load(int3(dp, 0));
            float weight = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
            if (depth <= 0.0f)
            {
                sum += SceneColorTexture.Load(int3(p, 0)).rgb * weight;
                weightSum += weight;
            }
        }
    }
    return weightSum > 1e-5f ? sum / weightSum : 0.0f;
}

float3 ComputeRefraction(SurfaceData surface, float3 position, float3 normal, float3 viewDir, float2 screenUv,
                         out float pathLength)
{
    float3 N = dot(normal, viewDir) >= 0.0f ? normal : -normal;
    float eta = 1.0f / max(surface.IOR, 1.0f);
    float3 incident = -viewDir;
    float3 entering = refract(incident, N, eta);
    if (dot(entering, entering) < 1e-8f)
    {
        pathLength = 0;
        return 0.0f;
    }
    entering = normalize(entering);
    float thickness = max(surface.Thickness, 0.0f);
    pathLength = thickness / max(abs(dot(entering, N)), 1e-4f);
    // 2 番目の界面は最初の界面と平行。両方の界面で屈折すると、
    // 横方向の変位を残して入射方向へ戻る。
    float3 exitPosition = position + entering * pathLength;
    float3 exitDirection = refract(entering, N, max(surface.IOR, 1.0f));
    exitDirection = SafeNormalizeCommon(exitDirection, incident);
    float roughness = saturate(Transparent0.z);
#ifdef SHADER_RAYTRACING
    if (QualityFlags.w > 0.5f && SceneRayFlags.z > 0.5f)
    {
        RayDesc ray;
        ray.Origin = exitPosition + exitDirection * 0.001f;
        ray.Direction = exitDirection;
        ray.TMin = 0.001f;
        ray.TMax = 200.0f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x03, ray);
        while (query.Proceed())
        {
        }
        if (query.CommittedStatus() != COMMITTED_NOTHING)
        {
            float3 hit = ray.Origin + exitDirection * query.CommittedRayT();
            float2 uv;
            if (ProjectWorldToScreenCommon(hit, uv) && all(uv > 0) && all(uv < 1))
            {
                float d = DepthTexture.SampleLevel(g_SamplerState, uv, 0);
                float3 visible = ReconstructForwardWorld(uv, d);
                if (d > 0.0f && length(visible - hit) < max(0.05f, length(hit - SurfaceCameraOriginCommon(hit, CameraPos)) * 0.01f))
                {
                    return SampleGlassBackground(uv, roughness);
                }
            }
        }
    }
#endif
    float previous = 0.0f;
    [loop] for (uint step = 0; step < 32; ++step)
    {
        float distance = 0.025f * (exp2((step + 1) * 0.40f) - 1.0f);
        float3 p = exitPosition + exitDirection * distance;
        float2 uv;
        if (!ProjectWorldToScreenCommon(p, uv) || any(uv <= 0) || any(uv >= 1))
        {
            break;
        }
        float depth = DepthTexture.SampleLevel(g_SamplerState, uv, 0);
        if (depth > 0.0f)
        {
            float3 opaque = ReconstructForwardWorld(uv, depth);
            float delta = length(p - SurfaceCameraOriginCommon(p, CameraPos)) - length(opaque - SurfaceCameraOriginCommon(opaque, CameraPos));
            if (delta >= 0.0f)
            {
                float low = previous;
                float high = distance;
                [unroll] for (uint j = 0; j < 5; ++j)
                {
                    float mid = (low + high) * 0.5f;
                    float3 q = exitPosition + exitDirection * mid;
                    float2 quv;
                    if (!ProjectWorldToScreenCommon(q, quv) || any(quv <= 0) || any(quv >= 1))
                    {
                        high = mid;
                        continue;
                    }
                    float qd = DepthTexture.SampleLevel(g_SamplerState, quv, 0);
                    float3 qw = ReconstructForwardWorld(quv, qd);
                    if (qd > 0.0f && length(q - SurfaceCameraOriginCommon(q, CameraPos)) >= length(qw - SurfaceCameraOriginCommon(qw, CameraPos)))
                    {
                        high = mid;
                    }
                    else
                    {
                        low = mid;
                    }
                }
                float3 hit = exitPosition + exitDirection * high;
                float2 huv;
                if (ProjectWorldToScreenCommon(hit, huv) && all(huv > 0) && all(huv < 1))
                {
                    float hd = DepthTexture.SampleLevel(g_SamplerState, huv, 0);
                    float3 hw = ReconstructForwardWorld(huv, hd);
                    if (hd > 0.0f && length(hw - hit) < max(0.1f, length(hit - SurfaceCameraOriginCommon(hit, CameraPos)) * 0.02f))
                    {
                        return SampleGlassBackground(huv, roughness);
                    }
                }
                break;
            }
        }
        previous = distance;
    }
    return SampleGlassVisibleBackground(exitPosition, exitDirection, roughness);
}

#include "GeometryGlass.hlsli"
#include "MaterialOpticsPreview.hlsli"
#include "NormalMapDecode.hlsli"

static float g_PomForwardDepth = 0.0f;

float4 ShadeForward(in PSInput3D In)
{
    float3 glassIncident = -SurfaceViewDirectionCommon(In.WorldPos, CameraPos);
    float3 glassRayDx = ddx(glassIncident);
    float3 glassRayDy = ddy(glassIncident);
    float3 glassOrigin = SurfaceCameraOriginCommon(In.WorldPos, CameraPos);
    float3 glassOriginDx = ddx(glassOrigin);
    float3 glassOriginDy = ddy(glassOrigin);
    const float PI = 3.14159265358979323846f;

    AssetMaterialParameters assetMaterial = LoadAssetMaterial(0);
    AssetParallaxResult pom =
        ApplyAssetParallaxEx(assetMaterial, In.TexCoord, In.WorldPos, In.Normal, In.CameraPos, g_SamplerState);
    float2 materialUv = pom.Uv;
    // ピクセル深度オフセットと POM セルフシャドウ（主光源）。
    g_PomForwardDepth = AssetParallaxPixelDepth(pom, In.WorldPos, In.CameraPos, saturate(In.Position.z));
#ifdef IMPOSTOR_FORWARD
    g_PomForwardDepth = g_ImpostorDepth; // インポスター：アトラスから復元した表面の深度
#endif
#ifdef POM_HAS_MAIN_LIGHT
    if (pom.Active)
    {
        g_PomSelfShadowLight = PomMainLightDirection(In.WorldPos);
        g_PomSelfShadow = AssetParallaxSelfShadow(pom, g_PomSelfShadowLight, g_SamplerState);
    }
#endif
    float materialPartId = (MaterialMode == 1) ? (float)ShaderClass : max(In.Diffuse.a, 0.0f);
    // 手動モードへ切り替えた後も、モデル色の alpha にマテリアルの分類が残る場合がある。
    float4 baseColor = float4(In.Diffuse.rgb, 1.0f);
    if (assetMaterial.Surface.z > 0.5f)
    {
        baseColor = float4(1, 1, 1, 1);
    }
    if (UseTexture != 0)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, materialUv);
    }
    float assetMetallic = mMetallic;
    float assetRoughness = mRoughness;
    float4 assetEmissionAO;
    ApplyAssetMaterial(assetMaterial, materialUv, g_SamplerState, baseColor, assetMetallic, assetRoughness,
                       assetEmissionAO);
#ifdef IMPOSTOR_FORWARD
    baseColor = float4(g_ImpostorAlbedo, 1.0f); // インポスター：被覆率は呼び出し側で切り抜き済み
#endif
    baseColor.a = saturate(baseColor.a * MaterialAlpha);
    clip(baseColor.a - (1.0f / 255.0f));

    float3 surfaceNormal = In.Normal * ((FlipNormal != 0) ? -1.0f : 1.0f);
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = DecodeTangentNormalSample(g_NormalTexture.Sample(g_SamplerState, materialUv));
        surfaceNormal = ApplyNormalMapCommon(surfaceNormal, In.WorldPos, In.TexCoord,
                                             ScaleAssetNormal(tangentNormal, assetMaterial));
    }
    else
    {
        // 法線マップが無いときは POM の高さマップから法線を作る。
        surfaceNormal = ApplyAssetHeightNormal(assetMaterial, surfaceNormal, In.WorldPos, In.TexCoord, materialUv,
                                               g_SamplerState);
    }
    surfaceNormal = SafeNormalizeCommon(surfaceNormal, float3(0.0f, 1.0f, 0.0f));
#ifdef IMPOSTOR_FORWARD
    surfaceNormal = g_ImpostorNormal;
#endif

    MaterialPartShaderParams partParams = ResolveMaterialPartParams(materialPartId);
    bool usePartParams = MaterialMode != 1;
    float4 material =
        float4(usePartParams ? partParams.Basic.x : assetMetallic, usePartParams ? partParams.Basic.y : assetRoughness,
               usePartParams ? partParams.Basic.z : mFresnel, materialPartId);
    float4 shadowParams = usePartParams ? float4(partParams.Shadow0.x, partParams.Shadow0.y, partParams.Shadow0.z, 0.0f)
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
    ResolveLightAggregate(In.WorldPos, In.Position.xy, lightDir, lightColor, lightAttenuation, volumeScatter,
                          rangeBlend);
    float lightIntensity = LightColor.a;

    float metallicParam = material.r;
    float roughnessParam = material.g;
    float f0 = material.b;
    float3 NdotL = saturate(dot(surfaceNormal, lightDir));
    float3 environmentColor = 0.0f;
    if (Transparent0.y <= 0.0f)
    {
        environmentColor = EnvironmentTexture.SampleLevel(g_SamplerState, materialUv, roughnessParam * 10.0f).rgb;
    }
    NdotL += lightIntensity;

    float3 diffuse = 0.0f;
    {
        float3 light = lightColor.rgb * saturate(dot(lightDir, surfaceNormal));
        float2 iblTexcoord;
        iblTexcoord.x = -atan2(surfaceNormal.x, surfaceNormal.z) / (PI * 2);
        iblTexcoord.y = asin(surfaceNormal.y) / PI;
        if (Transparent0.y <= 0.0f)
        {
            light += EnvironmentTexture.SampleLevel(g_SamplerState, iblTexcoord, roughnessParam * 10.0f).rgb * 6;
        }
        diffuse = light * baseColor.rgb / PI;
    }

    // 分類ごとにライトループ全体をインライン展開せず、従来の可視性クエリーを共有する。
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
        float selfShadowMask =
            smoothstep(shadowThreshold - shadowSoftness, shadowThreshold + shadowSoftness, castShadow);
        shadowDensity =
            saturate(max(shadowDensity, selfShadowMask * shadowStrength * lerp(0.20f, 0.85f, lightShadowPower)));
        float3 lightTint = (lightLuminance > 0.0001f) ? lightColor.rgb / lightLuminance : float3(1.0f, 1.0f, 1.0f);
        float3 shadowTint = saturate(float3(0.018f, 0.016f, 0.020f) + environmentColor + lightTint * 0.025f);
        float3 litColor = baseColor.rgb * (lightColor.rgb * 0.72f + NdotL);
        baseColor.rgb = diffuse * lerp(litColor, litColor * shadowTint, shadowDensity);
    }

    if (pbr || transparent || btdf || bsdf)
    {
        SurfaceData surface = BuildLightingSurface(baseColor, surfaceNormal, material.rgb, assetEmissionAO);
        surface.Transmission = saturate(Transparent0.y);
        surface.IOR = max(Transparent0.x, 1.0f);
        surface.Thickness = max(Transparent1.x, 0.0f);
        surface.AbsorptionColor = max(Transparent1.yzw, 0.0f);
        if (surface.Transmission > 0.0f)
        {
            float ratio = (surface.IOR - 1.0f) / (surface.IOR + 1.0f);
            surface.F0 = ratio * ratio;
            material.b = surface.F0;
        }
        float3 viewDir = SurfaceViewDirectionCommon(In.WorldPos, CameraPos);
        if (SceneRayParameters.w > 0.5f && (surface.Transmission > 0.0f || g_ShadingModel.Model == 0u))
        {
            float3 previewColor = EvaluateMaterialOpticsPreview(surface, In.WorldPos, viewDir);
            return float4(previewColor * surface.Opacity, surface.Opacity);
        }
        float3 directLight;
        float visibility;
        int filterRadius =
            clamp((int)round(lerp(max(VirtualShadowGlobal.z, 1.0f), 3.0f, saturate(shadowSoftness * 2.0f))), 1, 3);
        g_MLCellIndex = MLCellIndexFromWorld(In.WorldPos);
        ResolveSurfaceLightAggregateShadowed(In.WorldPos, ddx(In.WorldPos), ddy(In.WorldPos), surfaceNormal,
                                             In.Position.xy, filterRadius, true, viewDir, surface.BaseColor,
                                             material.rgb, shadowSoftness, directLight, lightDir, lightColor,
                                             lightAttenuation, volumeScatter, rangeBlend, visibility);
        uint width;
        uint height;
        SceneColorTexture.GetDimensions(width, height);
        float2 screenUv = In.Position.xy / float2(max(width, 1u), max(height, 1u));
        // ガラスは直接光だけを受ける。基本ローブに環境照明、
        // 画面空間 GI、反射プローブの寄与を加えない。
        const bool glassMaterial = surface.Transmission > 0.0f;
        float3 color;
        if (glassMaterial)
        {
            color = directLight + surface.Emissive;
        }
        else
        {
            color = EvaluateSurfaceEnvironment(surface, viewDir, In.WorldPos, screenUv, shadowSoftness, directLight);
            color = ApplySurfaceScreenEffects(color - surface.Emissive, surface.Emissive, screenUv,
                                              surface.BaseColor * (1 - surface.Metallic));
        }
        float3 premultiplied = color * surface.Opacity;
        float coverage = surface.Opacity;
        // 透過率は BSDF ローブの重みであり、(1-alpha) ではない。alpha は
        // 幾何的な被覆率なので、全面を覆うガラスでも alpha=1 を使える。
        float glassWeight = surface.Transmission * (1.0f - saturate(surface.Metallic));
        if (glassWeight > 0.001f)
        {
#ifdef SHADER_RAYTRACING
            float3 geometryRadiance;
            bool ownsPixel;
            if (TraceGeometryGlass(In.WorldPos, viewDir, glassRayDx, glassRayDy, glassOriginDx, glassOriginDy,
                                   geometryRadiance, ownsPixel))
            {
                // 最も手前のラスターレイヤーだけが、追跡した経路全体を担当する。
                if (!ownsPixel)
                {
                    clip(-1.0f);
                }
                return float4(geometryRadiance * surface.Opacity, surface.Opacity);
            }
#endif
            float3 N = dot(surfaceNormal, viewDir) >= 0 ? surfaceNormal : -surfaceNormal;
            float pathLength;
            float3 background = ComputeRefraction(surface, In.WorldPos, N, viewDir, screenUv, pathLength);
            float F = GlassFresnel(dot(N, viewDir), 1.0f, surface.IOR);
            float3 beer = GlassBeer(surface.BaseColor, surface.AbsorptionColor, surface.Thickness, pathLength);
            float3 R, T;
            GlassSlabWeights(F, beer, R, T);
            // フレネル項によって、透過光から反射分のエネルギーを引き続き除く。
            // ガラスの環境反射は意図的に無効にしている。
            // 既存のクラスター化された直接光の鏡面反射経路を、
            // 拡散アルベド 0 と IOR から導く誘電体の F0 で評価する。
            float f0 = pow((surface.IOR - 1.0f) / (surface.IOR + 1.0f), 2.0f);
            float3 specularDirect, glassLightDir, glassLightColor;
            float glassAttenuation, glassVolume, glassRange, glassVisibility;
            ResolveSurfaceLightAggregateShadowed(In.WorldPos, ddx(In.WorldPos), ddy(In.WorldPos), N, In.Position.xy,
                                                 filterRadius, true, viewDir, 0.0f, float3(0.0f, surface.Roughness, f0),
                                                 shadowSoftness, specularDirect, glassLightDir, glassLightColor,
                                                 glassAttenuation, glassVolume, glassRange, glassVisibility);
            float3 glassColor = specularDirect + T * background + surface.Emissive;
            premultiplied = lerp(color, glassColor, glassWeight) * surface.Opacity;
        }
        return float4(premultiplied, saturate(coverage));
    }

    if (brdf)
    {
        float3 N = normalize(surfaceNormal + 0.00001f);
        float3 L = normalize(lightDir + 0.00001f);
        float3 V = SurfaceViewDirectionCommon(In.WorldPos, CameraPos);
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
        float3 directReflection =
            (diffuseBRDF + specularBRDF) * lightColor.rgb * lightIntensity * NdotLScalar * shadowVisibility;
        float3 envReflection = SampleEnvironmentLatLong(normalize(reflect(-V, N)), roughness) * F;
        baseColor.rgb = directReflection + envReflection;
    }

    baseColor.rgb = baseColor.rgb * assetEmissionAO.a + assetEmissionAO.rgb;
    baseColor.rgb *= baseColor.a;
    return baseColor;
}

// ---------------------------------------------------------------------------------------------
// 出力：SV_Target0 は乗算済みの色（通常の全パス）。
// OIT の透明パスでは、PSO がターゲット 0 をマスクし、SV_Target1/2 を
// 重み付きブレンド OIT の蓄積先と透過残量のターゲットへ合成する（McGuire & Bavoil 2013）。
// レンダーターゲットが 1 つのパスでは SV_Target1/2 を無視する。
// ---------------------------------------------------------------------------------------------
struct ForwardOutput
{
    float4 Color : SV_Target0;
    float4 Accumulation : SV_Target1;
    float4 Revealage : SV_Target2;
    float HardwareDepth : SV_DepthLessEqual; // POM のピクセル深度オフセット（常に補間深度以下）
};

#ifndef IMPOSTOR_FORWARD
ForwardOutput main(in PSInput3D In)
{
    float4 premultiplied = ShadeForward(In);
    ForwardOutput output;
    output.Color = premultiplied;
    float alpha = saturate(premultiplied.a);
    float viewDistance = length(In.WorldPos - SurfaceCameraOriginCommon(In.WorldPos, CameraPos));
    // 式 (7)：距離に基づく重み。0.1 .. 500 m で安定する。
    float weight = alpha * clamp(0.03f / (0.00001f + pow(viewDistance / 200.0f, 4.0f)), 0.01f, 3000.0f);
    output.Accumulation = float4(premultiplied.rgb, alpha) * weight;
    output.Revealage = alpha.xxxx;
    output.HardwareDepth = min(g_PomForwardDepth, In.Position.z);
    return output;
}
#endif
