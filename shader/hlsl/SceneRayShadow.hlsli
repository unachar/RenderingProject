#ifndef SCENE_RAY_SHADOW_HLSLI
#define SCENE_RAY_SHADOW_HLSLI

#ifdef SHADER_RAYTRACING
RaytracingAccelerationStructure LightingScene : register(t25);
cbuffer SceneRayGlobals : register(b5)
{
    float4 SceneRayFlags; // DXR capability, shadow enable, TLAS ready, reserved
    float4 SceneRayParameters; // maximum range, normal bias, reserved
    uint4 SceneShadowLights[160];
};

float TraceSceneShadow(float3 position, float3 normal, float3 lightDirection, float maximumDistance)
{
    if (SceneRayFlags.y < 0.5f || SceneRayFlags.z < 0.5f) return 1.0f;
    RayDesc ray;
    float3 offsetDirection = dot(normal, normal) > 0.01f ? normal : lightDirection;
    ray.Origin = position + offsetDirection * SceneRayParameters.y;
    ray.Direction = lightDirection;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = max(maximumDistance - SceneRayParameters.y, ray.TMin + 0.001f);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) { }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1.0f : 0.0f;
}
#endif

#endif
