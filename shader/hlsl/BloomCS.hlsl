// =============================================================================================
// BloomCS.hlsl  -  physically based bloom (dual filter mip chain), lens flare and screen space
// rain / snow. Runs on the linear HDR scene inside AdvancedEffects (before tone mapping).
//   BloomDownsample : 13 tap filter (Jimenez 2014, CoD:AW); level 0 adds a soft threshold and
//                     the Karis average to suppress fireflies.
//   BloomUpsample   : 3x3 tent, up[i] = down[i] + tent(up[i+1]) * radius
//   BloomCombine    : energy conserving blend + lens flare (Chapman ghosts / halo) + weather.
// =============================================================================================
cbuffer BloomConstants : register(b0)
{
    uint2 DestinationSize;
    uint2 SourceSize;
    float Threshold;      // 0 = no threshold
    float Radius;         // upsample scatter
    float Intensity;      // final blend
    float Normalization;  // 1 / (1 + r + r^2 + ...)
    float FlareIntensity;
    float FlareHalo;
    float RainIntensity;
    float SnowIntensity;
    float Time;
    float WindX;
    uint FirstLevel;
    uint Flags;           // bit0 bloom, bit1 flare, bit2 rain, bit3 snow
};

Texture2D<float4> Source0 : register(t0);
Texture2D<float4> Source1 : register(t1);
Texture2D<float4> Source2 : register(t2);
Texture2D<float> SceneDepth : register(t3);
RWTexture2D<float4> Destination : register(u0);
SamplerState LinearClamp : register(s0);

static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

float KarisWeight(float3 color)
{
    return 1.0f / (1.0f + dot(color, kLuma));
}

float3 SoftThreshold(float3 color)
{
    if (Threshold <= 0.0f) return color;
    float brightness = max(color.r, max(color.g, color.b));
    float knee = Threshold * 0.5f;
    float soft = clamp(brightness - Threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 0.00001f);
    float contribution = max(soft, brightness - Threshold) / max(brightness, 0.00001f);
    return color * contribution;
}

[numthreads(8, 8, 1)]
void BloomDownsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= DestinationSize)) return;
    float2 uv = (id.xy + 0.5f) / DestinationSize;
    float2 t = 1.0f / SourceSize;
    float3 a = Source0.SampleLevel(LinearClamp, uv + t * float2(-2, -2), 0).rgb;
    float3 b = Source0.SampleLevel(LinearClamp, uv + t * float2(0, -2), 0).rgb;
    float3 c = Source0.SampleLevel(LinearClamp, uv + t * float2(2, -2), 0).rgb;
    float3 d = Source0.SampleLevel(LinearClamp, uv + t * float2(-2, 0), 0).rgb;
    float3 e = Source0.SampleLevel(LinearClamp, uv, 0).rgb;
    float3 f = Source0.SampleLevel(LinearClamp, uv + t * float2(2, 0), 0).rgb;
    float3 g = Source0.SampleLevel(LinearClamp, uv + t * float2(-2, 2), 0).rgb;
    float3 h = Source0.SampleLevel(LinearClamp, uv + t * float2(0, 2), 0).rgb;
    float3 i = Source0.SampleLevel(LinearClamp, uv + t * float2(2, 2), 0).rgb;
    float3 j = Source0.SampleLevel(LinearClamp, uv + t * float2(-1, -1), 0).rgb;
    float3 k = Source0.SampleLevel(LinearClamp, uv + t * float2(1, -1), 0).rgb;
    float3 l = Source0.SampleLevel(LinearClamp, uv + t * float2(-1, 1), 0).rgb;
    float3 m = Source0.SampleLevel(LinearClamp, uv + t * float2(1, 1), 0).rgb;
    float3 result;
    if (FirstLevel != 0)
    {
        // Karis average per 2x2 group removes single pixel fireflies from the whole chain.
        float3 g0 = (j + k + l + m) * 0.25f;
        float3 g1 = (a + b + d + e) * 0.25f;
        float3 g2 = (b + c + e + f) * 0.25f;
        float3 g3 = (d + e + g + h) * 0.25f;
        float3 g4 = (e + f + h + i) * 0.25f;
        float w0 = KarisWeight(g0) * 0.5f, w1 = KarisWeight(g1) * 0.125f, w2 = KarisWeight(g2) * 0.125f;
        float w3 = KarisWeight(g3) * 0.125f, w4 = KarisWeight(g4) * 0.125f;
        result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / max(w0 + w1 + w2 + w3 + w4, 0.0001f);
        result = SoftThreshold(result);
    }
    else
    {
        result = e * 0.125f + (a + c + g + i) * 0.03125f + (b + d + f + h) * 0.0625f + (j + k + l + m) * 0.125f;
    }
    Destination[id.xy] = float4(max(result, 0.0f), 1.0f);
}

[numthreads(8, 8, 1)]
void BloomUpsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= DestinationSize)) return;
    float2 uv = (id.xy + 0.5f) / DestinationSize;
    float2 t = 1.0f / SourceSize; // size of the lower (coarser) level
    float3 tent = Source1.SampleLevel(LinearClamp, uv, 0).rgb * 4.0f;
    tent += (Source1.SampleLevel(LinearClamp, uv + t * float2(-1, 0), 0).rgb + Source1.SampleLevel(LinearClamp, uv + t * float2(1, 0), 0).rgb +
        Source1.SampleLevel(LinearClamp, uv + t * float2(0, -1), 0).rgb + Source1.SampleLevel(LinearClamp, uv + t * float2(0, 1), 0).rgb) * 2.0f;
    tent += Source1.SampleLevel(LinearClamp, uv + t * float2(-1, -1), 0).rgb + Source1.SampleLevel(LinearClamp, uv + t * float2(1, -1), 0).rgb +
        Source1.SampleLevel(LinearClamp, uv + t * float2(-1, 1), 0).rgb + Source1.SampleLevel(LinearClamp, uv + t * float2(1, 1), 0).rgb;
    tent /= 16.0f;
    float3 current = Source0.SampleLevel(LinearClamp, uv, 0).rgb;
    Destination[id.xy] = float4(current + tent * Radius, 1.0f);
}

