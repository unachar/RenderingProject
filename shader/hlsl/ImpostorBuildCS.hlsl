// インポスターアトラスの後処理（エントリ：DilateCS / DownsampleCS）。ImpostorRenderer が実行時にコンパイルする。
// DilateCS     : ベイク結果 -> アトラス mip0。空き texel をフレーム内の最も近い被覆 texel で埋める（alpha は 0 のまま）。
//                バイリニア / mip で背景の黒や不定の深度が滲み込むのを防ぐ。
// DownsampleCS : mip k -> mip k+1。被覆率で重み付けした平均（色は線形空間で平均し sRGB で格納）。
#include "ImpostorMath.hlsli"

cbuffer ImpostorBuildConstants : register(b0)
{
    uint DestinationSize; // 出力 mip の1辺の texel 数
    uint FrameSize;       // 出力 mip での1フレームの texel 数
    uint DilateRadius;
    uint BuildPadding;
};

Texture2D<float4> SourceAlbedo : register(t0); // sRGB ビュー（読み出し値は線形）
Texture2D<float4> SourceNormal : register(t1);
RWTexture2D<unorm float4> DestinationAlbedo : register(u0); // UNORM ビュー（sRGB 符号化して書く）
RWTexture2D<unorm float4> DestinationNormal : register(u1);

[numthreads(8, 8, 1)] void DilateCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DestinationSize || id.y >= DestinationSize)
    {
        return;
    }
    float4 albedo = SourceAlbedo.Load(int3(id.xy, 0));
    float4 normal = SourceNormal.Load(int3(id.xy, 0));
    if (albedo.a <= 0.0f)
    {
        const int2 frameOrigin = int2((id.xy / FrameSize) * FrameSize);
        const int2 frameEnd = frameOrigin + int2(FrameSize, FrameSize);
        const int radius = int(DilateRadius);
        float bestDistance = 1e30f;
        float3 bestColor = albedo.rgb;
        float4 bestNormal = normal;
        for (int y = -radius; y <= radius; ++y)
        {
            for (int x = -radius; x <= radius; ++x)
            {
                int2 p = int2(id.xy) + int2(x, y);
                if (any(p < frameOrigin) || any(p >= frameEnd))
                {
                    continue;
                }
                float4 candidate = SourceAlbedo.Load(int3(p, 0));
                float distanceSq = float(x * x + y * y);
                if (candidate.a > 0.0f && distanceSq < bestDistance)
                {
                    bestDistance = distanceSq;
                    bestColor = candidate.rgb;
                    bestNormal = SourceNormal.Load(int3(p, 0));
                }
            }
        }
        albedo.rgb = bestColor;
        normal = bestNormal;
        albedo.a = 0.0f;
    }
    DestinationAlbedo[id.xy] = float4(ImpostorLinearToSrgb(albedo.rgb), albedo.a);
    DestinationNormal[id.xy] = normal;
}

[numthreads(8, 8, 1)] void DownsampleCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DestinationSize || id.y >= DestinationSize)
    {
        return;
    }
    const int2 source = int2(id.xy) * 2;
    float4 albedo[4];
    float4 normalDepth[4];
    [unroll] for (int i = 0; i < 4; ++i)
    {
        int2 p = source + int2(i & 1, i >> 1);
        albedo[i] = SourceAlbedo.Load(int3(p, 0));
        normalDepth[i] = SourceNormal.Load(int3(p, 0));
    }

    float coverageSum = albedo[0].a + albedo[1].a + albedo[2].a + albedo[3].a;
    float3 colorSum = 0.0f;
    float3 normalSum = 0.0f;
    float depthSum = 0.0f;
    float weightSum = 0.0f;
    float bestWeight = -1.0f;
    float part = normalDepth[0].a;
    [unroll] for (int j = 0; j < 4; ++j)
    {
        // 被覆 texel があればそれだけで平均、全て空なら（膨張済みの値を）単純平均する。
        float weight = coverageSum > 1e-4f ? albedo[j].a : 1.0f;
        colorSum += albedo[j].rgb * weight;
        normalSum += ImpostorOctDecode(normalDepth[j].rg * 2.0f - 1.0f) * weight;
        depthSum += normalDepth[j].b * weight;
        weightSum += weight;
        if (albedo[j].a > bestWeight)
        {
            bestWeight = albedo[j].a;
            part = normalDepth[j].a;
        }
    }
    float inverseWeight = 1.0f / max(weightSum, 1e-6f);
    float normalLengthSq = dot(normalSum, normalSum);
    float3 normal = normalLengthSq > 1e-10f ? normalSum * rsqrt(normalLengthSq) : float3(0.0f, 1.0f, 0.0f);

    DestinationAlbedo[id.xy] = float4(ImpostorLinearToSrgb(colorSum * inverseWeight), coverageSum * 0.25f);
    DestinationNormal[id.xy] = float4(ImpostorOctEncode(normal) * 0.5f + 0.5f, depthSum * inverseWeight, part);
}
