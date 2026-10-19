Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> Velocity : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float> HistoryDepth : register(t4);
Texture2D<float> Revealage : register(t5);
Texture2D<float2> DetailHistory : register(t6);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float> OutputDepth : register(u1);
RWTexture2D<float2> OutputDetail : register(u2);
SamplerState LinearClamp : register(s0);

cbuffer Constants : register(b0)
{
    float4 Extent;     // 入力 xy、出力 zw。
    float4 Jitter;     // 現在 xy、前回 zw。正規化 UV の +Y は下向き。
    float4 Parameters; // 履歴の有効フラグ、透過残量の有効フラグ、サブピクセル AA、細部の安定性。
    float4x4 InvCurrent, Previous;
    float4 Reconstruction; // 平面の深度、薄い形状、zw は予約領域。
};
#ifndef TSR_USE_SHARED_TILE
#define TSR_USE_SHARED_TILE 1
#endif
#define TSR_TILE_WIDTH 12
#if TSR_USE_SHARED_TILE
groupshared float4 CurrentTileColor[TSR_TILE_WIDTH * TSR_TILE_WIDTH];
groupshared float CurrentTileDepth[TSR_TILE_WIDTH * TSR_TILE_WIDTH];
groupshared int2 CurrentTileOrigin;

int CurrentTileStart(float first, float inputSize)
{
    return clamp((int)floor(first - .5) - 1, 0, (int)inputSize - 1);
}

int CurrentTileEnd(float last, float inputSize)
{
    return clamp((int)floor(last - .5) + 2, 0, (int)inputSize - 1);
}

void PrepareCurrentTile(uint2 group, uint lane)
{
    // 8x8 の出力ピクセルには、バイキュービックと 3x3 の周辺領域を含めて
    // 最大 12x12 の入力テクセルが必要。縮小時は元の直接読み取り経路を維持する。
    if (all(Extent.xy <= Extent.zw))
    {
        float2 first = ((float2(group * 8) + .5) / Extent.zw + Jitter.xy) * Extent.xy;
        float2 last = ((float2(group * 8 + 7) + .5) / Extent.zw + Jitter.xy) * Extent.xy;
        int2 origin = int2(CurrentTileStart(first.x, Extent.x), CurrentTileStart(first.y, Extent.y));
        int2 end = int2(CurrentTileEnd(last.x, Extent.x), CurrentTileEnd(last.y, Extent.y));
        uint2 size = uint2(end - origin + 1);
        if (lane == 0)
        {
            CurrentTileOrigin = origin;
        }
        for (uint index = lane; index < size.x * size.y; index += 64)
        {
            uint2 local = uint2(index % size.x, index / size.x);
            int2 pixel = origin + int2(local);
            uint slot = local.y * TSR_TILE_WIDTH + local.x;
            CurrentTileColor[slot] = Color.Load(int3(pixel, 0));
            CurrentTileDepth[slot] = Depth.Load(int3(pixel, 0));
        }
        // このグループ内で一様なバリアは、不完全なグループの return より前に置く。
        GroupMemoryBarrierWithGroupSync();
    }
}
#endif
float4 LoadCurrentColor(int2 pixel)
{
#if TSR_USE_SHARED_TILE
    if (all(Extent.xy <= Extent.zw))
    {
        int2 local = pixel - CurrentTileOrigin;
        return CurrentTileColor[local.y * TSR_TILE_WIDTH + local.x];
    }
#endif
    return Color.Load(int3(pixel, 0));
}

float LoadCurrentDepth(int2 pixel)
{
#if TSR_USE_SHARED_TILE
    if (all(Extent.xy <= Extent.zw))
    {
        int2 local = pixel - CurrentTileOrigin;
        return CurrentTileDepth[local.y * TSR_TILE_WIDTH + local.x];
    }
#endif
    return Depth.Load(int3(pixel, 0));
}

float3 ToY(float3 c)
{
    return float3(dot(c, float3(.25, .5, .25)), c.r - c.b, c.g - (c.r + c.b) * .5);
}

float3 FromY(float3 c)
{
    return float3(c.x + c.y * .5 - c.z * .5, c.x + c.z * .5, c.x - c.y * .5 - c.z * .5);
}

float Cubic(float x)
{
    x = abs(x);
    if (x < 1)
    {
        return (1.5 * x - 2.5) * x * x + 1;
    }
    if (x < 2)
    {
        return ((-.5 * x + 2.5) * x - 4) * x + 2;
    }
    return 0;
}