float Hash21(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float3 LensFlare(float2 uv)
{
    float2 flipped = 1.0f - uv;
    float2 ghostVector = (0.5f - flipped) * 0.37f;
    float3 result = 0.0f;
    [unroll] for (int ghost = 0; ghost < 5; ++ghost)
    {
        float2 offset = frac(flipped + ghostVector * ghost);
        float weight = pow(saturate(1.0f - length(0.5f - offset) / 0.7071f), 8.0f);
        // Chromatic distortion: each channel slightly displaced along the ghost axis.
        float2 shift = normalize(ghostVector + 0.00001f) * 0.006f;
        float3 sample = float3(
            Source2.SampleLevel(LinearClamp, offset + shift, 0).r,
            Source2.SampleLevel(LinearClamp, offset, 0).g,
            Source2.SampleLevel(LinearClamp, offset - shift, 0).b);
        result += max(sample - 1.0f, 0.0f) * weight * (ghost == 0 ? 0.5f : 1.0f);
    }
    float2 haloVector = normalize(ghostVector + 0.00001f) * 0.42f;
    float2 haloUv = frac(flipped + haloVector);
    float haloWeight = pow(saturate(1.0f - length(0.5f - haloUv) / 0.7071f), 5.0f);
    result += max(Source2.SampleLevel(LinearClamp, haloUv, 0).rgb - 1.0f, 0.0f) * haloWeight * FlareHalo * 4.0f;
    // Spectral tint (warm centre, cool edges) like coated optics.
    float radial = length(uv - 0.5f) * 1.4f;
    return result * lerp(float3(1.0f, 0.85f, 0.7f), float3(0.6f, 0.75f, 1.0f), saturate(radial));
}

// Falling streaks in three parallax layers; brighter where the scene is lit.
float RainLayer(float2 uv, float scale, float speed, float slant, float seed)
{
    float2 p = float2(uv.x * scale + uv.y * slant * scale, uv.y * scale * 0.12f - Time * speed);
    float2 cell = floor(p);
    float2 local = frac(p);
    float random = Hash21(cell + seed);
    if (random > 0.35f) return 0.0f;
    float x = abs(local.x - frac(0.2f + random * 1.6f));
    float streak = saturate(1.0f - x * scale * 0.9f);
    float y = frac(local.y + random * 7.0f);
    float lengthMask = smoothstep(0.0f, 0.15f, y) * smoothstep(0.75f, 0.35f, y);
    return streak * streak * lengthMask;
}

float SnowLayer(float2 uv, float scale, float speed, float seed, float aspect)
{
    float2 p = float2(uv.x * aspect, uv.y) * scale;
    p.y -= Time * speed;
    p.x += sin(Time * 0.7f + p.y * 0.8f + seed) * 0.35f + Time * WindX * 0.5f;
    float2 cell = floor(p);
    float2 local = frac(p) - 0.5f;
    float random = Hash21(cell + seed);
    if (random > 0.45f) return 0.0f;
    float2 center = (float2(Hash21(cell + 3.1f + seed), Hash21(cell + 7.7f + seed)) - 0.5f) * 0.6f;
    float radius = 0.06f + random * 0.08f;
    return smoothstep(radius, radius * 0.4f, length(local - center));
}

[numthreads(8, 8, 1)]
void BloomCombine(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= DestinationSize)) return;
    float2 uv = (id.xy + 0.5f) / DestinationSize;
    float3 color = Source0[id.xy].rgb;
    float3 bloom = 0.0f;
    if ((Flags & 1u) != 0)
    {
        bloom = Source1.SampleLevel(LinearClamp, uv, 0).rgb * Normalization;
        // Energy conserving: the bloom redistributes light instead of adding it.
        color = lerp(color, bloom, Intensity);
    }
    if ((Flags & 2u) != 0)
    {
        color += LensFlare(uv) * FlareIntensity * 0.05f;
    }
    float ambient = dot(bloom, kLuma) + 0.02f;
    if ((Flags & 4u) != 0)
    {
        float aspect = DestinationSize.x / (float)DestinationSize.y;
        float2 rainUv = float2(uv.x * aspect, uv.y);
        float rain = RainLayer(rainUv, 70.0f, 2.6f, WindX * 0.25f, 1.0f) * 0.5f +
            RainLayer(rainUv, 45.0f, 2.0f, WindX * 0.22f, 2.0f) * 0.35f +
            RainLayer(rainUv, 28.0f, 1.5f, WindX * 0.2f, 3.0f) * 0.25f;
        color += rain * RainIntensity * (0.03f + ambient * 0.6f);
    }
    if ((Flags & 8u) != 0)
    {
        float aspect = DestinationSize.x / (float)DestinationSize.y;
        float snow = SnowLayer(uv, 26.0f, 0.10f, 1.0f, aspect) * 0.6f + SnowLayer(uv, 14.0f, 0.06f, 5.0f, aspect) +
            SnowLayer(uv, 8.0f, 0.04f, 9.0f, aspect) * 1.2f;
        color = lerp(color, max(color, (0.35f + ambient * 2.0f).xxx), saturate(snow * SnowIntensity));
    }
    Destination[id.xy] = float4(color, 1.0f);
}
