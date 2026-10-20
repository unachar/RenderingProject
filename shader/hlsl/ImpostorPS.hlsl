// フォワード経路のインポスター。modelshaderPS の ShadeForward（ライト・影・GI・環境光）をそのまま使い、
// 表面（アルベド・法線・位置・深度）だけをアトラスから復元した値に差し替える。
#define IMPOSTOR_FORWARD
static float3 g_ImpostorAlbedo = float3(1.0f, 1.0f, 1.0f);
static float3 g_ImpostorNormal = float3(0.0f, 1.0f, 0.0f);
static float g_ImpostorDepth = 0.0f;

#include "modelshaderPS.hlsl"
#include "ImpostorCommon.hlsli"

ForwardOutput main(ImpostorPSInput input)
{
    ImpostorSurface surface = ResolveImpostorSurface(input);
    g_ImpostorAlbedo = surface.Albedo;
    g_ImpostorNormal = surface.WorldNormal;
    g_ImpostorDepth = surface.Depth;

    PSInput3D shading;
    shading.Position = input.Position;
    shading.Normal = surface.WorldNormal;
    shading.TexCoord = float2(0.0f, 0.0f);
    shading.Diffuse = float4(surface.Albedo, surface.PartId);
    shading.WorldPos = surface.WorldPos;
    shading.ViewPos = mul(float4(surface.WorldPos, 1.0f), View).xyz;
    shading.CameraPos = CameraPos;

    float4 premultiplied = ShadeForward(shading);
    ForwardOutput output;
    output.Color = premultiplied;
    output.Accumulation = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.Revealage = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.HardwareDepth = max(surface.Depth, input.Position.z);
    return output;
}
