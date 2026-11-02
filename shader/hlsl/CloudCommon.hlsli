#ifndef CLOUD_COMMON
#define CLOUD_COMMON
#define SHADER_POSTPROCESS
#include "common.hlsl"
#include "CelestialLighting.hlsli"

Texture2D<float> CloudSceneDepth : register(t30);
Texture2D<float4> CloudScatteringTransmittance : register(t33);
Texture2D<float2> CloudDepth : register(t34);
Texture2D<float4> CloudSceneMaterial : register(t35);
#include "CloudMedium.hlsli"

float SceneRayDistance(float2 uv)
{
    uint2 pixel = min(uint2(uv * CloudExtent.xy), uint2(CloudExtent.xy) - 1);
    // DeferredLightingPS と合わせ、空と環境のメッシュは遮蔽物ではなく背景として扱う。
    if (CloudSceneMaterial.Load(int3(pixel, 0)).a < -0.5)
    {
        return CloudTrace.x;
    }
    float depth = CloudSceneDepth.Load(int3(pixel, 0));
    if (depth <= 0.0f)
    {
        return CloudTrace.x;
    }
    float3 origin = CameraRayOrigin(uv, PPCameraPos.xyz, PPInvViewProjection);
    return min(length(ReconstructPostProcessWorldPositionCommon(uv, depth) - origin), CloudTrace.x);
}

struct CloudOutput
{
    float4 Color : SV_Target0; // RGB = 散乱、A = 透過率。不透明度ではない。
    float2 Depth : SV_Target1; // シーンのレイ距離、不透明度で重み付けした雲のレイ距離。
};