float4 Reconstruct(float2 uv)
{
    float2 position = uv * Extent.xy - .5;
    int2 base = int2(floor(position));
    float2 fraction = frac(position);
    float4 sum = 0;
    float weights = 0;
    [unroll] for (int y = -1; y <= 2; ++y)
    {
        [unroll] for (int x = -1; x <= 2; ++x)
        {
            float weight = Cubic(x - fraction.x) * Cubic(y - fraction.y);
            int2 pixel = clamp(base + int2(x, y), 0, int2(Extent.xy) - 1);
            sum += LoadCurrentColor(pixel) * weight;
            weights += weight;
        }
    }
    return max(sum / max(weights, 1e-5), 0);
}

bool HistoryDepthMatches(float2 uv, float expectedDepth, float footprintTolerance)
{
    // 履歴深度はポイントサンプリングする。前景と空を補間すると架空の面が生まれ、
    // 輪郭の AA に必要なサブピクセルの被覆が除外されてしまう。
    // 被覆がジッターに伴って移動するため、入力サンプル 1 個分の投影範囲を覆う。
    const float tolerance = max(max(.0002, abs(1 - expectedDepth) * .01), footprintTolerance);
    const int2 maximumPixel = int2(Extent.zw) - 1;
    const int2 center = clamp(int2(uv * Extent.zw), 0, maximumPixel);
    float difference = abs(HistoryDepth.Load(int3(center, 0)) - expectedDepth);
    [unroll] for (int y = -1; y <= 1; y += 2)
    {
        [unroll] for (int x = -1; x <= 1; x += 2)
        {
            const float2 sampleUv = uv + float2(x, y) / Extent.xy;
            const int2 samplePixel = clamp(int2(sampleUv * Extent.zw), 0, maximumPixel);
            difference = min(difference, abs(HistoryDepth.Load(int3(samplePixel, 0)) - expectedDepth));
        }
    }
    return difference <= tolerance;
}
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
#if TSR_USE_SHARED_TILE
    PrepareCurrentTile(group.xy, lane);
