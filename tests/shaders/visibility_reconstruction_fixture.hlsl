#define GEOMETRIC_SPECULAR_AA 1
#include "../../shader/hlsl/GPUDrivenVisibilityResolveCS.hlsl"
RWStructuredBuffer<float4> TestResults : register(u6);
#define TEST_VISIBILITY_ROOT "UAV(u6),SRV(t30),SRV(t33),SRV(t34),SRV(t35),CBV(b0)"

float4 TestReconstruction(uint4 id, float2 pixel, uint2 extent)
{
    PSInput3D input;
    bool valid = Reconstruct(id, pixel, extent, input);
    if (!valid)
    {
        return float4(0.04f, 0, 0, 0);
    }
    PrepareVisibilityMaterialGradients(input, id, extent);
    float dxSquared = dot(VisibilityNormalDx, VisibilityNormalDx);
    float dySquared = dot(VisibilityNormalDy, VisibilityNormalDy);
    float gradient = GeometricSpecularAA::FiniteGradientSquared(dxSquared, dySquared);
    return float4(GeometricSpecularAA::FilterMaterialRoughness(0.04f, gradient, 11),
                  dxSquared, dySquared, 1);
}

[RootSignature(TEST_VISIBILITY_ROOT)]
[numthreads(8, 1, 1)]
void TestVisibility(uint3 dispatch : SV_DispatchThreadID)
{
    uint scenario = dispatch.x;
    uint4 id = uint4(0, 0, 0, 1);
    float2 pixel = float2(7.5f, 7.5f);
    if (scenario == 1) pixel.x += 1;
    if (scenario == 2) id = uint4(1, 1, 0, 1);
    if (scenario == 3) id.y = 2; // Degenerate triangle.
    if (scenario == 4) id.y = 3; // Nonresident geometry record.
    if (scenario == 5) id.y = 5; // Non-finite interpolated normal.
    if (scenario == 6) id.x = 1; // Same geometry under a different instance transform.
    if (scenario == 7) id.z = 1; // Out-of-range primitive.
    TestResults[scenario] = TestReconstruction(id, pixel, uint2(16, 16));
}

[RootSignature(TEST_VISIBILITY_ROOT)]
[numthreads(1, 1, 1)]
void TestPerspective()
{
    // The center reconstructs, but extrapolating one pixel in X reaches a zero
    // perspective denominator. That failed direction must have a zero gradient.
    TestResults[0] = TestReconstruction(uint4(0, 4, 0, 1), float2(0.5f, 0.5f), uint2(1, 1));
    PSInput3D neighbor;
    bool neighborValid = Reconstruct(uint4(0, 4, 0, 1), float2(1.5f, 0.5f), uint2(1, 1), neighbor);
    TestResults[1] = float4(neighborValid ? 1.0f : 0.0f, 0, 0, 0);
}
