#include "../shader/hlsl/ProceduralTexture.hlsli"
StructuredBuffer<ProceduralTexture::Parameters> Definitions : register(t0);
RWStructuredBuffer<float4> Result : register(u0);

[numthreads(8, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ProceduralTexture::Parameters p = Definitions[0];
    float2 uv = float2(0.12345 + id.x * 0.000001, 0.6789);
    float height = ProceduralTexture::Scalar(p, uv);
    float3 color = ProceduralTexture::Color(p, height);
    float3 normal = ProceduralTexture::Normal(p, uv, (1.0 / 2048).xx);
    Result[id.x * 2] = float4(color, height);
    Result[id.x * 2 + 1] = float4(normal, 1);
}