#endif
    if (any(id.xy >= uint2(Extent.zw)))
    {
        return;
    }
    float2 uv = (id.xy + .5) / Extent.zw;
    float2 source = uv + Jitter.xy;
    int2 pixel = clamp(int2(source * Extent.xy), 0, int2(Extent.xy) - 1);
    float4 current = Reconstruct(source);
    float currentLuma = dot(current.rgb, float3(.2126, .7152, .0722));
    float depth = LoadCurrentDepth(pixel);
    OutputDepth[id.xy] = depth;
    OutputDetail[id.xy] = float2(currentLuma, 0);
    // 以下の全ての除外経路で、空間再構築をフォールバックとして保つ。
    Output[id.xy] = current;
    if (Parameters.x < .5)
    {
        return;
    }

    int2 velocityPixel = pixel;
    float nearest = depth;
    float3 minimumColor = 1e20;
    float3 maximumColor = -1e20;
    float3 mean = 0;
    float3 square = 0;
    float4 cardinalLuma = 0; // 3x3 のタップから再利用する左、右、上、下。
    float4 cardinalDepth = depth;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 neighbor = clamp(pixel + int2(x, y), 0, int2(Extent.xy) - 1);
            float neighborDepth = LoadCurrentDepth(neighbor);
            if (x == -1 && y == 0)
            {
                cardinalDepth.x = neighborDepth;
            }
            if (x == 1 && y == 0)
            {
                cardinalDepth.y = neighborDepth;
            }
            if (x == 0 && y == -1)
            {
                cardinalDepth.z = neighborDepth;
            }
            if (x == 0 && y == 1)
            {
                cardinalDepth.w = neighborDepth;
            }
            if (neighborDepth < nearest)
            {
                nearest = neighborDepth;
                velocityPixel = neighbor;
            }
            float3 color = ToY(LoadCurrentColor(neighbor).rgb);
            if (x == -1 && y == 0)
            {
                cardinalLuma.x = color.x;
            }
            if (x == 1 && y == 0)
            {
                cardinalLuma.y = color.x;
            }
            if (x == 0 && y == -1)
            {
                cardinalLuma.z = color.x;
            }
            if (x == 0 && y == 1)
            {
                cardinalLuma.w = color.x;
            }
            minimumColor = min(minimumColor, color);
            maximumColor = max(maximumColor, color);
            mean += color;
            square += color * color;
        }
    }
    mean /= 9;
    float3 sigma = sqrt(max(square / 9 - mean * mean, 0));
    minimumColor = max(minimumColor, mean - 1.5 * sigma);
    maximumColor = min(maximumColor, mean + 1.5 * sigma);

    // 速度には投影ジッターを含め、履歴はジッターなしで保存する。
    float2 motion = Velocity.Load(int3(velocityPixel, 0)).zw;
    float2 oldUv = uv - motion + Jitter.xy - Jitter.zw;
    if (!all(isfinite(motion)) || any(oldUv <= 0) || any(oldUv >= 1))
    {
        return;
    }
    float reactive = Parameters.y > .5 ? 1 - saturate(Revealage.Load(int3(pixel, 0))) : 0;
    float motionPixels = length((motion - Jitter.xy + Jitter.zw) * Extent.zw);
    float separation = max(1e-6, abs(1 - depth) * .02);
    bool thinDepthRidge =
        depth < .99999 && ((cardinalDepth.x > depth + separation && cardinalDepth.y > depth + separation) ||
                           (cardinalDepth.z > depth + separation && cardinalDepth.w > depth + separation));
    bool observedThinGeometry =
        Reconstruction.y > .5 && Parameters.w > .5 && thinDepthRidge && motionPixels < .25 && reactive == 0;
    // 色の履歴を再利用できない、新しく現れたサンプルでも初期値を与える。
    if (observedThinGeometry)
    {
        OutputDetail[id.xy] = float2(currentLuma, 1.8);
    }
    float4 world = mul(float4(source * float2(2, -2) + float2(-1, 1), depth, 1), InvCurrent);
    if (abs(world.w) < 1e-8)
    {
        return;
    }
    world /= world.w;
    float4 previous = mul(world, Previous);
    float expected = previous.z / max(previous.w, 1e-8);
    bool subpixelAA = Parameters.z > .5;
    float footprintTolerance = 0;
    if (Reconstruction.x > .5 && depth < .99999)
    {
        // 透視投影の NDC 深度は平面上でアフィンとなる。対称で連続した
        // 深度勾配だけが入力サンプルの投影範囲を広げられる。
        // 輪郭や幅の狭い前景物体では、厳密な除外を保つ。
        float2 curvature = abs(cardinalDepth.xz + cardinalDepth.yw - 2 * depth);
        float2 slope = .5 * abs(cardinalDepth.yw - cardinalDepth.xz);
        float planarityTolerance = max(2e-6, abs(1 - depth) * .0005);
        bool planar = all(curvature < planarityTolerance) && all(cardinalDepth < .99999);
        if (planar && all(slope < abs(1 - depth) * .1))
        {
            footprintTolerance = slope.x + slope.y + planarityTolerance;
        }
    }
    bool depthMatches;
    if (subpixelAA)
    {
        depthMatches = HistoryDepthMatches(oldUv, expected, footprintTolerance);
    }
    else
    {
        float oldDepth = HistoryDepth.SampleLevel(LinearClamp, oldUv, 0);
        depthMatches = abs(oldDepth - expected) <= max(.0002, abs(1 - expected) * .01);
    }
    if (previous.w <= 0)
    {
        return;
    }
    if (Reconstruction.y > .5 && Parameters.w > .5 && motionPixels < .25 && reactive == 0)
    {
        int2 historyPixel = clamp(int2(oldUv * Extent.zw), 0, int2(Extent.zw) - 1);
        float2 previousDetail = DetailHistory.Load(int3(historyPixel, 0));
        // [-1,1] を超える値は、過去に観測した深度の稜線を示す。
        // 観測がないたびに小数部分の信頼度を減衰させ、速度が同じまま
        // 実際の細い物体が消えた場合に、保持期間を制限する。
        float confidence = max(abs(previousDetail.y) - 1, 0);
        float oldDepth = HistoryDepth.Load(int3(historyPixel, 0));
        if (confidence > .1 && oldDepth < depth - separation && nearest >= oldDepth - separation)
        {
            float4 retained = History.SampleLevel(LinearClamp, oldUv, 0);
            if (all(isfinite(retained)))
            {
                Output[id.xy] = float4(lerp(current.rgb, retained.rgb, .8), current.a);
                OutputDepth[id.xy] = oldDepth;
                OutputDetail[id.xy] = float2(currentLuma, sign(previousDetail.y) * (1 + confidence * .6));
                return;
            }
        }
        // 新しく観測したサブピクセルの稜線は、最大の明るさで再開せず、
        // 以前の背景の上へ被覆を蓄積する必要がある。
        if (observedThinGeometry && oldDepth > depth + separation)
        {
            depthMatches = true;
        }
    }
    if (!depthMatches)
    {
        return;
    }
    float4 history = History.SampleLevel(LinearClamp, oldUv, 0);
    if (!all(isfinite(history)))
    {
        return;
    }
    float detailProtection = 0;
    float detailLock = 0;
    if (subpixelAA && Parameters.w > .5)
    {
        // 符号付きの不安定度に最後の変化方向を保持する。符号反転の反復は
        // サンプリング不足の細部を示し、一方向に続く変化はロックを減衰させる。
        float2 previousDetail = DetailHistory.SampleLevel(LinearClamp, oldUv, 0);
        if (abs(previousDetail.y) > 1)
        {
            previousDetail.y = sign(previousDetail.y) * (abs(previousDetail.y) - 1);
        }
        float change = currentLuma - previousDetail.x;
        float relativeChange = abs(change) / max(max(currentLuma, previousDetail.x), .05);
        float instability = abs(previousDetail.y) * .93;
        float direction = sign(previousDetail.y);
        if (relativeChange > .05)
        {
            float centerLuma = ToY(LoadCurrentColor(pixel).rgb).x;
            float horizontalRidge = (centerLuma - cardinalLuma.x) * (centerLuma - cardinalLuma.y);
            float verticalRidge = (centerLuma - cardinalLuma.z) * (centerLuma - cardinalLuma.w);
            // 局所コントラストを使い、影の中の細線にも明るい細線と同じロックを与える。
            // 下限値により、ほぼ平坦な領域の量子化誤差を増幅しない。
            float contrast = max(maximumColor.x - minimumColor.x, .01);
            float thinFeature = saturate(max(horizontalRidge, verticalRidge) / (contrast * contrast));
            bool reverses = change * previousDetail.y < 0 && abs(previousDetail.y) > .05;
            instability =
                reverses ? min(abs(previousDetail.y) + .45, 1) : max(.2 * thinFeature, abs(previousDetail.y) * .85);
            direction = sign(change);
        }
        float stabilityEligibility = saturate(1 - motionPixels) * (1 - reactive);
        OutputDetail[id.xy] = float2(currentLuma, direction * instability * stabilityEligibility);
        if (observedThinGeometry)
        {
            OutputDetail[id.xy] = float2(currentLuma, (direction == 0 ? 1 : direction) * 1.8);
        }
        // 近傍が平坦な色へ変わるとき、消えかけた細線を保護する。
        // 十分にサンプリングされた輪郭では、元の鮮明な解決を維持する。
        float flatNeighborhood = saturate(1 - sigma.x * 8);
        detailLock = saturate((instability - .15) / .5) * stabilityEligibility;
        detailProtection = detailLock * flatNeighborhood;
    }
    // ジッター付きの輪郭と蓄積した被覆が異なること自体は正常。
    // 現在の近傍色の範囲外だけを、説明できない変化と見なす。
    bool stablePlane = subpixelAA && Parameters.w > .5 && footprintTolerance > 0 && motionPixels < .25 &&
                       reactive == 0 && sigma.x < max(abs(mean.x), .05) * .15;
    if (stablePlane)
    {
        // 平坦な近傍には空間的な分散がほとんどない。その幅 0 の範囲へクリップすると、
        // 弱い GI ノイズが蓄積されず、そのまま出力されてしまう。
        // 検証済みの静止平面では、小さな絶対・相対許容差を認める。
        // 大きな照明変化は、引き続き現在フレームの範囲で制限する。
        float3 noiseAllowance = float3(.001, .002, .002) + .003 * abs(mean);
        minimumColor -= noiseAllowance;
        maximumColor += noiseAllowance;
    }
    float3 clippedHistory = clamp(ToY(history.rgb), minimumColor, maximumColor);
    clippedHistory = lerp(clippedHistory, ToY(history.rgb), detailProtection);
    float neighborhoodReaction = saturate(abs(ToY(history.rgb).x - clippedHistory.x) / max(abs(clippedHistory.x), .05));
    float historyLuma = dot(history.rgb, float3(.2126, .7152, .0722));
    float legacyReaction = saturate(abs(currentLuma - historyLuma) / max(max(currentLuma, historyLuma), .05));
    reactive = max(reactive, subpixelAA ? neighborhoodReaction : legacyReaction);
    float stillness = subpixelAA ? exp2(-motionPixels * .25) * (1 - reactive) : 0;
    // 入力解像度が低い場合、元のサンプルが出力ピクセル数個分離れることがある。
    // 毎フレーム 5 倍に拡大したキュービックの輪郭を平均せず、実際の位置を蓄積する。
    // 移動したピクセルと除外したピクセルでは、空間再構築を維持する。
    float2 sampleDistance = ((float2(pixel) + .5) / Extent.xy - source) * Extent.zw;
    float sampleConfidence = exp2(-.5 * dot(sampleDistance, sampleDistance));
    float currentWeight = lerp(.08, .002 + .1 * sampleConfidence, stillness);
    if (stablePlane)
    {
        currentWeight *= .5;
    }
    currentWeight *= lerp(1, .25, detailLock);
    float weight = (1 - currentWeight) * (1 - reactive) * exp2(-motionPixels * .04);
    float3 temporalSample = lerp(current.rgb, LoadCurrentColor(pixel).rgb, stillness);
    Output[id.xy] = float4(lerp(temporalSample, FromY(clippedHistory), weight), current.a);
}
