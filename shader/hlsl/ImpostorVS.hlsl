#define SHADER_3D
#define IMPOSTOR_VERTEX_SHADER
#include "common.hlsl"
#include "ImpostorCommon.hlsli"

// 1インスタンス = カメラに正対する四角形1枚（6頂点、Position.xy = 角の符号）。
struct ImpostorInstance
{
    row_major float4x4 World;
};

StructuredBuffer<ImpostorInstance> ImpostorInstances : register(t0, space2);

float3x3 ImpostorInverse3x3(float3x3 m)
{
    float3 r0 = cross(m[1], m[2]);
    float3 r1 = cross(m[2], m[0]);
    float3 r2 = cross(m[0], m[1]);
    float determinant = dot(m[0], r0);
    determinant = abs(determinant) > 1e-12f ? determinant : 1e-12f;
    return transpose(float3x3(r0, r1, r2)) / determinant;
}

ImpostorPSInput main(VSInput3D input, uint instanceId : SV_InstanceID)
{
    row_major float4x4 world = ImpostorInstances[instanceId].World;
    const float3x3 world3x3 = (float3x3)world;
    const float3 translation = world[3].xyz;
    const float3 localCenter = ImpostorBounds.xyz;
    const float localRadius = max(ImpostorBounds.w, 1e-4f);

    const float3 worldCenter = mul(localCenter, world3x3) + translation;
    const float maxScale = max(length(world[0].xyz), max(length(world[1].xyz), length(world[2].xyz)));
    const float worldRadius = localRadius * maxScale;

    float3 toCamera = SurfaceViewDirectionCommon(worldCenter, CameraPos);
    const float distanceToCamera = max(length(worldCenter - SurfaceCameraOriginCommon(worldCenter, CameraPos)), 1e-4f);

    // 境界球のカメラ側の接平面に置く。表面は必ずこの平面より奥にあるので、
    // ピクセル深度は補間深度以下（SV_DepthLessEqual）になり Early-Z が効く。
    const float offset = min(worldRadius, distanceToCamera * 0.5f);
    const float planeDistance = distanceToCamera - offset;
    float halfSize = worldRadius;
    if (abs(Projection[3][3]) < 0.5f && distanceToCamera > worldRadius * 1.001f)
    {
        // 平面上での境界球のシルエット半径（透視投影で必要な最小サイズ）。
        halfSize = planeDistance * worldRadius /
                   sqrt(max(distanceToCamera * distanceToCamera - worldRadius * worldRadius, 1e-8f));
    }
    halfSize = min(halfSize * 1.02f, worldRadius * 4.0f);

    const float3 upHint = abs(toCamera.y) > 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    const float3 right = normalize(cross(upHint, toCamera));
    const float3 up = cross(toCamera, right);
    const float3 worldPos =
        worldCenter + toCamera * offset + (right * input.Position.x + up * input.Position.y) * halfSize;

    const float3x3 inverseWorld = ImpostorInverse3x3(world3x3);
    const float3 localCamera = mul(SurfaceCameraOriginCommon(worldCenter, CameraPos) - translation, inverseWorld);

    float2 cell0;
    float2 cell1;
    float2 cell2;
    float3 weights;
    ImpostorSelectFrames(localCamera - localCenter, ImpostorGrid.x, ImpostorGrid.y > 0.5f, cell0, cell1, cell2,
                         weights);

    ImpostorPSInput output;
    output.Position = mul(mul(float4(worldPos, 1.0f), View), Projection);
    output.WorldPos = worldPos;
    output.LocalPos = mul(worldPos - translation, inverseWorld);
    output.LocalCamera = localCamera;
    output.FrameCells01 = float4(cell0, cell1);
    output.FrameCell2Weights = float4(cell2, weights.x, weights.y);
    output.WorldRow0 = float4(world[0].xyz, translation.x);
    output.WorldRow1 = float4(world[1].xyz, translation.y);
    output.WorldRow2 = float4(world[2].xyz, translation.z);
    return output;
}
