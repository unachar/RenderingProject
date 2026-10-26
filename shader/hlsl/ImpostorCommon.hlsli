#ifndef IMPOSTOR_COMMON_HLSLI
#define IMPOSTOR_COMMON_HLSLI

// 実行時のインポスター（オクタヘドラル、3フレームブレンド + 深度による視差補正 + ピクセル深度出力）。
// 取り込み側で common.hlsl（SHADER_3D）と g_Texture(t0) / g_NormalTexture(t2) / g_SamplerState(s0) を宣言しておくこと。
#include "ImpostorMath.hlsli"

cbuffer ImpostorConstants : register(b4)
{
    float4 ImpostorBounds;   // xyz: ローカル中心, w: 半径
    float4 ImpostorGrid;     // x: 1辺のフレーム数, y: 上半球のみ, z: アトラス解像度, w: 最大 mip
    float4 ImpostorParams;   // x: 被覆率のしきい値, y: mip バイアス
    float4 ImpostorReserved;
};

struct ImpostorPSInput
{
#ifdef IMPOSTOR_VERTEX_SHADER
    float4 Position : SV_POSITION;
#else
    // SV_DepthLessEqual を出すため、位置は noperspective centroid（POM と同じ制約）。
    noperspective centroid float4 Position : SV_POSITION;
#endif
    float3 WorldPos : TEXCOORD1;
    float3 LocalPos : TEXCOORD2;
    nointerpolation float3 LocalCamera : TEXCOORD3;
    nointerpolation float4 FrameCells01 : TEXCOORD4;
    nointerpolation float4 FrameCell2Weights : TEXCOORD5; // xy: セル2, z: 重み0, w: 重み1
    nointerpolation float4 WorldRow0 : TEXCOORD6;         // xyz: ワールド行列の行, w: 平行移動
    nointerpolation float4 WorldRow1 : TEXCOORD7;
    nointerpolation float4 WorldRow2 : TEXCOORD8;
};

#ifndef IMPOSTOR_VERTEX_SHADER
struct ImpostorSurface
{
    float3 Albedo;
    float3 WorldNormal;
    float3 WorldPos;
    float Depth;
    float PartId;
    float Coverage;
};

float2 ImpostorAtlasUv(float2 cell, float2 frameUv, float framesPerAxis, float halfTexel)
{
    return (cell + clamp(frameUv, halfTexel, 1.0f - halfTexel)) / framesPerAxis;
}

