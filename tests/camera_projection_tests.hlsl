#include "../shader/hlsl/CameraProjection.hlsli"
#define MEGALIGHTS_CB_REGISTER b1
#include "../shader/hlsl/MegaLightsCommon.hlsli"

cbuffer CameraTest : register(b0)
{
    float4x4 InverseViewProjection;
    float4 CameraAndDepth;
};
RWStructuredBuffer<float4> Results : register(u0);

[numthreads(1, 1, 1)] void main()
{
    Results[0] = float4(CameraRayDirection(float2(0.5, 0.5), InverseViewProjection), 0);
    Results[1] = float4(CameraRayDirection(float2(0.9, 0.2), InverseViewProjection), 0);
    Results[2] = float4(CameraRayOrigin(float2(0.5, 0.5), CameraAndDepth.xyz, InverseViewProjection), 1);
    Results[3] = float4(CameraRayOrigin(float2(0.9, 0.2), CameraAndDepth.xyz, InverseViewProjection), 1);
    float3 world = MLWorldPositionFromDepth(uint2(50, 25), CameraAndDepth.w);
    float3 view = mul(float4(world, 1), MLWorldToView).xyz;
    Results[4] = float4(world, MLViewZFromDepth(CameraAndDepth.w));
    Results[5] = float4(MLViewToPixel(view), MLViewDirection(world).xy);
}
