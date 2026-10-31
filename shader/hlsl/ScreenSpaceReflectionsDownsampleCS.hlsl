// SSR result mip generation: 2x2 box downsample of the resolved
// reflection (radiance + confidence) so the lighting pass can
// sample rough reflections from a prefiltered mip instead of
// point-sampling the sharp half-resolution trace.

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Output : register(u0);

cbuffer DownsampleConstants : register(b0)
{
    uint2 OutputSize;
    uint2 SourceSize;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= OutputSize.x || dispatchThreadId.y >= OutputSize.y)
    {
        return;
    }
    const int2 base = int2(dispatchThreadId.xy) * 2;
    const int2 maxTexel = int2(SourceSize) - 1;
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    sum += Source.Load(int3(min(base + int2(0, 0), maxTexel), 0));
    sum += Source.Load(int3(min(base + int2(1, 0), maxTexel), 0));
    sum += Source.Load(int3(min(base + int2(0, 1), maxTexel), 0));
    sum += Source.Load(int3(min(base + int2(1, 1), maxTexel), 0));
    Output[dispatchThreadId.xy] = sum * 0.25f;
}
