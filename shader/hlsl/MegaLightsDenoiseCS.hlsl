// =============================================================================================
// MegaLights M3 : デノイズ（SVGF 方式：時間方向の蓄積 + 分散に応じた À-Trous）
//   入力  Raw      : MegaLightsShadePS が書いた ML6 の結果（デモジュレート済みの照明。a = 1 で対象ピクセル）
//   Temporal       : Velocity で前フレームへリプロジェクト → 深度（前フレームのビュー Z）と法線で履歴を棄却
//                    → 指数移動平均。輝度の 1 次・2 次モーメントから分散を求める（履歴が短い間は 3x3 の空間分散）
//                    ・ファイアフライ抑制：中心の輝度を近傍の最大値までに抑える（近傍に同程度の値が無い孤立点だけ）
//                    ・アンチラグ：ライトが変わったフレームは履歴の長さを短く制限して素早く追従する
//   ATrous         : 5x5 B3 カーネル（間隔 1, 2, 4, ...）。エッジ停止は面の平面距離・法線・輝度（分散で正規化）
//                    1 回目の結果を履歴へ書き戻す（次フレームの時間方向の入力がきれいになる）
//   出力は「照明だけ」なので、ディファードで材質の反射率（MLRemodulation）を掛け戻す。
//   実行時に DXC（cs_6_5）でコンパイルする。.vcxproj への追加は不要。
// =============================================================================================
#define MEGALIGHTS_CB_REGISTER b0
#include "MegaLightsCommon.hlsli"

cbuffer DenoiseConstants : register(b1)
{
    float4 PrevViewZRow; // 前フレームのビュー Z = dot(float4(world, 1), PrevViewZRow)
    uint HistoryValid;   // 0: 前フレームの履歴を使わない
    uint MaxHistory;     // 履歴の長さの上限（このフレームの値。アンチラグ時は短い）
    uint StepSize;       // À-Trous の間隔（ピクセル）
    uint DenoiseFlags;   // bit0: ファイアフライ抑制, bit1: 1 回目の結果を履歴へ書き戻す
    float AlphaMin;      // 色の最小ブレンド率
    float MomentsAlphaMin;
    float DepthTolerance;  // 履歴の深度の許容差（相対）
    float NormalThreshold; // 履歴の法線の許容（cos）
    float PhiNormal;       // À-Trous の法線の鋭さ
    float PhiLuminance;    // À-Trous の輝度の許容（σ の倍数）
    float PhiPlane;        // À-Trous の平面距離の許容（ピクセルの大きさの倍数）
    float Padding;
};

Texture2D<float4> GBufferNormal : register(t9);
Texture2D<float> GBufferDepth : register(t10);
Texture2D<float4> GBufferVelocity : register(t15);

StructuredBuffer<float4> InputColor : register(t0);      // Temporal: Raw / ATrous: 前段の結果
StructuredBuffer<float4> PrevColor : register(t1);       // rgb: 照明, a: 履歴の長さ
StructuredBuffer<uint4> PrevMoments : register(t2);      // x: m1, y: m2, z: ビュー Z, w: 法線（oct, unorm16x2）
RWStructuredBuffer<float4> OutputColor : register(u0);   // Temporal / ATrous の出力（rgb, a: 分散。対象外は -1）
RWStructuredBuffer<float4> HistoryColor : register(u1);  // このフレームの履歴（色）
RWStructuredBuffer<uint4> HistoryMoments : register(u2); // このフレームの履歴（モーメント）

#define ML_DENOISE_FIREFLY 1u
#define ML_DENOISE_FEEDBACK 2u

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

uint PixelIndex(uint2 pixel)
{
    return pixel.y * MLScreen.x + pixel.x;
}

bool InScreen(int2 pixel)
{
    return all(pixel >= 0) && all(pixel < int2(MLScreen.xy));
}

float2 OctWrap(float2 v)
{
    return (1.0f - abs(v.yx)) * float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}

uint PackNormal(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.z >= 0.0f ? n.xy : OctWrap(n.xy);
    uint2 q = (uint2)round(saturate(e * 0.5f + 0.5f) * 65535.0f);
    return q.x | (q.y << 16u);
}

float3 UnpackNormal(uint packed)
{
    float2 e = float2(packed & 0xffffu, packed >> 16u) / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f)
    {
        n.xy = OctWrap(n.xy);
    }
    return normalize(n);
}

