cbuffer Parameters : register(b0) { float4 Color; }
Texture2D<float4> Input : register(t0);
SamplerState LinearSampler : register(s0);
RWStructuredBuffer<float4> Output : register(u0);

#define TEST_ROOT "CBV(b0),DescriptorTable(SRV(t0)),UAV(u0),StaticSampler(s0)"

[RootSignature(TEST_ROOT)]
float4 Vertex(uint vertex : SV_VertexID) : SV_Position
{
    return float4(float2((vertex << 1) & 2, vertex & 2) * 2.0f - 1.0f, Color.x, 1.0f);
}

[RootSignature(TEST_ROOT)]
float4 Pixel(float4 position : SV_Position) : SV_Target
{
    return Input.SampleLevel(LinearSampler, position.xy * Color.xy, 0) * Color;
}

[RootSignature(TEST_ROOT)]
[numthreads(1, 1, 1)]
void Compute(uint3 index : SV_DispatchThreadID)
{
    Output[index.x] = Color;
}