ImpostorSurface ResolveImpostorSurface(ImpostorPSInput input)
{
    const float framesPerAxis = max(ImpostorGrid.x, 2.0f);
    const bool hemisphere = ImpostorGrid.y > 0.5f;
    const float atlasResolution = max(ImpostorGrid.z, 1.0f);
    const float maxMip = ImpostorGrid.w;
    const float3 center = ImpostorBounds.xyz;
    const float radius = max(ImpostorBounds.w, 1e-4f);
    const float frameTexels = atlasResolution / framesPerAxis;
    const float halfTexel = 0.5f / frameTexels;

    float3 origin = input.LocalCamera;
    float3 rayDirection = normalize(input.LocalPos - origin);
    if (abs(Projection[3][3]) > 0.5f)
    {
        rayDirection = normalize(center - origin);
        origin = input.LocalPos - rayDirection * dot(input.LocalPos - origin, rayDirection);
    }

    float2 cells[3] = {input.FrameCells01.xy, input.FrameCells01.zw, input.FrameCell2Weights.xy};
    float weights[3] = {input.FrameCell2Weights.z, input.FrameCell2Weights.w,
                        saturate(1.0f - input.FrameCell2Weights.z - input.FrameCell2Weights.w)};

    float3 directions[3];
    float3 rights[3];
    float3 ups[3];
    float denominators[3];
    float2 planeUv[3];
    [unroll] for (int k = 0; k < 3; ++k)
    {
        directions[k] = ImpostorFrameDirection(cells[k], framesPerAxis, hemisphere);
        ImpostorFrameBasis(directions[k], rights[k], ups[k]);
        float denominator = dot(rayDirection, directions[k]);
        denominators[k] = abs(denominator) < 1e-4f ? -1e-4f : denominator;
        // 視線とフレーム平面（中心を通り撮影方向に垂直）の交点。
        float t = dot(center - origin, directions[k]) / denominators[k];
        planeUv[k] = ImpostorProject(origin + rayDirection * t, center, radius, rights[k], ups[k]);
    }

    // mip は平面 UV の画面微分から決める（discard より前に計算する）。
    float2 dx = ddx(planeUv[0]) * frameTexels;
    float2 dy = ddy(planeUv[0]) * frameTexels;
    float lod = clamp(0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8f)) + ImpostorParams.y, 0.0f, maxMip);

    // パス1：各フレームの深度から、視線上の表面点を推定する（視差補正）。
    float3 rayPoints[3];
    [unroll] for (int p = 0; p < 3; ++p)
    {
        float4 normalDepth = g_NormalTexture.SampleLevel(
            g_SamplerState, ImpostorAtlasUv(cells[p], planeUv[p], framesPerAxis, halfTexel), lod);
        float t = (radius - 2.0f * radius * normalDepth.b - dot(origin - center, directions[p])) / denominators[p];
        rayPoints[p] = origin + rayDirection * t;
    }

    // パス2：補正した位置で再サンプルしてブレンドする。
    float coverage = 0.0f;
    float weightSum = 0.0f;
    float3 colorSum = 0.0f;
    float3 normalSum = 0.0f;
    float3 positionSum = 0.0f;
    float bestWeight = -1.0f;
    float partEncoded = 0.0f;
    [unroll] for (int s = 0; s < 3; ++s)
    {
        float2 frameUv = ImpostorProject(rayPoints[s], center, radius, rights[s], ups[s]);
        float inside = (all(frameUv >= 0.0f) && all(frameUv <= 1.0f)) ? 1.0f : 0.0f;
        float2 atlasUv = ImpostorAtlasUv(cells[s], frameUv, framesPerAxis, halfTexel);
        float4 albedo = g_Texture.SampleLevel(g_SamplerState, atlasUv, lod);
        float4 normalDepth = g_NormalTexture.SampleLevel(g_SamplerState, atlasUv, lod);
        float frameCoverage = albedo.a * inside;
        float weight = weights[s] * frameCoverage;
        coverage += weight;
        weightSum += weight;
        colorSum += albedo.rgb * weight;
        normalSum += ImpostorOctDecode(normalDepth.rg * 2.0f - 1.0f) * weight;
        float t = (radius - 2.0f * radius * normalDepth.b - dot(origin - center, directions[s])) / denominators[s];
        positionSum += (origin + rayDirection * t) * weight;
        if (weight > bestWeight)
        {
            bestWeight = weight;
            partEncoded = normalDepth.a;
        }
    }

    clip(coverage - ImpostorParams.x);

    const float inverseWeight = 1.0f / max(weightSum, 1e-5f);
    const float3 localPosition = positionSum * inverseWeight;
    const float normalLengthSq = dot(normalSum, normalSum);
    const float3 localNormal = normalLengthSq > 1e-10f ? normalSum * rsqrt(normalLengthSq) : -rayDirection;

    const float3x3 world3x3 = float3x3(input.WorldRow0.xyz, input.WorldRow1.xyz, input.WorldRow2.xyz);
    const float3 translation = float3(input.WorldRow0.w, input.WorldRow1.w, input.WorldRow2.w);

    ImpostorSurface surface;
    surface.Albedo = colorSum * inverseWeight;
    surface.WorldPos = mul(localPosition, world3x3) + translation;
    // 通常のインスタンス描画（modelshaderInstancedVS）と同じ法線変換。
    surface.WorldNormal = normalize(mul(localNormal, world3x3));
    float4 clipPosition = mul(mul(float4(surface.WorldPos, 1.0f), View), Projection);
    surface.Depth = clipPosition.w > 1e-6f ? saturate(clipPosition.z / clipPosition.w) : saturate(input.Position.z);
    surface.PartId = round(saturate(partEncoded) * 15.0f);
    surface.Coverage = coverage;
    return surface;
}
#endif // !IMPOSTOR_VERTEX_SHADER

#endif
