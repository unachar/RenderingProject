// Native Hi-Z screen-space reflections: trace pass (see
// docs/rendering-technology-ssr-design.md). One thread per output
// pixel marches the reflected ray (world space) through the Hi-Z
// pyramid and writes reflected radiance (rgb) + confidence (a)
// sampled from the previous frame's lit scene color. Own root
// signature: b0 constants, t0 depth, t1 Hi-Z, t2 world normal
// (GBuffer encoding), t3 previous scene color, u0 result, s0 point,
// s1 linear.
//
// Depth convention: the engine uses reverse-Z. The Hi-Z pyramid
// stores per-cell the FARTHEST surface depth (smallest stored
// value). Stored depth d converts to positive view-space distance
// as ViewZ = DepthNumerator / (d - DepthDenominator); the host
// derives both from the projection matrix (for the row-vector
// convention, Numerator = P[3][2], Denominator = P[2][2]).

Texture2D<float> SceneDepth : register(t0);
Texture2D<float> HiZTexture : register(t1);
Texture2D<float4> GBufferNormal : register(t2);
Texture2D<float4> PreviousSceneColor : register(t3);
Texture2D<float4> GBufferMaterial : register(t4);
RWTexture2D<float4> ReflectionResult : register(u0);
SamplerState PointSampler : register(s0);
SamplerState LinearSampler : register(s1);

cbuffer TraceConstants : register(b0)
{
    float4x4 InvViewProjection;
    float4x4 ViewProjection;
    float2 Resolution;
    float2 InvResolution;
    float DepthNumerator;
    float DepthDenominator;
    float MaxDistance;
    float Thickness;
    uint MaxSteps;
    float StridePixels;
    float EdgeFade;
    float FallbackRoughness;
    float3 CameraPosition;
    float TracePadding0;
    float3 CameraForward;
    uint GBufferScale; // full-res GBuffer texel = pixel * GBufferScale (1 full-res trace, 2 half-res)
};

float ViewZFromStoredDepth(float storedDepth)
{
    return DepthNumerator / (storedDepth - DepthDenominator);
}

float3 WorldPositionFromDepth(float2 uv, float storedDepth)
{
    const float4 clip = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), storedDepth, 1.0f);
    const float4 world = mul(clip, InvViewProjection);
    return world.xyz / world.w;
}

float2 ProjectToUv(float3 worldPosition)
{
    const float4 clip = mul(float4(worldPosition, 1.0f), ViewProjection);
    return (clip.xy / clip.w) * float2(0.5f, -0.5f) + 0.5f;
}

float ViewDistanceOf(float3 worldPosition)
{
    return dot(worldPosition - CameraPosition, CameraForward);
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

    const int3 gbufferPixel = int3(pixel * max(GBufferScale, 1u), 0);
    const float depth = SceneDepth.Load(gbufferPixel);
    if (depth <= 1e-7f)
    {
        return; // sky (reverse-Z far)
    }
    const float3 worldPosition = WorldPositionFromDepth(uv, depth);
    const float3 worldNormal = normalize(GBufferNormal.Load(gbufferPixel).rgb * 2.0f - 1.0f);
    const float3 V = normalize(worldPosition - CameraPosition);
    const float3 R = reflect(V, worldNormal);
    if (dot(R, CameraForward) <= 0.001f)
    {
        return; // reflected ray does not advance into the frustum
    }

    // March along the ray (parameter t = distance from the shaded
    // point), sampling the Hi-Z pyramid at a mip derived from the
    // step footprint; refine hits with a short binary search
    // against mip 0 depth.
    const float3 rayStart = worldPosition + R * (Thickness * 0.5f);
    const float stepLength = MaxDistance / float(MaxSteps);
    float previousT = 0.0f;
    bool hit = false;
    float2 hitUv = uv;
    for (uint step = 1; step <= MaxSteps; ++step)
    {
        const float t = stepLength * float(step);
        const float3 rayPosition = rayStart + R * t;
        const float rayViewZ = ViewDistanceOf(rayPosition);
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
                    if (ViewDistanceOf(midPosition) > ViewZFromStoredDepth(refinedDepth))
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
    // Per-pixel roughness of the reflecting surface (material
    // GBuffer g channel) drives the prefilter mip; the constant is
    // only a fallback for pixels without a material entry.
    const float roughness = GBufferMaterial.Load(gbufferPixel).g;
    const float effectiveRoughness = roughness > 0.001f ? roughness : FallbackRoughness;
    const float3 radiance = PreviousSceneColor.SampleLevel(LinearSampler, hitUv, effectiveRoughness * 4.0f).rgb;
    ReflectionResult[pixel] = float4(radiance, confidence);
}
