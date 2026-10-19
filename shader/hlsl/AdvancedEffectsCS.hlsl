// =============================================================================================
// 高度な画面効果（AdvancedEffectsCS.hlsl）
// フロクセルのボリュームフォグ（注入・積分・合成）、ヒストグラムによる自動露出、
// モーションブラーと被写界深度。ブルームとトーンマッピングの前に線形 HDR シーンへ適用する。
// advancedeffects.h が実行時にコンパイルする（cs_6_5、DXC）。
// =============================================================================================

cbuffer AdvancedConstants : register(b0)
{
    float4x4 InvViewProjection;  // 現在フレーム、ジッター適用済み（GBuffer と一致）
    float4x4 PrevViewProjection; // 前フレーム、フロクセルの再投影用
    float4 CameraPosition;       // xyz、w = フレーム時間（秒）
    float4 PrevCameraPosition;   // xyz、w = 履歴の有効フラグ（0/1）
    uint4 Extent;                // シーンの幅、高さ、フレーム番号、ライト数
    uint4 FroxelExtent;          // フロクセルの x、y、z、ローカルフォグ数
    float4 FogDepth;             // 近距離、遠距離、log2(far / near)、履歴の重み
    float4 FogMedium0;           // 全体の密度、高度による減衰、消散、距離スケール
    float4 FogMedium1;           // レイリー散乱の強度、ミー散乱の強度、ミー散乱の g、散乱強度
    float4 FogRayleighColor;     // rgb、w = 環境光の強度
    float4 FogMieColor;          // rgb、w = TLAS による影が利用可能なら 1
    float4 FogAmbientColor;      // rgb = 空の環境光（多重散乱の近似）、w = 露出のリセット
    float4 ExposureParams;       // 輝度の最小 log2 値、log2 範囲、露出補正 EV、順応速度
    float4 ExposureLimits;       // 最小露出、最大露出、下位パーセンタイル、上位パーセンタイル
    float4 MotionBlurParams;     // シャッタースケール、最大長（px）、サンプル数、有効フラグ
    float4 DofParams;            // 焦点距離、合焦範囲、最大半径（px）、有効フラグ
    uint4 Features;              // フォグ、自動露出、モーションブラー、被写界深度
    float4 LocalFogData0[16];    // 位置、半径
    float4 LocalFogData1[16];    // 高度による減衰、密度、形状（1 = 球）、有効フラグ
    float4 LocalFogColor[16];
    uint4 MegaLightsFog; // x: MegaLights のグリッドを使う, y: フロクセルあたりのサンプル数
};

struct FogLight
{
    float4 PositionType;   // xyz = 位置、w = 種類（0 = 平行光源、1 = 点光源、2 = スポット、3 = ボリューム）
    float4 DirectionRange; // xyz = 方向（平行光源は光源方向、スポットは前方）、w = 範囲
    float4 Color;          // rgb * 強度、w = ボリューム密度
    float4 Extras;         // 内側の cos、外側の cos、影の有効フラグ、予約領域
};

Texture2D<float4> SceneColor : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float4> SceneVelocity : register(t2);
Texture3D<float4> FroxelHistory : register(t3);
Texture3D<float4> FroxelCurrent : register(t4);
Texture3D<float4> FroxelIntegrated : register(t5);
Texture2D<float4> PostA : register(t6);
Texture2D<float4> PostB : register(t7);
RaytracingAccelerationStructure ShadowScene : register(t10);
StructuredBuffer<FogLight> FogLights : register(t11);

RWTexture3D<float4> FroxelOut : register(u0);
RWTexture3D<float4> IntegratedOut : register(u1);
RWTexture2D<float4> PostAOut : register(u2);
RWTexture2D<float4> PostBOut : register(u3);
RWStructuredBuffer<uint> Histogram : register(u4);
RWStructuredBuffer<float> ExposureState : register(u5); // 0 = 露出、1 = 平均輝度、2 = 初期化済み

