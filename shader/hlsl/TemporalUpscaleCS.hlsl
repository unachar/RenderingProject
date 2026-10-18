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
    float4 Extent; // Input xy, output zw.
    float4 Jitter; // Current xy, previous zw, normalized UV with +Y down.
    float4 Parameters; // History valid, revealage valid, subpixel AA, detail stability.
    float4x4 InvCurrent, Previous;
    float4 Reconstruction; // Planar depth, thin geometry, reserved zw.
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
    // 8x8 output pixels need at most 12x12 input texels, including the
    // bicubic and 3x3 halos. Downscaling keeps the original direct-load path.
    if (all(Extent.xy <= Extent.zw))
    {
        float2 first = ((float2(group * 8) + .5) / Extent.zw + Jitter.xy) * Extent.xy;
        float2 last = ((float2(group * 8 + 7) + .5) / Extent.zw + Jitter.xy) * Extent.xy;
        int2 origin = int2(CurrentTileStart(first.x, Extent.x), CurrentTileStart(first.y, Extent.y));
        int2 end = int2(CurrentTileEnd(last.x, Extent.x), CurrentTileEnd(last.y, Extent.y));
        uint2 size = uint2(end - origin + 1);
        if (lane == 0) CurrentTileOrigin = origin;
        for (uint index = lane; index < size.x * size.y; index += 64)
        {
            uint2 local = uint2(index % size.x, index / size.x);
            int2 pixel = origin + int2(local);
            uint slot = local.y * TSR_TILE_WIDTH + local.x;
            CurrentTileColor[slot] = Color.Load(int3(pixel, 0));
            CurrentTileDepth[slot] = Depth.Load(int3(pixel, 0));
        }
        // This group-uniform barrier must precede the partial-group return.
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
float3 ToY(float3 c) { return float3(dot(c,float3(.25,.5,.25)),c.r-c.b,c.g-(c.r+c.b)*.5); }
float3 FromY(float3 c) { return float3(c.x+c.y*.5-c.z*.5,c.x+c.z*.5,c.x-c.y*.5-c.z*.5); }
float Cubic(float x) { x=abs(x); return x<1?(1.5*x-2.5)*x*x+1:x<2?((-.5*x+2.5)*x-4)*x+2:0; }
float4 Reconstruct(float2 uv)
{
    float2 position = uv * Extent.xy - .5;
    int2 base = int2(floor(position));
    float2 fraction = frac(position);
    float4 sum = 0;
    float weights = 0;
    [unroll] for (int y = -1; y <= 2; ++y)
    [unroll] for (int x = -1; x <= 2; ++x)
    {
        float weight = Cubic(x - fraction.x) * Cubic(y - fraction.y);
        int2 pixel = clamp(base + int2(x, y), 0, int2(Extent.xy) - 1);
        sum += LoadCurrentColor(pixel) * weight;
        weights += weight;
    }
    return max(sum / max(weights, 1e-5), 0);
}
bool HistoryDepthMatches(float2 uv, float expectedDepth, float footprintTolerance)
{
    // History depth is point sampled: filtering foreground and sky creates a
    // fictitious surface and rejects the subpixel coverage needed for edge AA.
    // Cover one input sample's footprint because its coverage moves with jitter.
    const float tolerance = max(max(.0002, abs(1 - expectedDepth) * .01), footprintTolerance);
    const int2 maximumPixel = int2(Extent.zw) - 1;
    const int2 center = clamp(int2(uv * Extent.zw), 0, maximumPixel);
    float difference = abs(HistoryDepth.Load(int3(center, 0)) - expectedDepth);
    [unroll] for (int y = -1; y <= 1; y += 2)
    [unroll] for (int x = -1; x <= 1; x += 2)
    {
        const float2 sampleUv = uv + float2(x, y) / Extent.xy;
        const int2 samplePixel = clamp(int2(sampleUv * Extent.zw), 0, maximumPixel);
        difference = min(difference, abs(HistoryDepth.Load(int3(samplePixel, 0)) - expectedDepth));
    }
    return difference <= tolerance;
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
#if TSR_USE_SHARED_TILE
    PrepareCurrentTile(group.xy, lane);
#endif
    if (any(id.xy >= uint2(Extent.zw))) return;
    float2 uv = (id.xy + .5) / Extent.zw;
    float2 source = uv + Jitter.xy;
    int2 pixel = clamp(int2(source * Extent.xy), 0, int2(Extent.xy) - 1);
    float4 current = Reconstruct(source);
    float currentLuma = dot(current.rgb, float3(.2126, .7152, .0722));
    float depth = LoadCurrentDepth(pixel);
    OutputDepth[id.xy] = depth;
    OutputDetail[id.xy] = float2(currentLuma, 0);
    // Every rejection path below keeps spatial reconstruction as its fallback.
    Output[id.xy] = current;
    if (Parameters.x < .5) return;

    int2 velocityPixel = pixel;
    float nearest = depth;
    float3 minimumColor = 1e20, maximumColor = -1e20, mean = 0, square = 0;
    float4 cardinalLuma = 0; // Left, right, up, down, reused from the 3x3 taps.
    float4 cardinalDepth = depth;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 neighbor = clamp(pixel + int2(x, y), 0, int2(Extent.xy) - 1);
        float neighborDepth = LoadCurrentDepth(neighbor);
        if (x == -1 && y == 0) cardinalDepth.x = neighborDepth;
        if (x == 1 && y == 0) cardinalDepth.y = neighborDepth;
        if (x == 0 && y == -1) cardinalDepth.z = neighborDepth;
        if (x == 0 && y == 1) cardinalDepth.w = neighborDepth;
        if (neighborDepth < nearest)
        {
            nearest = neighborDepth;
            velocityPixel = neighbor;
        }
        float3 color = ToY(LoadCurrentColor(neighbor).rgb);
        if (x == -1 && y == 0) cardinalLuma.x = color.x;
        if (x == 1 && y == 0) cardinalLuma.y = color.x;
        if (x == 0 && y == -1) cardinalLuma.z = color.x;
        if (x == 0 && y == 1) cardinalLuma.w = color.x;
        minimumColor = min(minimumColor, color);
        maximumColor = max(maximumColor, color);
        mean += color;
        square += color * color;
    }
    mean /= 9;
    float3 sigma = sqrt(max(square / 9 - mean * mean, 0));
    minimumColor = max(minimumColor, mean - 1.5 * sigma);
    maximumColor = min(maximumColor, mean + 1.5 * sigma);

    // Velocity includes projection jitter; history is stored without jitter.
    float2 motion = Velocity.Load(int3(velocityPixel, 0)).zw;
    float2 oldUv = uv - motion + Jitter.xy - Jitter.zw;
    if (!all(isfinite(motion)) || any(oldUv <= 0) || any(oldUv >= 1)) return;
    float reactive = Parameters.y > .5 ? 1 - saturate(Revealage.Load(int3(pixel, 0))) : 0;
    float motionPixels = length((motion - Jitter.xy + Jitter.zw) * Extent.zw);
    float separation = max(1e-6, abs(1 - depth) * .02);
    bool thinDepthRidge = depth < .99999 &&
        ((cardinalDepth.x > depth + separation && cardinalDepth.y > depth + separation) ||
         (cardinalDepth.z > depth + separation && cardinalDepth.w > depth + separation));
    bool observedThinGeometry = Reconstruction.y > .5 && Parameters.w > .5 &&
        thinDepthRidge && motionPixels < .25 && reactive == 0;
    // Seed even on a newly revealed sample whose color history cannot be reused.
    if (observedThinGeometry) OutputDetail[id.xy] = float2(currentLuma, 1.8);
    float4 world = mul(float4(source * float2(2, -2) + float2(-1, 1), depth, 1), InvCurrent);
    if (abs(world.w) < 1e-8) return;
    world /= world.w;
    float4 previous = mul(world, Previous);
    float expected = previous.z / max(previous.w, 1e-8);
    bool subpixelAA = Parameters.z > .5;
    float footprintTolerance = 0;
    if (Reconstruction.x > .5 && depth < .99999)
    {
        // Perspective NDC depth is affine over a planar surface. Only symmetric,
        // continuous depth gradients may expand the input sample's footprint;
        // silhouettes and narrow foreground objects retain strict rejection.
        float2 curvature = abs(cardinalDepth.xz + cardinalDepth.yw - 2 * depth);
        float2 slope = .5 * abs(cardinalDepth.yw - cardinalDepth.xz);
        float planarityTolerance = max(2e-6, abs(1 - depth) * .0005);
        bool planar = all(curvature < planarityTolerance) && all(cardinalDepth < .99999);
        if (planar && all(slope < abs(1 - depth) * .1))
            footprintTolerance = slope.x + slope.y + planarityTolerance;
    }
    bool depthMatches;
    if (subpixelAA)
        depthMatches = HistoryDepthMatches(oldUv, expected, footprintTolerance);
    else
    {
        float oldDepth = HistoryDepth.SampleLevel(LinearClamp, oldUv, 0);
        depthMatches = abs(oldDepth - expected) <= max(.0002, abs(1 - expected) * .01);
    }
    if (previous.w <= 0) return;
    if (Reconstruction.y > .5 && Parameters.w > .5 && motionPixels < .25 && reactive == 0)
    {
        int2 historyPixel = clamp(int2(oldUv * Extent.zw), 0, int2(Extent.zw) - 1);
        float2 previousDetail = DetailHistory.Load(int3(historyPixel, 0));
        // Values beyond [-1,1] tag a previously observed depth ridge. The
        // fractional confidence decays on every missing observation, bounding
        // retention when a real thin object disappears at unchanged velocity.
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
        // A newly observed subpixel ridge needs to accumulate coverage over
        // the previous background instead of restarting at full brightness.
        if (observedThinGeometry && oldDepth > depth + separation) depthMatches = true;
    }
    if (!depthMatches) return;
    float4 history = History.SampleLevel(LinearClamp, oldUv, 0);
    if (!all(isfinite(history))) return;
    float detailProtection = 0;
    float detailLock = 0;
    if (subpixelAA && Parameters.w > .5)
    {
        // Signed instability retains the last change direction. Repeated sign
        // reversals identify undersampled detail; persistent changes decay the lock.
        float2 previousDetail = DetailHistory.SampleLevel(LinearClamp, oldUv, 0);
        if (abs(previousDetail.y) > 1)
            previousDetail.y = sign(previousDetail.y) * (abs(previousDetail.y) - 1);
        float change = currentLuma - previousDetail.x;
        float relativeChange = abs(change) / max(max(currentLuma, previousDetail.x), .05);
        float instability = abs(previousDetail.y) * .93;
        float direction = sign(previousDetail.y);
        if (relativeChange > .05)
        {
            float centerLuma = ToY(LoadCurrentColor(pixel).rgb).x;
            float horizontalRidge = (centerLuma - cardinalLuma.x) * (centerLuma - cardinalLuma.y);
            float verticalRidge = (centerLuma - cardinalLuma.z) * (centerLuma - cardinalLuma.w);
            // Use local contrast so a strand in shadow can seed the same lock as
            // a bright strand. The floor avoids amplifying near-flat quantization.
            float contrast = max(maximumColor.x - minimumColor.x, .01);
            float thinFeature = saturate(max(horizontalRidge, verticalRidge) / (contrast * contrast));
            bool reverses = change * previousDetail.y < 0 && abs(previousDetail.y) > .05;
            instability = reverses ? min(abs(previousDetail.y) + .45, 1) : max(.2 * thinFeature, abs(previousDetail.y) * .85);
            direction = sign(change);
        }
        float stabilityEligibility = saturate(1 - motionPixels) * (1 - reactive);
        OutputDetail[id.xy] = float2(currentLuma, direction * instability * stabilityEligibility);
        if (observedThinGeometry)
            OutputDetail[id.xy] = float2(currentLuma, (direction == 0 ? 1 : direction) * 1.8);
        // Protect a disappearing strand when its neighborhood collapses to a
        // flat color. A well-sampled silhouette keeps the original sharp resolve.
        float flatNeighborhood = saturate(1 - sigma.x * 8);
        detailLock = saturate((instability - .15) / .5) * stabilityEligibility;
        detailProtection = detailLock * flatNeighborhood;
    }
    // A jittered silhouette legitimately differs from the accumulated coverage.
    // Only color outside the current neighborhood signals an unexplained change.
    bool stablePlane = subpixelAA && Parameters.w > .5 && footprintTolerance > 0 &&
        motionPixels < .25 && reactive == 0 && sigma.x < max(abs(mean.x), .05) * .15;
    if (stablePlane)
    {
        // A flat neighborhood has almost no spatial variance. Clipping to that
        // zero-width box forwards weak GI noise instead of accumulating it.
        // Allow a small absolute/relative tolerance on verified stationary planes.
        // Large lighting changes remain bounded by the current frame's box.
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
    // At low input resolution, a source sample can be several output pixels
    // away. Accumulate its actual location instead of averaging a 5x magnified
    // cubic edge every frame. Moving/rejected pixels retain spatial reconstruction.
    float2 sampleDistance = ((float2(pixel) + .5) / Extent.xy - source) * Extent.zw;
    float sampleConfidence = exp2(-.5 * dot(sampleDistance, sampleDistance));
    float currentWeight = lerp(.08, .002 + .1 * sampleConfidence, stillness);
    if (stablePlane) currentWeight *= .5;
    currentWeight *= lerp(1, .25, detailLock);
    float weight = (1 - currentWeight) * (1 - reactive) * exp2(-motionPixels * .04);
    float3 temporalSample = lerp(current.rgb, LoadCurrentColor(pixel).rgb, stillness);
    Output[id.xy] = float4(lerp(temporalSample, FromY(clippedHistory), weight), current.a);
}

