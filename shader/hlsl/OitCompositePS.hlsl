// Weighted blended OIT resolve (McGuire & Bavoil 2013). Compiled at runtime by transparencyeffects.h.
Texture2D<float4> OitAccumulation : register(t0);
Texture2D<float> OitRevealage : register(t1);

struct FullscreenOutput
{
    float4 Position : SV_POSITION;
};

FullscreenOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

// Output: rgb = average transparent colour, a = total coverage (1 - revealage).
// Blend: src * a + dst * (1 - a).
float4 CompositePS(FullscreenOutput input) : SV_Target
{
    int2 pixel = int2(input.Position.xy);
    float revealage = OitRevealage.Load(int3(pixel, 0));
    if (revealage >= 0.9999f) discard;
    float4 accumulation = OitAccumulation.Load(int3(pixel, 0));
    // Suppress overflow of the fp16 accumulation.
    if (any(isinf(accumulation.rgb))) accumulation.rgb = accumulation.aaa;
    float3 average = accumulation.rgb / max(accumulation.a, 0.00001f);
    return float4(average, 1.0f - revealage);
}