// ---- MegaLights M6：フロクセルのローカルライト（グリッドのセルから確率的に選ぶ）
#define MEGALIGHTS_CB_REGISTER b1
#include "MegaLightsCommon.hlsli"
StructuredBuffer<MegaLightData> FogMegaLights : register(t12);
StructuredBuffer<uint2> FogMegaLightCells : register(t13);
StructuredBuffer<uint> FogMegaLightIndices : register(t14);

SamplerState LinearClamp : register(s0);
SamplerState PointClamp : register(s1);

static const float kPi = 3.14159265f;
static const uint kHistogramBins = 64;

// ---------------------------------------------------------------------------------------------
// 補助関数
// ---------------------------------------------------------------------------------------------
float Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return (value >> 8) / 16777216.0f;
}

float3 ViewRay(float2 uv)
{
    float4 farPoint = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, 1, 1), InvViewProjection);
    return normalize(farPoint.xyz / farPoint.w - CameraPosition.xyz);
}

float3 WorldFromDepth(float2 uv, float depth)
{
    float4 world = mul(float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1), InvViewProjection);
    return world.xyz / world.w;
}

float SliceToDistance(float slice)
{
    return FogDepth.x * exp2(slice / FroxelExtent.z * FogDepth.z);
}

float DistanceToSliceCoordinate(float dist)
{
    return saturate(log2(max(dist, FogDepth.x) / FogDepth.x) / FogDepth.z);
}

float HenyeyGreenstein(float cosine, float g)
{
    g = clamp(g, -0.95f, 0.95f);
    float g2 = g * g;
    float denominator = max(1 + g2 - 2 * g * cosine, 0.0001f);
    return (1 - g2) / (4 * kPi * denominator * sqrt(denominator));
}

