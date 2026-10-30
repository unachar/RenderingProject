// Stage 2J live probe capture, step 1: cubemap -> equirectangular
// conversion (see docs/rendering-technology-stage2j-capture-design.md).
// The direction mapping is the exact inverse of the equirect
// sampling in SurfaceLighting.hlsli / EnvironmentPrefilterCS.hlsl,
// so the output can feed the existing GGX prefilter unchanged.
// Self-contained root signature: b0 constants, t0 cube, u0 output,
// s0 static sampler.

TextureCube<float4> SourceCube : register(t0);
RWTexture2D<float4> EquirectOutput : register(u0);
SamplerState LinearSampler : register(s0);

cbuffer ConvertConstants : register(b0)
{
    uint2 OutputSize;
    uint2 ConvertPadding;
};

static const float CONVERT_PI = 3.14159265359f;

// Inverse of the equirect mapping used by SurfaceLighting.hlsli.
float3 EquirectUvToDirection(float2 uv)
{
    const float phi = (uv.x - 0.5f) * 2.0f * CONVERT_PI;
    const float theta = uv.y * CONVERT_PI;
    const float sinTheta = sin(theta);
    return float3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= OutputSize.x || dispatchThreadId.y >= OutputSize.y)
    {
        return;
    }
    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(OutputSize);
    const float3 direction = EquirectUvToDirection(uv);
    EquirectOutput[dispatchThreadId.xy] = float4(SourceCube.SampleLevel(LinearSampler, direction, 0.0f).rgb, 1.0f);
}