float3 LoadNormal(uint2 pixel)
{
    return normalize(GBufferNormal.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
}

// --------------------------------------------------------------------------------------------
// 時間方向
// --------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)] void Temporal(uint3 dispatch : SV_DispatchThreadID) {
    const uint2 pixel = dispatch.xy;
    if (any(pixel >= MLScreen.xy))
    {
        return;
    }
    const uint index = PixelIndex(pixel);
    const float4 raw = InputColor[index];
    const float depth = GBufferDepth.Load(int3(pixel, 0));
    if (raw.a < 0.5f || !MLIsGeometryDepth(depth))
    {
        OutputColor[index] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        HistoryColor[index] = 0.0f;
        HistoryMoments[index] = uint4(0u, 0u, 0u, 0u);
        return;
    }

    const float viewZ = MLViewZFromDepth(depth);
    const float3 normal = LoadNormal(pixel);
    const float3 worldPos = MLWorldPositionFromDepth(pixel, depth);

    // ---- 近傍（3x3）の統計：ファイアフライ抑制と、履歴が短い間の分散
    float3 color = max(raw.rgb, 0.0f);
    float neighborMax = 0.0f;
    float sum1 = 0.0f;
    float sum2 = 0.0f;
    float count = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2 q = int2(pixel) + int2(x, y);
            if (!InScreen(q))
            {
                continue;
            }
            const float4 neighbor = InputColor[PixelIndex(uint2(q))];
            if (neighbor.a < 0.5f)
            {
                continue;
            }
            const float l = Luminance(max(neighbor.rgb, 0.0f));
            sum1 += l;
            sum2 += l * l;
            count += 1.0f;
            if (x != 0 || y != 0)
            {
                neighborMax = max(neighborMax, l);
            }
        }
    }
    float luminance = Luminance(color);
    if ((DenoiseFlags & ML_DENOISE_FIREFLY) != 0u && count > 1.5f && luminance > neighborMax && luminance > 0.0f)
    {
        color *= neighborMax / luminance;
        luminance = neighborMax;
    }

    // ---- リプロジェクト（velocity.zw = 現在の UV - 前フレームの UV）
    const float2 size = float2(MLScreen.xy);
    const float2 previousUv = (float2(pixel) + 0.5f) / size - GBufferVelocity.Load(int3(pixel, 0)).zw;
    const float2 previousPixel = previousUv * size - 0.5f;
    const float expectedPreviousZ = dot(float4(worldPos, 1.0f), PrevViewZRow);
    float3 historyColor = 0.0f;
    float historyLength = 0.0f;
    float2 historyMoments = 0.0f;
    float weightSum = 0.0f;
    if (HistoryValid != 0u)
    {
        const int2 base = int2(floor(previousPixel));
        const float2 f = previousPixel - float2(base);
        const float bilinear[4] = {(1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y), (1.0f - f.x) * f.y, f.x * f.y};
        const int2 offsets[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};
        [unroll] for (uint tap = 0u; tap < 4u; ++tap)
        {
            const int2 q = base + offsets[tap];
            if (!InScreen(q) || bilinear[tap] <= 0.0f)
            {
                continue;
            }
            const uint qIndex = PixelIndex(uint2(q));
            const uint4 moments = PrevMoments[qIndex];
            const float previousZ = asfloat(moments.z);
            if (previousZ <= 0.0f)
            {
                continue;
            }
            if (abs(previousZ - expectedPreviousZ) > DepthTolerance * max(expectedPreviousZ, 1.0e-3f))
            {
                continue;
            }
            if (dot(UnpackNormal(moments.w), normal) < NormalThreshold)
            {
                continue;
            }
            const float4 previous = PrevColor[qIndex];
            historyColor += previous.rgb * bilinear[tap];
            historyLength += previous.a * bilinear[tap];
            historyMoments += float2(asfloat(moments.x), asfloat(moments.y)) * bilinear[tap];
            weightSum += bilinear[tap];
        }
    }

    float historyLen = 1.0f;
    float3 outputColor = color;
    float2 moments = float2(luminance, luminance * luminance);
    if (weightSum > 1.0e-3f)
    {
        historyColor /= weightSum;
        historyMoments /= weightSum;
        historyLen = min(historyLength / weightSum + 1.0f, (float)max(MaxHistory, 1u));
        const float alpha = max(AlphaMin, 1.0f / historyLen);
        const float momentsAlpha = max(MomentsAlphaMin, 1.0f / historyLen);
        outputColor = lerp(historyColor, color, alpha);
        moments = lerp(historyMoments, moments, momentsAlpha);
    }

    float variance;
    if (historyLen < 4.0f && count > 0.5f)
    {
        // 履歴が短い：空間の分散で代用（大きめに見積もって強くぼかす）
        const float mean = sum1 / count;
        variance = max(sum2 / count - mean * mean, 0.0f) * (4.0f / historyLen);
    }
    else
    {
        variance = max(moments.y - moments.x * moments.x, 0.0f);
    }

    OutputColor[index] = float4(outputColor, variance);
    HistoryColor[index] = float4(outputColor, historyLen);
    HistoryMoments[index] = uint4(asuint(moments.x), asuint(moments.y), asuint(viewZ), PackNormal(normal));
}

    // --------------------------------------------------------------------------------------------
    // À-Trous（分散で輝度の許容を決める）
    // --------------------------------------------------------------------------------------------
    [numthreads(8, 8, 1)] void ATrous(uint3 dispatch : SV_DispatchThreadID)
{
    const uint2 pixel = dispatch.xy;
    if (any(pixel >= MLScreen.xy))
    {
        return;
    }
    const uint index = PixelIndex(pixel);
    const float4 center = InputColor[index];
    if (center.a < 0.0f)
    {
        OutputColor[index] = center;
        return;
    }
    const float depth = GBufferDepth.Load(int3(pixel, 0));
    const float3 normal = LoadNormal(pixel);
    const float3 worldPos = MLWorldPositionFromDepth(pixel, depth);
    const float viewZ = MLViewZFromDepth(depth);
    // 1 ピクセルの大きさ（ワールド）。平面距離の許容をこれに比例させる
    const float pixelFootprint = 2.0f * viewZ / (MLProjection.y * (float)MLScreen.y);

    // 中心の分散を 3x3 ガウスで平滑化（輝度の許容に使う）
    float filteredVariance = 0.0f;
    float varianceWeight = 0.0f;
    [unroll] for (int vy = -1; vy <= 1; ++vy)
    {
        [unroll] for (int vx = -1; vx <= 1; ++vx)
        {
            const int2 q = int2(pixel) + int2(vx, vy);
            if (!InScreen(q))
            {
                continue;
            }
            const float v = InputColor[PixelIndex(uint2(q))].a;
            if (v < 0.0f)
            {
                continue;
            }
            const float w = (vx == 0 ? 0.5f : 0.25f) * (vy == 0 ? 0.5f : 0.25f);
            filteredVariance += v * w;
            varianceWeight += w;
        }
    }
    filteredVariance /= max(varianceWeight, 1.0e-6f);
    const float centerLuminance = Luminance(center.rgb);
    const float luminanceScale = 1.0f / (PhiLuminance * sqrt(max(filteredVariance, 0.0f)) + 1.0e-4f);
    const float planeScale = 1.0f / (PhiPlane * pixelFootprint * (float)StepSize + 1.0e-4f);

    const float kernel[3] = {3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f};
    const float centerWeight = kernel[0] * kernel[0];
    float3 colorSum = center.rgb * centerWeight;
    float varianceSum = center.a * centerWeight * centerWeight;
    float weightSum = centerWeight;
    [unroll] for (int y = -2; y <= 2; ++y)
    {
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }
            const int2 q = int2(pixel) + int2(x, y) * (int)StepSize;
            if (!InScreen(q))
            {
                continue;
            }
            const float4 tapColor = InputColor[PixelIndex(uint2(q))];
            if (tapColor.a < 0.0f)
            {
                continue;
            }
            const float qDepth = GBufferDepth.Load(int3(q, 0));
            const float3 qWorld = MLWorldPositionFromDepth(uint2(q), qDepth);
            const float3 qNormal = LoadNormal(uint2(q));
            const float wPlane = abs(dot(normal, qWorld - worldPos)) * planeScale;
            const float wNormal = pow(saturate(dot(normal, qNormal)), PhiNormal);
            const float wLuminance = abs(Luminance(tapColor.rgb) - centerLuminance) * luminanceScale;
            const float w = kernel[abs(x)] * kernel[abs(y)] * wNormal * exp(-wPlane - wLuminance);
            colorSum += tapColor.rgb * w;
            varianceSum += tapColor.a * w * w;
            weightSum += w;
        }
    }
    const float3 color = colorSum / weightSum;
    OutputColor[index] = float4(color, varianceSum / (weightSum * weightSum));
    if ((DenoiseFlags & ML_DENOISE_FEEDBACK) != 0u)
    {
        // 1 回目の結果を履歴の色へ（履歴の長さはそのまま）
        HistoryColor[index] = float4(color, HistoryColor[index].a);
    }
}
