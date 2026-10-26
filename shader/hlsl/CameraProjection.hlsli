#ifndef CAMERA_PROJECTION_HLSLI
#define CAMERA_PROJECTION_HLSLI

bool CameraProjectionIsOrthographic(float4x4 inverseViewProjection)
{
    return abs(inverseViewProjection[2][3]) < 1e-6f;
}

float3 CameraRayDirection(float2 uv, float4x4 inverseViewProjection)
{
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    float4 nearPoint = mul(float4(ndc, 1, 1), inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 0, 1), inverseViewProjection);
    // Homogeneous subtraction works for both finite orthographic points and
    // the perspective direction at infinity (farPoint.w == 0).
    return normalize(farPoint.xyz * nearPoint.w - nearPoint.xyz * farPoint.w);
}

float3 CameraRayOriginFromWorld(float3 worldPosition, float3 cameraPosition, float4x4 inverseViewProjection)
{
    if (!CameraProjectionIsOrthographic(inverseViewProjection))
    {
        return cameraPosition;
    }
    float3 direction = CameraRayDirection(float2(0.5, 0.5), inverseViewProjection);
    return worldPosition - direction * dot(worldPosition - cameraPosition, direction);
}

float3 CameraRayOrigin(float2 uv, float3 cameraPosition, float4x4 inverseViewProjection)
{
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    float4 nearPoint = mul(float4(ndc, 1, 1), inverseViewProjection);
    return CameraRayOriginFromWorld(nearPoint.xyz / nearPoint.w, cameraPosition, inverseViewProjection);
}

float3 CameraViewDirection(float3 worldPosition, float3 cameraPosition, float4x4 inverseViewProjection)
{
    return CameraProjectionIsOrthographic(inverseViewProjection)
               ? -CameraRayDirection(float2(0.5, 0.5), inverseViewProjection)
               : normalize(cameraPosition - worldPosition + 1e-20f);
}

#endif