float TraceVisibility(float3 origin, float3 direction, float dist)
{
    if (FogMieColor.w < 0.5f)
    {
        return 1;
    }
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.02f;
    ray.TMax = max(dist, 0.03f);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(ShadowScene, RAY_FLAG_NONE, 0x01, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1 : 0;
}

// ワールド位置の媒質：rgb = ローカルフォグの入射散乱による発光、a = 消散（1/m）。
void EvaluateMedium(float3 position, out float globalDensity, out float3 localEmission, out float extinction)
{
    globalDensity = FogMedium0.x * exp(-max(position.y, 0) * FogMedium0.y);
    extinction = FogMedium0.z * globalDensity * FogMedium0.w;
    localEmission = 0;
    [loop] for (uint index = 0; index < FroxelExtent.w; ++index)
    {
        float4 data0 = LocalFogData0[index];
        float4 data1 = LocalFogData1[index];
        if (data1.w < 0.5f || data1.y <= 0.00001f)
        {
            continue;
        }
        float3 delta = position - data0.xyz;
        float radius = max(data0.w, 0.01f);
        float weight = data1.z >= 0.5f
                           ? saturate(1 - length(delta) / radius)
                           : saturate(1 - length(delta.xz) / radius) * exp(-abs(delta.y) * max(data1.x, 0.0001f));
        // 以前のレイマーチ式ローカルフォグと同じ光学的厚さのスケール（0.08）。
        float sigma = weight * data1.y * 0.08f;
        extinction += sigma;
        localEmission += LocalFogColor[index].rgb * sigma;
    }
}

// ---------------------------------------------------------------------------------------------
// MegaLights M6：フロクセルのローカルライト
//   セル（16px タイル × スライス）のライトから、影なしの寄与（輝度）に比例して N 個選び、
//   影を落とすライトだけ影レイを飛ばして 1/p で重み付けする（不偏）。ノイズはフロクセルの時間方向の履歴で減る。
//   従来の FogLights（上限 32）にはグリッドに載っていないライト（太陽・シャドウマップ持ち・Volume など）だけが入る。
// ---------------------------------------------------------------------------------------------
float3 MegaLightFogContribution(MegaLightData light, float3 position, float3 viewToCamera, float globalDensity,
                                float3 scatterAlbedo, float3 mieAlbedo, out float3 toLight, out float lightDistance)
{
    float3 offset = light.PositionType.xyz - position;
    lightDistance = length(offset);
    toLight = offset / max(lightDistance, 0.0001f);
    float fade = saturate(1 - lightDistance / max(light.DirectionRange.w, 0.01f));
    float attenuation = fade * fade;
    if ((int)round(light.PositionType.w) == 2)
    {
        float spotCosine = dot(-toLight, normalize(light.DirectionRange.xyz));
        attenuation *= smoothstep(light.Extra.y, light.Extra.x, spotCosine);
    }
    if (attenuation <= 0.00001f)
    {
        return 0;
    }
    float cosine = dot(toLight, viewToCamera);
    float3 phase =
        scatterAlbedo * (0.0596831f * (1 + cosine * cosine)) + mieAlbedo * HenyeyGreenstein(cosine, FogMedium1.z);
    // FogInject の従来ライトと同じ式（大気の媒質 + ライト自身のボリューム密度）
    float3 response = phase * globalDensity * FogMedium0.w + 0.16f * max(light.Extra.z, 0.0f) * 0.0795775f;
    return response * max(light.Color.rgb, 0.0f) * max(light.Flags.w, 0.0f) * attenuation;
}

float3 MegaLightsFogScattering(uint3 id, float3 position, float3 viewToCamera, float globalDensity,
                               float3 scatterAlbedo, float3 mieAlbedo)
{
    if (MegaLightsFog.x == 0u || !MLGridActive())
    {
        return 0;
    }
    float3 view = mul(float4(position, 1.0f), MLWorldToView).xyz;
    if (view.z <= MLDepth.z)
    {
        return 0;
    }
    float2 pixel = MLViewToPixel(view);
    if (any(pixel < 0.0f) || any(pixel >= float2(MLScreen.xy)))
    {
        return 0;
    }
    uint2 tile = min(uint2(pixel) / ML_TILE_SIZE, MLGrid.xy - 1u);
    uint2 cell = FogMegaLightCells[MLCellIndex(tile, MLSliceFromViewZ(view.z))];
    if (cell.y == 0u)
    {
        return 0;
    }

    const uint sampleCount = clamp(MegaLightsFog.y, 1u, 4u);
    const uint seed = MLSeed(id.xy + uint2(0u, id.z * 4099u), Extent.z, 0x464f47u);
    uint picks[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    float pickWeights[4] = {0, 0, 0, 0};
    float weightSum = 0;
    [loop] for (uint i = 0u; i < cell.y; ++i)
    {
        const uint lightIndex = FogMegaLightIndices[cell.x + i];
        float3 toLight;
        float lightDistance;
        const float w = dot(MegaLightFogContribution(FogMegaLights[lightIndex], position, viewToCamera, globalDensity,
                                                     scatterAlbedo, mieAlbedo, toLight, lightDistance),
                            float3(0.2126f, 0.7152f, 0.0722f));
        if (w <= 0.0f)
        {
            continue;
        }
        weightSum += w;
        [unroll] for (uint k = 0u; k < 4u; ++k)
        {
            if (k >= sampleCount)
            {
                break;
            }
            if (MLRandom(seed + k * 0x9e3779b9u + i * 0x85ebca6bu) * weightSum < w)
            {
                picks[k] = lightIndex;
                pickWeights[k] = w;
            }
        }
    }
    if (weightSum <= 0.0f)
    {
        return 0;
    }
    float3 result = 0;
    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        if (k >= sampleCount || picks[k] == 0xffffffffu)
        {
            continue;
        }
        const MegaLightData light = FogMegaLights[picks[k]];
        float3 toLight;
        float lightDistance;
        float3 contribution = MegaLightFogContribution(light, position, viewToCamera, globalDensity, scatterAlbedo,
                                                       mieAlbedo, toLight, lightDistance);
        float visibility = MLLightCastsShadow(light) ? TraceVisibility(position, toLight, lightDistance - 0.05f) : 1.0f;
        // p = w / Σw（寄与が 0 のライトは選ばれないが、そもそも寄与 0 なので不偏）
        result += contribution * visibility * (weightSum / pickWeights[k]);
    }
    return result / (float)sampleCount;
}

// ---------------------------------------------------------------------------------------------
// 1. フロクセルへの注入：媒質と照明をフロクセルごとに計算し、時間方向へ再投影する。
// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)] void FogInject(uint3 id : SV_DispatchThreadID) {
    if (any(id >= FroxelExtent.xyz))
    {
        return;
    }
    uint frame = Extent.z;
    // セル内でフロクセルごとにジッターを加え（深度方向は R2 列）、履歴で解消する。
    float jitter = frac(0.5f + frame * 0.618034f + Hash(id.x + id.y * 977u) * 0.25f);
    float2 uv = (id.xy + 0.5f) / FroxelExtent.xy;
    float3 rayDirection = ViewRay(uv);
    float dist = SliceToDistance(id.z + jitter);
    float3 position = CameraPosition.xyz + rayDirection * dist;

    float globalDensity;
    float3 localEmission;
    float extinction;
    EvaluateMedium(position, globalDensity, localEmission, extinction);

    float3 scattering = localEmission;
    float3 viewToCamera = -rayDirection;
    float3 scatterAlbedo = FogRayleighColor.rgb * FogMedium1.x;
    float3 mieAlbedo = FogMieColor.rgb * FogMedium1.y;
    uint shadowedLocal = 0;
    [loop] for (uint lightIndex = 0; lightIndex < Extent.w; ++lightIndex)
    {
        FogLight light = FogLights[lightIndex];
        int type = (int)round(light.PositionType.w);
        float3 toLight;
        float attenuation = 1;
        float lightDistance = 200.0f;
        if (type == 0)
        {
            toLight = normalize(light.DirectionRange.xyz);
        }
        else
        {
            float3 offset = light.PositionType.xyz - position;
            lightDistance = length(offset);
            toLight = offset / max(lightDistance, 0.0001f);
            float fade = saturate(1 - lightDistance / max(light.DirectionRange.w, 0.01f));
            attenuation = fade * fade;
            if (type == 2 || type == 3)
            {
                float spotCosine = dot(-toLight, normalize(light.DirectionRange.xyz));
                attenuation *= smoothstep(light.Extras.y, light.Extras.x, spotCosine);
            }
        }
        if (attenuation <= 0.00001f)
        {
            continue;
        }
        float cosine = dot(toLight, viewToCamera);
        float3 phase =
            scatterAlbedo * (0.0596831f * (1 + cosine * cosine)) + mieAlbedo * HenyeyGreenstein(cosine, FogMedium1.z);
        // 大気媒質にライト固有の設定済みボリューム密度を加える（光芒）。
        float3 response =
            phase * globalDensity * FogMedium0.w + (type == 0 ? 0.025f : 0.16f) * light.Color.w * 0.0795775f;
        float3 incident = light.Color.rgb * attenuation;
        if (all(response * incident < 0.0000001f))
        {
            continue;
        }
        float visibility = 1;
        bool castShadow = light.Extras.z > 0.5f;
        if (castShadow && (type == 0 || shadowedLocal < 4))
        {
            visibility = TraceVisibility(position, toLight, type == 0 ? 200.0f : lightDistance - 0.05f);
            shadowedLocal += type == 0 ? 0 : 1;
        }
        scattering += response * incident * visibility * FogMedium1.w;
    }
    scattering +=
        MegaLightsFogScattering(id, position, viewToCamera, globalDensity, scatterAlbedo, mieAlbedo) * FogMedium1.w;
    // 低コストの多重散乱と空の環境光で、影の中のフォグが真っ黒になるのを防ぐ。
    scattering += FogAmbientColor.rgb * FogRayleighColor.w * globalDensity * FogMedium0.w;

    float4 current = float4(scattering, extinction);
    if (PrevCameraPosition.w > 0.5f)
    {
        float4 previousClip = mul(float4(position, 1), PrevViewProjection);
        if (previousClip.w > 0.0001f)
        {
            float2 previousUv = float2(previousClip.x, -previousClip.y) / previousClip.w * 0.5f + 0.5f;
            float previousDistance = length(position - PrevCameraPosition.xyz);
            float previousSlice = DistanceToSliceCoordinate(previousDistance);
            if (all(previousUv > 0) && all(previousUv < 1))
            {
                float4 history = FroxelHistory.SampleLevel(LinearClamp, float3(previousUv, previousSlice), 0);
                current = lerp(current, history, FogDepth.w);
            }
        }
    }
    FroxelOut[id] = current;
}

    // ---------------------------------------------------------------------------------------------
    // 2. 手前から奥へ積分する（エネルギー保存、Hillaire 2015）。
    // ---------------------------------------------------------------------------------------------
    [numthreads(8, 8, 1)] void FogIntegrate(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FroxelExtent.xy))
    {
        return;
    }
    float3 accumulated = 0;
    float transmittance = 1;
    float previousDistance = 0;
    [loop] for (uint slice = 0; slice < FroxelExtent.z; ++slice)
    {
        float dist = SliceToDistance(slice + 1);
        float thickness = dist - previousDistance;
        previousDistance = dist;
        float4 medium = FroxelCurrent[uint3(id.xy, slice)];
        float extinction = max(medium.a, 0.0000001f);
        float sliceTransmittance = exp(-extinction * thickness);
        accumulated += transmittance * medium.rgb * (1 - sliceTransmittance) / extinction;
        transmittance *= sliceTransmittance;
        IntegratedOut[uint3(id.xy, slice)] = float4(accumulated, transmittance);
    }
}

