#ifndef SCENE_RAY_SHADOW_HLSLI
#define SCENE_RAY_SHADOW_HLSLI

#if defined(SHADER_RAYTRACING) || defined(SHADER_MATERIAL_OPTICS_PREVIEW)
cbuffer SceneRayGlobals : register(b5)
{
    float4 SceneRayFlags;      // RT シャドウモード、影の有効フラグ、TLAS 準備完了、ガラスのメタデータのインスタンス数
    // 最大範囲、法線バイアス、角半径、負の影サンプル数（正はマテリアルプレビュー専用）。
    float4 SceneRayParameters;
    uint4 SceneShadowLights[160];
};
#endif

#ifdef SHADER_RAYTRACING
RaytracingAccelerationStructure LightingScene : register(t25);

float TraceSceneShadow(float3 position, float3 normal, float3 lightDirection, float maximumDistance)
{
    if (SceneRayFlags.y < 0.5f || SceneRayFlags.z < 0.5f)
    {
        return 1.0f;
    }
    RayDesc ray;
    float3 offsetDirection = dot(normal, normal) > 0.01f ? normal : lightDirection;
    ray.Origin = position + offsetDirection * SceneRayParameters.y;
    ray.Direction = lightDirection;
    ray.TMin = SceneRayParameters.y * 0.25f;
    ray.TMax = max(maximumDistance - SceneRayParameters.y, ray.TMin + 0.001f);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x01, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1.0f : 0.0f;
}

float TraceSceneDirectionalShadow(float3 position, float3 normal, float3 direction)
{
    const float radius = SceneRayParameters.z;
    const uint sampleCount = (uint)clamp(-SceneRayParameters.w, 1.0f, 8.0f);
    if (radius <= 0.0f || sampleCount == 1u)
    {
        return TraceSceneShadow(position, normal, direction, SceneRayParameters.x);
    }

    const float3 up = abs(direction.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0);
    const float3 tangent = normalize(cross(up, direction));
    const float3 bitangent = cross(direction, tangent);
    float visibility = 0.0f;
    [loop] for (uint sample = 0; sample < sampleCount; ++sample)
    {
        const float angle = sample * 2.39996323f;
        const float distanceFromCenter = radius * sqrt((sample + 0.5f) / sampleCount);
        const float3 rayDirection = normalize(direction + distanceFromCenter *
            (cos(angle) * tangent + sin(angle) * bitangent));
        visibility += TraceSceneShadow(position, normal, rayDirection, SceneRayParameters.x);
    }
    return visibility / sampleCount;
}

float TraceSceneLocalShadow(float3 position, float3 normal, float3 lightDirection, float3 lightPosition,
                            float sourceRadius)
{
    const float3 toLight = lightPosition - position;
    const float distanceToLight = length(toLight);
    if (sourceRadius <= 0.0f || distanceToLight <= 0.0001f)
    {
        return TraceSceneShadow(position, normal, lightDirection, min(distanceToLight, SceneRayParameters.x));
    }

    const float3 direction = toLight / distanceToLight;
    const float3 up = abs(direction.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0);
    const float3 tangent = normalize(cross(up, direction));
    const float3 bitangent = cross(direction, tangent);
    const uint sampleCount = 8u;
    float visibility = 0.0f;
    [loop] for (uint sample = 0; sample < sampleCount; ++sample)
    {
        const float angle = sample * 2.39996323f;
        const float radius = sourceRadius * sqrt((sample + 0.5f) / sampleCount);
        const float3 ray = toLight + radius * (cos(angle) * tangent + sin(angle) * bitangent);
        const float distanceToSample = length(ray);
        visibility += TraceSceneShadow(position, normal, ray / distanceToSample,
                                       min(distanceToSample, SceneRayParameters.x));
    }
    return visibility / sampleCount;
}

// 始点（面上の点）から終点（ライト上の点）までの影レイ。MegaLights のリファレンス・M2 と同じ式
// （MegaLightsSampleCS.hlsl の MLTraceSegment と一致させる）
float TraceSceneSegment(float3 position, float3 normal, float3 target)
{
    if (SceneRayFlags.z < 0.5f)
    {
        return 1.0f;
    }
    const float bias = clamp(SceneRayParameters.y, 0.001f, 0.1f);
    const float3 origin = position + normal * bias;
    const float3 segment = target - origin;
    const float distance = length(segment);
    if (distance <= bias * 2.0f)
    {
        return 1.0f;
    }
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = segment / distance;
    ray.TMin = bias * 0.25f;
    ray.TMax = max(distance - bias, ray.TMin + 0.001f);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(LightingScene, RAY_FLAG_NONE, 0x01, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1.0f : 0.0f;
}
#endif

#endif
