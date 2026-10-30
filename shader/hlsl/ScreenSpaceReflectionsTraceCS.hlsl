// Native Hi-Z screen-space reflections: trace pass (see
// docs/rendering-technology-ssr-design.md). One thread per output
// pixel marches the reflected ray through the Hi-Z pyramid and
// writes reflected radiance (rgb) + confidence (a) sampled from the
// previous frame's lit scene color. Own root signature: b0
// constants, t0 depth, t1 Hi-Z, t2 world normal (GBuffer encoding),
// t3 previous scene color, u0 result, s0 linear clamp.
//
// Depth convention: the engine uses reverse-Z. The Hi-Z pyramid
// stores per-cell the FARTHEST surface depth (smallest stored
// value). Stored depth d converts to positive view-space distance
// as ViewZ = DepthNumerator / (d - DepthDenominator); the host
// derives both constants from the projection matrix in use (third
// row), validated in the integration step.

Texture2D<float> SceneDepth : register(t0);
Texture2D<float> HiZTexture : register(t1);
Texture2D<float4> GBufferNormal : register(t2);
Texture2D<float4> PreviousSceneColor : register(t3);
RWTexture2D<float4> ReflectionResult : register(u0);
SamplerState PointSampler : register(s0);
SamplerState LinearSampler : register(s1);

cbuffer TraceConstants : register(b0)
{
    float4x4 InvProjection;
    float4x4 Projection;
    float2 Resolution;
    float2 InvResolution;
    float DepthNumerator;
    float DepthDenominator;
    float MaxDistance;
    float Thickness;
    uint MaxSteps;
    float StridePixels;
    float EdgeFade;
    float Roughness;
};

float ViewZFromStoredDepth(float storedDepth)
{
    return DepthNumerator / (storedDepth - DepthDenominator);
}

float3 ViewPositionFromDepth(float2 uv, float storedDepth)
{
    const float4 clip = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), storedDepth, 1.0f);
    const float4 view = mul(clip, InvProjection);
    return view.xyz / view.w;
}

float2 ProjectToUv(float3 viewPosition)
{
    const float4 clip = mul(float4(viewPosition, 1.0f), Projection);
    return (clip.xy / clip.w) * float2(0.5f, -0.5f) + 0.5f;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)Resolution.x || dispatchThreadId.y >= (uint)Resolution.y)
    {
        return;
    }
    const uint2 pixel = dispatchThreadId.xy;
    const float2 uv = (float2(pixel) + 0.5f) * InvResolution;
    ReflectionResult[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);

    const float depth = SceneDepth.Load(int3(pixel, 0));
    if (depth <= 1e-7f)
    {
        return; // sky (reverse-Z far)
    }
    const float3 viewPosition = ViewPositionFromDepth(uv, depth);
    const float3 worldNormal = normalize(GBufferNormal.Load(int3(pixel, 0)).rgb * 2.0f - 1.0f);
    // The trace works in view space; the host passes normals already
    // transformed by the normal matrix packed into InvProjection's
    // frame setup step (integration step wires the view-space normal
    // GBuffer view). Until then worldNormal is used as-is and the
    // module stays disabled by default.
    const float3 V = normalize(viewPosition);
    const float3 R = reflect(V, normalize(worldNormal));
    if (R.z >= 0.0f)
    {
        return; // reflected ray points away from the camera
    }

    // March along the ray (parameter t = distance from the shaded
    // point), sampling the Hi-Z pyramid at a mip derived from the
    // step footprint; refine hits with a short binary search
    // against mip 0 depth.
    const float3 rayStart = viewPosition + R * (Thickness * 0.5f);
    const float stepLength = MaxDistance / float(MaxSteps);
    float previousT = 0.0f;
    bool hit = false;
    float2 hitUv = uv;
    for (uint step = 1; step <= MaxSteps; ++step)
    {
        const float t = stepLength * float(step);
        const float3 rayPosition = rayStart + R * t;
        const float rayViewZ = -rayPosition.z;
        if (rayViewZ <= 0.0f)
        {
            break;
        }
        const float2 rayUv = ProjectToUv(rayPosition);
        if (rayUv.x < 0.0f || rayUv.x > 1.0f || rayUv.y < 0.0f || rayUv.y > 1.0f)
        {
            break;
        }
        const float mip = clamp(log2(max(StridePixels, 1.0f)), 0.0f, 8.0f);
        const float cellDepth = HiZTexture.SampleLevel(PointSampler, rayUv, mip);
        if (cellDepth > 1e-7f)
        {
            const float surfaceViewZ = ViewZFromStoredDepth(cellDepth);
            if (rayViewZ > surfaceViewZ && rayViewZ - surfaceViewZ < Thickness + stepLength)
            {
                float lo = previousT;
                float hi = t;
                float2 refinedUv = rayUv;
                for (uint i = 0; i < 5; ++i)
                {
                    const float midT = 0.5f * (lo + hi);
                    const float3 midPosition = rayStart + R * midT;
                    refinedUv = ProjectToUv(midPosition);
                    const float refinedDepth = SceneDepth.SampleLevel(PointSampler, refinedUv, 0.0f);
                    if (refinedDepth <= 1e-7f)
                    {
                        lo = midT;
                        continue;
                    }
                    if (-midPosition.z > ViewZFromStoredDepth(refinedDepth))
                    {
                        hi = midT;
                    }
                    else
                    {
                        lo = midT;
                    }
                }
                hit = true;
                hitUv = refinedUv;
                break;
            }
        }
        previousT = t;
    }
    if (!hit)
    {
        return;
    }

    // Confidence: screen-edge fade and grazing fade.
    const float2 edgeDistance = min(hitUv, 1.0f - hitUv);
    const float edgeFade = smoothstep(0.0f, max(EdgeFade, 1e-4f), min(edgeDistance.x, edgeDistance.y));
    const float facingFade = saturate(dot(-V, R) * 2.0f);
    const float confidence = edgeFade * facingFade;
    if (confidence <= 0.001f)
    {
        return;
    }
    const float3 radiance = PreviousSceneColor.SampleLevel(LinearSampler, hitUv, Roughness * 4.0f).rgb;
    ReflectionResult[pixel] = float4(radiance, confidence);
}