float4 SampleIntegratedFog(float2 uv, float dist)
{
    float coordinate = DistanceToSliceCoordinate(dist) * FroxelExtent.z; // スライス単位
    // スライス z は奥の境界（z + 1）までの積分値を格納する。
    float4 fog =
        FroxelIntegrated.SampleLevel(LinearClamp, float3(uv, saturate((coordinate - 0.5f) / FroxelExtent.z)), 0);
    if (coordinate < 1)
    {
        fog = lerp(float4(0, 0, 0, 1), fog, saturate(coordinate));
    }
    return fog;
}

// ---------------------------------------------------------------------------------------------
// 3. 合成：シーン → PostA（フォグ）。背景にはボリュームの最遠端を使う。
// ---------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)] void Composite(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Extent.xy))
    {
        return;
    }
    float4 color = SceneColor[id.xy];
    float depth = SceneDepth[id.xy];
    bool physicalSkyPixel = depth >= 0.9999f && (Features.x & 2u) != 0;
    if ((Features.x & 1u) != 0 && !physicalSkyPixel)
    {
        float2 uv = (id.xy + 0.5f) / Extent.xy;
        float dist = depth >= 0.9999f ? FogDepth.y : length(WorldFromDepth(uv, depth) - CameraPosition.xyz);
        // 画面空間の小さなディザーで、フロクセルの三線形補間の形状を目立たなくする。
        float dither = (Hash(id.x + id.y * 4099u + Extent.z * 7919u) - 0.5f) / Extent.x;
        float4 fog = SampleIntegratedFog(uv + dither, dist);
        color.rgb = color.rgb * fog.a + fog.rgb;
    }
    PostAOut[id.xy] = color;
}

