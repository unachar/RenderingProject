#define SHADER_POSTPROCESS
#include "common.hlsl"

// =============================================================================================
// TAA（ちらつき対策版）
//   ・動きベクトルは 3x3 で最も手前の深度のピクセルのものを使う（細い物・輪郭の取りこぼしによる履歴の引きずり防止）
//   ・履歴は Catmull-Rom（5 タップ）で読む（双線形で読むたびに履歴がぼけ、ぼけを補うシャープ処理でちらつく、の悪循環を断つ）
//   ・近傍は YCoCg の平均 ± γσ（分散クリップ）。min/max の箱でクランプすると、遠くの 1px 未満の物が
//     ジッターのたびに箱の外へ出て履歴が捨てられ、点滅する。分散クリップは平均へ向けて線分で切るので安定
//   ・静止していて履歴とよく合うピクセルほど γ を広げ、履歴を長く使う（遠景のちらつき対策の本体）
//   ・輝度による重み付け（Karis）：明るい 1px が履歴を支配して明滅するのを抑える
//   定数・ルート引数は従来と同じ
// =============================================================================================
Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> HistoryColor : register(t1);
Texture2D<float> DepthBuffer : register(t2);
Texture2D<float4> VelocityBuffer : register(t3);
SamplerState LinearSampler : register(s0);

cbuffer AaConstants : register(b4)
{
    float4x4 PrevViewProj;
    float BlendFactor;
    float2 RcpFrame;
    float HistoryValid;
};

float3 RgbToYCoCg(float3 c)
{
    return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b, 0.5f * c.r - 0.5f * c.b, -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}

float3 YCoCgToRgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// 輝度の逆数で重み付け（HDR の明るい点の支配を抑える）
float LumaWeight(float3 ycocg)
{
    return 1.0f / (1.0f + max(ycocg.x, 0.0f));
}

// Catmull-Rom を双線形 5 回で（Jimenez 2016）
float3 SampleHistoryCatmullRom(float2 uv, float2 textureSize)
{
    const float2 position = uv * textureSize;
    const float2 center = floor(position - 0.5f) + 0.5f;
    const float2 f = position - center;
    const float2 f2 = f * f;
    const float2 f3 = f2 * f;
    const float2 w0 = -0.5f * f3 + f2 - 0.5f * f;
    const float2 w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
    const float2 w2 = -1.5f * f3 + 2.0f * f2 + 0.5f * f;
    const float2 w3 = 0.5f * f3 - 0.5f * f2;
    const float2 w12 = w1 + w2;
    const float2 texel = 1.0f / textureSize;
    const float2 uv0 = (center - 1.0f) * texel;
    const float2 uv3 = (center + 2.0f) * texel;
    const float2 uv12 = (center + w2 / w12) * texel;
    float3 result = 0.0f;
    float weight = 0.0f;
    float w;
    w = w12.x * w0.y;  result += HistoryColor.SampleLevel(LinearSampler, float2(uv12.x, uv0.y), 0).rgb * w;  weight += w;
    w = w0.x * w12.y;  result += HistoryColor.SampleLevel(LinearSampler, float2(uv0.x, uv12.y), 0).rgb * w;  weight += w;
    w = w12.x * w12.y; result += HistoryColor.SampleLevel(LinearSampler, float2(uv12.x, uv12.y), 0).rgb * w; weight += w;
    w = w3.x * w12.y;  result += HistoryColor.SampleLevel(LinearSampler, float2(uv3.x, uv12.y), 0).rgb * w;  weight += w;
    w = w12.x * w3.y;  result += HistoryColor.SampleLevel(LinearSampler, float2(uv12.x, uv3.y), 0).rgb * w;  weight += w;
    return max(result / max(weight, 1.0e-4f), 0.0f);
}

// 平均へ向かう線分で AABB にクリップ（クランプより色ずれが少ない）
float3 ClipToAabb(float3 history, float3 center, float3 extents)
{
    const float3 offset = history - center;
    const float3 unit = abs(offset / max(extents, 1.0e-4f));
    const float maxUnit = max(unit.x, max(unit.y, unit.z));
    return maxUnit > 1.0f ? center + offset / maxUnit : history;
}

float4 main(PSInputPostProcess input) : SV_TARGET
{
    const int2 pixel = int2(input.Position.xy);
    const float2 uv = input.TexCoord;
    const float2 textureSize = 1.0f / max(RcpFrame, 1.0e-6f);
    const int2 maxPixel = int2(textureSize) - 1;

    // ---- 近傍の統計（YCoCg・輝度重み付き）と、最も手前のピクセル
    const float3 currentRgb = CurrentColor.Load(int3(pixel, 0)).rgb;
    const float3 current = RgbToYCoCg(currentRgb);
    float3 moment1 = 0.0f, moment2 = 0.0f;
    float closestDepth = 2.0f;
    int2 closestPixel = pixel;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 neighbor = clamp(pixel + int2(x, y), int2(0, 0), maxPixel);
            const float3 sampleColor = RgbToYCoCg(CurrentColor.Load(int3(neighbor, 0)).rgb);
            moment1 += sampleColor;
            moment2 += sampleColor * sampleColor;
            const float depth = DepthBuffer.Load(int3(neighbor, 0));
            if (depth < closestDepth)
            {
                closestDepth = depth;
                closestPixel = neighbor;
            }
        }
    }
    const float3 mean = moment1 / 9.0f;
    const float3 sigma = sqrt(max(moment2 / 9.0f - mean * mean, 0.0f));

    // ---- 履歴
    const float2 velocity = VelocityBuffer.Load(int3(closestPixel, 0)).ba;
    const float2 historyUv = uv - velocity;
    const bool historyInBounds = all(historyUv >= 0.0f) && all(historyUv <= 1.0f);
    float3 history = RgbToYCoCg(SampleHistoryCatmullRom(historyUv, textureSize));

    // 動きが小さい（静止に近い）ほど、クリップの幅 γ を広げて履歴を長く保つ
    const float motionPixels = length(velocity * textureSize);
    const float stillness = saturate(1.0f - motionPixels / 2.0f);
    const float gamma = lerp(1.0f, 1.75f, stillness);
    const float3 boxMin = min(mean - gamma * sigma, current);
    const float3 boxMax = max(mean + gamma * sigma, current);
    const float3 clipped = ClipToAabb(history, (boxMin + boxMax) * 0.5f, (boxMax - boxMin) * 0.5f);
    // クリップで大きく動いた（= 履歴が合っていない）ほど、今のフレームを強く使う
    const float rejection = saturate(length(clipped - history) / max(clipped.x + 0.05f, 0.05f));
    history = clipped;

    // ---- 混合率：基本は BlendFactor。静止なら長い履歴（最大 0.96）、速い動きや不一致では短く
    const float motionConfidence = saturate(1.0f - motionPixels / 32.0f);
    float historyWeight = lerp(BlendFactor, 0.96f, stillness) * motionConfidence * (1.0f - 0.5f * rejection);
    historyWeight *= HistoryValid * (historyInBounds ? 1.0f : 0.0f);

    // 輝度重み付きで混ぜる（明るい点の明滅を抑える）
    const float currentWeight = (1.0f - historyWeight) * LumaWeight(current);
    const float historyWeightL = historyWeight * LumaWeight(history);
    const float3 resolved = (current * currentWeight + history * historyWeightL) / max(currentWeight + historyWeightL, 1.0e-5f);

    float3 rgb = YCoCgToRgb(resolved);
    rgb = all(isfinite(rgb)) ? max(rgb, 0.0f) : currentRgb;
    return float4(rgb, 1.0f);
}
