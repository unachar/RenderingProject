#ifndef TONE_MAPPING_HLSLI
#define TONE_MAPPING_HLSLI
// Requires PostProcessParams (GradeParams0/1, GradeLift/Gamma/Gain) from common.hlsl.
// All functions take and return linear Rec.709 values.

static const float3 kLumaWeights = float3(0.2126f, 0.7152f, 0.0722f);

// --- AgX (Troy Sobotka), polynomial fit of the default contrast curve ------------------------
float3 AgXContrastApprox(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

float3 AgXToneMap(float3 color, bool punchy)
{
    // Inset matrix (columns).
    const float3 c0 = float3(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f);
    const float3 c1 = float3(0.0784335999999992f, 0.878468636469772f, 0.0784336f);
    const float3 c2 = float3(0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
    const float minEv = -12.47393f;
    const float maxEv = 4.026069f;
    color = c0 * color.r + c1 * color.g + c2 * color.b;
    color = clamp(log2(max(color, 1e-10f)), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    color = AgXContrastApprox(color);
    if (punchy)
    {
        color = pow(max(color, 0.0f), 1.35f);
        float luma = dot(color, kLumaWeights);
        color = luma + 1.4f * (color - luma);
    }
    // Outset matrix (columns) back to display encoded Rec.709.
    const float3 d0 = float3(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f);
    const float3 d1 = float3(-0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f);
    const float3 d2 = float3(-0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f);
    color = d0 * color.r + d1 * color.g + d2 * color.b;
    // The curve targets a 2.2 display; return linear light for the existing output path.
    return pow(saturate(color), 2.2f);
}

// --- ACES fitted (Stephen Hill): RRT + ODT with the sRGB input / output matrices --------------
float3 AcesFittedToneMap(float3 color)
{
    const float3x3 inputMatrix = float3x3(
        0.59719f, 0.35458f, 0.04823f,
        0.07600f, 0.90834f, 0.01566f,
        0.02840f, 0.13383f, 0.83777f);
    const float3x3 outputMatrix = float3x3(
        1.60475f, -0.53108f, -0.07367f,
        -0.10208f, 1.10813f, -0.00605f,
        -0.00327f, -0.07276f, 1.07602f);
    color = mul(inputMatrix, color);
    float3 a = color * (color + 0.0245786f) - 0.000090537f;
    float3 b = color * (0.983729f * color + 0.4329510f) + 0.238081f;
    color = a / b;
    return saturate(mul(outputMatrix, color));
}

// --- Khronos PBR Neutral: preserves base colour hue and saturation (product visualisation) ----
float3 PbrNeutralToneMap(float3 color)
{
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return saturate(color);
    float d = 1.0f - startCompression;
    float newPeak = 1.0f - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return saturate(lerp(color, newPeak.xxx, g));
}

float3 ApplyToneMapper(float3 color)
{
    int mode = (int)round(GradeParams0.x);
    if (mode == 1) return AgXToneMap(color, GradeParams1.z > 0.5f);
    if (mode == 2) return AcesFittedToneMap(color);
    if (mode == 3) return PbrNeutralToneMap(color);
    return ACESToneMap(color);
}

// Scene-referred grading (before the tone mapper): white balance, contrast, saturation, vibrance.
float3 GradeSceneLinear(float3 color)
{
    float temperature = GradeParams1.x;
    float tint = GradeParams1.y;
    // Simple white balance: warm/cool along blue-yellow, tint along green-magenta.
    float3 balance = float3(1.0f + temperature * 0.10f, 1.0f - tint * 0.08f, 1.0f - temperature * 0.10f);
    color *= balance / dot(balance, kLumaWeights);
    float contrast = max(GradeParams0.y, 0.0f);
    if (abs(contrast - 1.0f) > 0.001f)
    {
        // Contrast around middle grey in log space keeps highlights and blacks stable.
        color = 0.18f * pow(max(color, 1e-6f) / 0.18f, contrast);
    }
    float luma = dot(color, kLumaWeights);
    float saturation = max(GradeParams0.z, 0.0f);
    color = max(luma + (color - luma) * saturation, 0.0f);
    float vibrance = GradeParams0.w;
    if (abs(vibrance) > 0.001f)
    {
        float maxChannel = max(color.r, max(color.g, color.b));
        float minChannel = min(color.r, min(color.g, color.b));
        float currentSaturation = (maxChannel - minChannel) / max(maxChannel, 1e-5f);
        luma = dot(color, kLumaWeights);
        color = max(luma + (color - luma) * (1.0f + vibrance * (1.0f - currentSaturation)), 0.0f);
    }
    return color;
}

// Display-referred lift / gamma / gain (ASC CDL style) on the tone mapped image.
float3 GradeDisplay(float3 color)
{
    float3 encoded = pow(saturate(color), 1.0f / 2.2f);
    encoded = GradeGain.rgb * (encoded + GradeLift.rgb * (1.0f - encoded));
    encoded = pow(saturate(encoded), 1.0f / max(GradeGamma.rgb, 0.05f));
    return pow(encoded, 2.2f);
}

// --- Lens effects --------------------------------------------------------------------------------
float LensHash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

// Lateral chromatic aberration: red and blue sampled at radially scaled positions.
float3 SampleWithChromaticAberration(Texture2D<float4> source, SamplerState linearSampler, float2 uv)
{
    float amount = LensParams0.z;
    float3 center = source.SampleLevel(linearSampler, uv, 0).rgb;
    if (amount <= 0.001f) return center;
    float width, height;
    source.GetDimensions(width, height);
    float2 fromCenter = uv - 0.5f;
    float2 offset = fromCenter * dot(fromCenter, fromCenter) * 4.0f * amount / float2(width, height);
    float red = source.SampleLevel(linearSampler, uv - offset, 0).r;
    float blue = source.SampleLevel(linearSampler, uv + offset, 0).b;
    return float3(red, center.g, blue);
}

// Vignette (natural cos^4 style falloff) and luminance dependent film grain, display space.
float3 ApplyLensOutput(float3 color, float2 uv, float2 pixel)
{
    float vignette = LensParams0.x;
    if (vignette > 0.001f)
    {
        float2 d = (uv - 0.5f) * float2(1.0f, 0.75f) * 2.0f;
        float falloff = saturate(1.0f - dot(d, d) * vignette * (1.0f - LensParams0.y * 0.5f));
        color *= lerp(1.0f, falloff * falloff, saturate(vignette * 2.0f));
    }
    float grain = LensParams0.w;
    if (grain > 0.001f)
    {
        float noise = LensHash(pixel + frac(LensParams1.y * 0.6180339f) * 1024.0f) - 0.5f;
        float luma = dot(color, kLumaWeights);
        // Grain is strongest in the mid tones, like film.
        float response = 4.0f * luma * (1.0f - saturate(luma));
        color = max(color + noise * grain * 0.12f * (0.25f + response), 0.0f);
    }
    return color;
}

#endif