// ---------------------------------------------------------------------------------------------
// 4. 自動露出：対数輝度ヒストグラム → パーセンタイル平均 → 時間方向の順応。
// ---------------------------------------------------------------------------------------------
groupshared uint SharedHistogram[kHistogramBins];

[numthreads(64, 1, 1)] void HistogramClear(uint3 id : SV_DispatchThreadID) {
    if (id.x < kHistogramBins)
    {
        Histogram[id.x] = 0;
    }
}

    [numthreads(16, 16, 1)] void HistogramBuild(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index < kHistogramBins)
    {
        SharedHistogram[index] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    // 平均の計算には半解像度で十分で、アトミック演算の回数も半減する。
    uint2 pixel = id.xy * 2;
    if (all(pixel < Extent.xy))
    {
        float luminance = dot(PostA[pixel].rgb, float3(0.2126f, 0.7152f, 0.0722f));
        uint bin = 0;
        if (luminance > 0.000001f)
        {
            float normalized = saturate((log2(luminance) - ExposureParams.x) / ExposureParams.y);
            bin = 1 + (uint)(normalized * (kHistogramBins - 2));
        }
        InterlockedAdd(SharedHistogram[bin], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < kHistogramBins && SharedHistogram[index] != 0)
    {
        InterlockedAdd(Histogram[index], SharedHistogram[index]);
    }
}

[numthreads(64, 1, 1)] void ExposureAdapt(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    SharedHistogram[index] = Histogram[index];
    GroupMemoryBarrierWithGroupSync();
    if (index != 0)
    {
        return;
    }
    // 黒いピクセル（ビン 0）を除き、下位と上位のパーセンタイル間で平均する。
    uint total = 0;
    [loop] for (uint bin = 1; bin < kHistogramBins; ++bin)
    {
        total += SharedHistogram[bin];
    }
    float low = total * ExposureLimits.z;
    float high = total * ExposureLimits.w;
    float sum = 0;
    float weight = 0;
    float cumulative = 0;
    [loop] for (uint bucket = 1; bucket < kHistogramBins; ++bucket)
    {
        float count = SharedHistogram[bucket];
        float begin = cumulative;
        cumulative += count;
        float used = max(min(cumulative, high) - max(begin, low), 0);
        float log2Luminance = ExposureParams.x + (bucket - 0.5f) / (kHistogramBins - 2) * ExposureParams.y;
        sum += log2Luminance * used;
        weight += used;
    }
    float averageLuminance = weight > 0 ? exp2(sum / weight) : 0.18f;
    float target = 0.18f * exp2(ExposureParams.z) / max(averageLuminance, 0.0001f);
    target = clamp(target, ExposureLimits.x, ExposureLimits.y);
    float previous = ExposureState[0];
    bool initialized =
        FogAmbientColor.w < 0.5f && ExposureState[2] > 0.5f && previous > 0 && !isnan(previous) && !isinf(previous);
    // 対数空間で順応させる。目と同様、明るくなる速度は暗くなる速度より遅い。
    float speed = ExposureParams.w * (target > previous ? 0.6f : 1.0f);
    float blend = 1 - exp(-CameraPosition.w * speed);
    float exposure = initialized ? exp2(lerp(log2(previous), log2(target), blend)) : target;
    ExposureState[0] = exposure;
    ExposureState[1] = averageLuminance;
    ExposureState[2] = 1;
}

float CurrentExposure()
{
    return Features.y != 0 ? max(ExposureState[0], 0.000001f) : 1;
}

// ---------------------------------------------------------------------------------------------
// 5. 解決：モーションブラーと露出、PostA → PostB。
// ---------------------------------------------------------------------------------------------
float4 ResolvePixel(uint3 id)
{
    if (any(id.xy >= Extent.xy))
    {
        return 0;
    }
    float4 center = PostA[id.xy];
    float3 color = center.rgb;
    if (Features.z != 0)
    {
        float2 velocity = SceneVelocity[id.xy].zw * Extent.xy * MotionBlurParams.x; // ピクセル単位
        float velocityLength = sqrt(dot(velocity, velocity));
        if (velocityLength > 0.5f)
        {
            velocity *= min(velocityLength, MotionBlurParams.y) / velocityLength;
            float centerDepth = SceneDepth[id.xy];
            uint samples = (uint)MotionBlurParams.z;
            float offset = Hash(id.x * 3u + id.y * 7919u + Extent.z) - 0.5f;
            float3 sum = color;
            float weightSum = 1;
            [loop] for (uint index = 0; index < samples; ++index)
            {
                float t = ((index + 0.5f + offset) / samples) - 0.5f;
                int2 samplePixel = clamp(int2(id.xy + velocity * t), 0, int2(Extent.xy) - 1);
                float sampleDepth = SceneDepth[samplePixel];
                float2 sampleVelocity = SceneVelocity[samplePixel].zw * Extent.xy * MotionBlurParams.x;
                // 背景のサンプルは常に加算する。前景のサンプルはこのピクセルに届くほど
                // 移動した場合だけ加算し、静止した前景が引き伸ばされるのを防ぐ。
                bool behind = sampleDepth >= centerDepth - 0.00002f;
                float reach = sqrt(dot(sampleVelocity, sampleVelocity)) * 0.5f;
                float dist = abs(t) * velocityLength;
                float weight = behind ? 1 : saturate(reach - dist + 1);
                sum += PostA[samplePixel].rgb * weight;
                weightSum += weight;
            }
            color = sum / weightSum;
        }
    }
    return float4(color * CurrentExposure(), center.a);
}

// ---------------------------------------------------------------------------------------------
// 6. 被写界深度（散布を収集で近似、黄金角の円盤）、PostB → PostA。
// ---------------------------------------------------------------------------------------------
float CircleOfConfusion(float2 uv, float depth)
{
    float dist = depth >= 0.9999f ? 100000.0f : length(WorldFromDepth(uv, depth) - CameraPosition.xyz);
    float focus = max(DofParams.x, 0.05f);
    float blur = saturate((abs(dist - focus) - DofParams.y * 0.5f) / focus);
    return blur * DofParams.z; // ピクセル単位
}

Texture2D<float4> HalfResolveInput : register(t0, space11);
Texture2D<float4> HalfDofInput : register(t1, space11);
RWTexture2D<float4> HalfResolveOutput : register(u0, space11);
RWTexture2D<float4> HalfDofOutput : register(u1, space11);

cbuffer HalfLensConstants : register(b2)
{
    uint HalfWidth, HalfHeight, HalfHasDof, HalfPadding;
};

static bool UseHalfDofSource = false;

float4 DofSource(int2 pixel)
{
    if (UseHalfDofSource)
    {
        return HalfResolveInput.Load(int3(clamp(pixel / 2, 0, int2(HalfWidth, HalfHeight) - 1), 0));
    }
    return PostB.Load(int3(pixel, 0));
}

float4 DepthOfFieldPixel(uint3 id)
{
    if (any(id.xy >= Extent.xy))
    {
        return 0;
    }
    float2 uv = (id.xy + 0.5f) / Extent.xy;
    float centerDepth = SceneDepth[id.xy];
    float centerCoc = CircleOfConfusion(uv, centerDepth);
    float4 center = DofSource(int2(id.xy));
    float searchRadius = DofParams.z;
    float centerWeight = 1 / max(centerCoc * centerCoc, 1);
    float3 sum = center.rgb * centerWeight;
    float weightSum = centerWeight;
    const uint kSamples = 32;
    float rotation = Hash(id.x + id.y * 7919u) * 6.2831853f;
    [loop] for (uint index = 0; index < kSamples; ++index)
    {
        float radius = sqrt((index + 0.5f) / kSamples) * searchRadius;
        float angle = index * 2.39996323f + rotation;
        float2 offset = float2(cos(angle), sin(angle)) * radius;
        int2 samplePixel = clamp(int2(id.xy + offset), 0, int2(Extent.xy) - 1);
        float sampleDepth = SceneDepth[samplePixel];
        float sampleCoc = CircleOfConfusion((samplePixel + 0.5f) / Extent.xy, sampleDepth);
        // サンプル自身のぼけ円盤がこのピクセルを覆う場合に寄与する。中心より奥のサンプルは
        // 中心のぼけ量で制限し、合焦した前景の鮮明さを保つ。
        float coverage = sampleDepth > centerDepth ? min(sampleCoc, centerCoc) : sampleCoc;
        float weight = saturate(coverage - radius + 1) / max(coverage * coverage, 1);
        sum += DofSource(samplePixel).rgb * weight;
        weightSum += weight;
    }
    return float4(sum / weightSum, center.a);
}

[numthreads(8, 8, 1)] void Resolve(uint3 id : SV_DispatchThreadID) {
    if (all(id.xy < uint2(Extent.xy)))
    {
        PostBOut[id.xy] = ResolvePixel(id);
    }
}

    [numthreads(8, 8, 1)] void DepthOfField(uint3 id : SV_DispatchThreadID)
{
    if (all(id.xy < uint2(Extent.xy)))
    {
        PostAOut[id.xy] = DepthOfFieldPixel(id);
    }
}

[numthreads(8, 8, 1)] void ResolveHalf(uint3 id : SV_DispatchThreadID) {
    if (id.x >= HalfWidth || id.y >= HalfHeight)
    {
        return;
    }
    HalfResolveOutput[id.xy] = ResolvePixel(uint3(min(id.xy * 2 + 1, uint2(Extent.xy) - 1), 0));
}[numthreads(8, 8, 1)] void DofHalf(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= HalfWidth || id.y >= HalfHeight)
    {
        return;
    }
    UseHalfDofSource = true;
    HalfDofOutput[id.xy] = DepthOfFieldPixel(uint3(min(id.xy * 2 + 1, uint2(Extent.xy) - 1), 0));
}
[numthreads(8, 8, 1)] void UpsampleLens(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(Extent.xy)))
    {
        return;
    }
    float depth = SceneDepth[id.xy];
    float2 position = (float2(id.xy) - 1) * .5;
    int2 base = int2(floor(position));
    float2 fraction = frac(position);
    float4 sum = 0;
    float weights = 0;
    [unroll] for (uint y = 0; y < 2; ++y)
    {
        [unroll] for (uint x = 0; x < 2; ++x)
        {
            int2 low = clamp(base + int2(x, y), 0, int2(HalfWidth, HalfHeight) - 1);
            uint2 full = min(uint2(low) * 2 + 1, uint2(Extent.xy) - 1);
            float geometry = exp2(-abs(SceneDepth[full] - depth) / max(2e-5, abs(1 - depth) * .02));
            float2 spatial = lerp(1 - fraction, fraction, float2(x, y));
            float weight = spatial.x * spatial.y * geometry;
            float4 color = HalfHasDof != 0 ? HalfDofInput.Load(int3(low, 0)) : HalfResolveInput.Load(int3(low, 0));
            sum += color * weight;
            weights += weight;
        }
    }
    float4 full = PostA.Load(int3(id.xy, 0));
    full.rgb *= CurrentExposure();
    float motion = Features.z != 0 ? length(SceneVelocity[id.xy].zw * Extent.xy * MotionBlurParams.x) : 0;
    float coc = HalfHasDof != 0 ? CircleOfConfusion((id.xy + .5) / Extent.xy, depth) : 0;
    float blend = saturate(max(motion, coc) - .5);
    PostBOut[id.xy] = weights > 1e-5 ? lerp(full, sum / weights, blend) : full;
}
