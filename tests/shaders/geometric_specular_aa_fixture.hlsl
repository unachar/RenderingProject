#include "../../shader/hlsl/GeometricSpecularAA.hlsli"
RWStructuredBuffer<float4> Output : register(u0);

[RootSignature("UAV(u0)")]
[numthreads(64, 1, 1)]
void Compute(uint3 dispatch : SV_DispatchThreadID)
{
    uint index = dispatch.x;
    uint scenario = index / 16;
    float materialClass = float(index % 16);
    float roughness = materialClass / 15.0f;
    float3 center = float3(0, 1, 0);
    float3 neighbor = float3(1, 1, 0);
    bool valid = true;
    if (scenario == 0) neighbor = center;
    if (scenario == 2) neighbor.x = asfloat(0x7fc00000u);
    if (scenario == 3) valid = false;
    if (scenario == 4) center = 0;
    if (scenario == 5) neighbor.x = asfloat(0x7f800000u);
    if (scenario == 6) neighbor = float3(0, -1, 0);
    float3 difference = GeometricSpecularAA::NormalDifference(center, neighbor, valid);
    float gradient = GeometricSpecularAA::FiniteGradientSquared(dot(difference, difference), 0);
    Output[index] = float4(GeometricSpecularAA::FilterMaterialRoughness(roughness, gradient, materialClass),
        GeometricSpecularAA::FilterRoughness(roughness, gradient, 0.15f, 0.2f),
        GeometricSpecularAA::FiniteGradientSquared(asfloat(0x7fc00000u), 0.25f), gradient);
}

struct RasterInput
{
    float4 Position : SV_Position;
    float3 Normal : NORMAL;
};

[RootSignature("UAV(u0)")]
RasterInput Vertex(uint vertex : SV_VertexID)
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    RasterInput output;
    output.Position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    output.Normal = float3(uv.x * 2 - 1, 1, uv.y * 2 - 1);
    return output;
}

[RootSignature("UAV(u0)")]
float4 Pixel(RasterInput input) : SV_Target
{
    float3 normal = input.Normal;
    if (input.Position.x < 8) normal = float3(0, 1, 0);
    normal = GeometricSpecularAA::GeometryNormal(normal);
    float3 dx = ddx(normal);
    float3 dy = ddy(normal);
    float gradient = GeometricSpecularAA::FiniteGradientSquared(dot(dx, dx), dot(dy, dy));
    float materialClass = 11;
    if (input.Position.x >= 8 && input.Position.x < 16) materialClass = 4; // ShaderClass::Toon
    if (input.Position.x >= 16 && input.Position.x < 24) materialClass = 10; // ShaderClass::Unlit
    return float4(GeometricSpecularAA::FilterMaterialRoughness(0.04f, gradient, materialClass),
                  0.04f, gradient, materialClass);
}