CloudOutput MarchCloud(float2 uv, float2 pixel)
{
    CloudOutput output;
    output.Color = float4(0, 0, 0, 1);
    if (CloudQuality.z > 0)
    {
        output.Color = float4(0, 0, 0, 0);
    }
    if (CloudQuality.z == 2 || CloudQuality.z == 4)
    {
        output.Color.rgb = 1;
    }
    float sceneDistance = SceneRayDistance(uv);
    output.Depth = float2(sceneDistance, CloudTrace.x);
    if (CloudQuality.z >= 6)
    {
        float3 noiseUv = float3(uv, CloudTrace.y);
        float noise = CloudQuality.z == 6 ? CloudBaseNoise.SampleLevel(CloudWrap, noiseUv, 0)
                                          : CloudDetailNoise.SampleLevel(CloudWrap, noiseUv, 0);
        output.Color = float4(noise.xxx, 0);
        return output;
    }
    float3 ray = ReconstructPostProcessViewRayCommon(uv);
    float3 origin = CameraRayOrigin(uv, PPCameraPos.xyz, PPInvViewProjection);
    float2 intervals[4];
    uint intervalCount = CloudIntervals(origin, ray, sceneDistance, intervals);
    if (intervalCount == 0)
    {
        return output;
    }
    float2 heightBounds = CloudHeightBounds();
    float3 sunDirection = SafeNormalizeCommon(SkySunDirection.xyz, float3(0, 1, 0));
    float3 sunRadiance = 0;
    int sunIndex = CloudSunIndex();
    if (sunIndex >= 0)
    {
        sunRadiance = max(LightColors[sunIndex].rgb * LightColors[sunIndex].w, 0);
    }
    float3 moonDirection = VirtualMoonDirection();
    float3 moonRadiance = VirtualMoonIrradiance() * CelestialAtmosphereTransmission(heightBounds.x, moonDirection);
    const float sunCosine = dot(ray, sunDirection);
    const float moonCosine = dot(ray, moonDirection);
    float3 solarBottom = sunRadiance * CelestialAtmosphereTransmission(heightBounds.x, sunDirection);
    float3 solarTop = sunRadiance * CelestialAtmosphereTransmission(heightBounds.y, sunDirection);
    const bool sunLit = any(solarTop > 0) && sunDirection.y > -0.02;
    const bool moonLit = any(moonRadiance > 0) && moonDirection.y > 0;
    float twilight = smoothstep(-0.12, 0.04, sunDirection.y) * (1 - smoothstep(0.04, 0.30, sunDirection.y));
    float3 ambientColor = lerp(float3(0.006, 0.009, 0.016),
                               max(AtmosphereColor0.rgb, 0) * max(SkyParameters.z, 0) * 0.05, SkyParameters.y);
    ambientColor = lerp(ambientColor, float3(0.12, 0.035, 0.018) * max(SkyParameters.z, 0) * 0.05, twilight);
    float3 ambient = ambientColor * CloudLighting.y;
    // 光マーチの長さ：雲の厚みに比例（数 km の全区間ではなく、自己遮蔽に効く範囲）
    const float layerThickness = max(heightBounds.y - heightBounds.x, 1.0);
    const float lightMarchLength = clamp(layerThickness * 0.6, 50.0, 3000.0);
    // ピクセルごとに安定した IGN。時間方向の再投影がないため、フレームごとに乱数を変えない。
    float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float offset = lerp(0.5, jitter, CloudLighting.w);
    float transmittance = 1;
    float3 scattering = 0;
    float3 debugLighting = 0;
    float densityIntegral = 0;
    float depthWeight = 0;
    float weightedDepth = 0;
    uint steps = 0;
    float occupiedDistance = 0;
    const float extinction = max(CloudShape.w, 0.000001);
    [loop] for (uint interval = 0; interval < intervalCount; ++interval)
    {
        float intervalStart = intervals[interval].x;
        float intervalLength = intervals[interval].y - intervalStart;
        occupiedDistance += intervalLength;
        // 最適化：空白は 4 倍の粗いステップ（基本形状のみ）で進み、雲に当たったら 1 ステップ戻って細かく進む。
        // 細かいステップは従来と同じ長さ（区間 / ステップ数）なので、密な部分の品質は落ちない。
        const float fineStep = intervalLength / max((float)CloudQuality.x, 1.0);
        const float coarseStep = fineStep * 4.0;
        float distance = intervalStart + offset * fineStep;
        bool fine = false;
        uint emptyRun = 0;
        const uint maxIterations = CloudQuality.x * 3u + 8u;
        [loop] for (uint iteration = 0; iteration < maxIterations && distance < intervals[interval].y; ++iteration)
        {
            float3 position = origin + ray * distance;
            if (!fine)
            {
                // 粗い探索
                ++steps;
                if (CloudDensityLod(position, false) < 0.00001 && CloudQuality.z != 3)
                {
                    distance += coarseStep;
                    continue;
                }
                // 雲に入った：1 粗ステップ分戻って細かいステップへ
                fine = true;
                emptyRun = 0;
                distance = max(intervalStart + offset * fineStep, distance - coarseStep + fineStep);
                continue;
            }
            ++steps;
            float density = CloudDensity(position);
            if (density < 0.00001 && CloudQuality.z != 3)
            {
                // 連続して空なら粗い探索へ戻る
                if (++emptyRun >= 6)
                {
                    fine = false;
                }
                distance += fineStep;
                continue;
            }
            emptyRun = 0;
            float heightFraction = saturate((position.y - heightBounds.x) / layerThickness);
            float3 solarIncident = lerp(solarBottom, solarTop, heightFraction);

            // ---- 光の透過（指数間隔の光マーチ）
            float sunDepth = sunLit ? CloudMarchOpticalDepth(position, sunDirection, lightMarchLength) : 0;
            float moonDepth = moonLit ? CloudMarchOpticalDepth(position, moonDirection, lightMarchLength) : 0;

            // ---- 多重散乱の近似（Wrenninge 2013 のオクターブ法）
            //  オクターブごとに消散 a^n、寄与 b^n、位相の異方性 c^n を下げる。
            //  厚い雲の内部が真っ黒にならず、明るい縁と柔らかな陰影になる。
            float3 sunLight = 0;
            float3 moonLight = 0;
            float contribution = 1;
            float attenuation = 1;
            float anisotropy = 1;
            [unroll] for (uint octave = 0; octave < 3; ++octave)
            {
                float g = CloudLighting.x * anisotropy;
                sunLight += contribution * exp(-sunDepth * extinction * attenuation) * CloudDualPhase(sunCosine, g);
                moonLight += contribution * exp(-moonDepth * extinction * attenuation) * CloudDualPhase(moonCosine, g);
                contribution *= 0.5;
                attenuation *= 0.4;
                anisotropy *= 0.5;
            }
            // 3 オクターブの合計（1 + 0.5 + 0.25）で正規化し、薄い雲の明るさは従来と揃える
            sunLight *= 1.0 / 1.75;
            moonLight *= 1.0 / 1.75;
            float powder = 1 + CloudLighting.z * (1 - exp(-2 * density)); // 従来と同じパウダー効果
            // 環境光：雲の上部ほど空に開けていて明るい
            float3 ambientSample = ambient * lerp(0.45, 1.25, heightFraction);
            float3 lighting = (solarIncident * sunLight * (sunLit ? 1 : 0) + moonRadiance * moonLight * (moonLit ? 1 : 0)) *
                                  powder +
                              ambientSample;

            // ---- ステップ内の解析積分（散乱アルベド 1）：ステップ長が変わっても明るさが変わらない
            float sampleT = exp(-density * extinction * fineStep);
            float weight = transmittance * (1 - sampleT);
            scattering += weight * lighting;
            debugLighting += lighting;
            densityIntegral += density * fineStep;
            weightedDepth += distance * weight;
            depthWeight += weight;
            transmittance *= sampleT;
            distance += fineStep;
            if (transmittance < 0.01)
            {
                break;
            }
        }
        if (transmittance < 0.01)
        {
            break;
        }
    }
    output.Color = float4(scattering, transmittance);
    output.Depth.y = depthWeight > 0.000001 ? weightedDepth / depthWeight : CloudTrace.x;
    if (CloudQuality.z == 1)
    {
        output.Color = float4(saturate(densityIntegral / max(occupiedDistance, 0.0001)).xxx, 0);
    }
    if (CloudQuality.z == 2)
    {
        output.Color = float4(transmittance.xxx, 0);
    }
    if (CloudQuality.z == 3)
    {
        output.Color = float4(debugLighting / max((float)steps, 1), 0);
    }
    if (CloudQuality.z == 4)
    {
        output.Color = float4((output.Depth.y / CloudTrace.x).xxx, 0);
    }
    if (CloudQuality.z == 5)
    {
        output.Color = float4(saturate(float(steps) / (CloudQuality.x * intervalCount)).xxx, 0);
    }
    return output;
}
#endif
