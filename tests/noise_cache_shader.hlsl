#include "../shader/hlsl/ProceduralTexture.hlsli"
StructuredBuffer<ProceduralTexture::Parameters> Definitions : register(t0);
RWStructuredBuffer<float4> Result : register(u0);
cbuffer Test : register(b0)
{
    uint SampleCount;
    uint DefinitionCount;
    uint ReferenceMode; // 0: cache, 1: analytical, 2: independent bilinear reference.
    uint CoordinateOffset;
    uint Scenario; // 0: nodes, 1: off nodes, 2: finer level, 3: invalid footprint.
};

float3 HalfColor(ProceduralTexture::Parameters p, float2 uv)
{
    float3 color = ProceduralTexture::Color(p, ProceduralTexture::Scalar(p, uv));
    return f16tof32(f32tof16(color));
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= SampleCount)
        return;

    ProceduralTexture::Parameters p = Definitions[id.x % DefinitionCount];
    float step = exp2(Scenario == 2 ? -24.0f : -20.0f);
    // 57,600 unique nodes cover 16x16 complete tiles, including negative borders.
    // Repeat 7,936 nodes to retain 65,536 requests without introducing edge pages.
    uint node = id.x % (240u * 240u);
    float2 cell = float2(int(node % 240u) - 120 + int(CoordinateOffset), int(node / 240u) - 120);
    float2 fraction = Scenario == 1 ? float2(0.25, 0.75) : float2(0, 0);
    float2 uv = (cell + fraction) * step;
    float2 footprint = Scenario == 3 ? float2(0, step) : float2(step, -step);
    float3 color;
    if (ReferenceMode == 1)
        color = ProceduralTexture::Color(p, ProceduralTexture::Scalar(p, uv));
    else if (ReferenceMode == 2)
    {
        // Do not call cache address/interpolation helpers in this oracle.
        float3 corners[4];
        [loop]
        for (uint corner = 0; corner < 4; ++corner)
        {
            float2 cornerOffset = float2(corner & 1u, corner >> 1u);
            corners[corner] = HalfColor(p, (cell + cornerOffset) * step);
        }
        float3 lower = lerp(corners[0], corners[1], fraction.x);
        float3 upper = lerp(corners[2], corners[3], fraction.x);
        color = lerp(lower, upper, fraction.y);
    }
    else
        color = ProceduralTexture::CachedColor(p, uv, footprint);

    float3 cached;
    bool hit = ProceduralTexture::FindCachedColor(p, uv, footprint, cached);
    Result[id.x] = float4(color, hit ? 1 : 0);
}



