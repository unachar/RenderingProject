#define SHADER_3D
#include "common.hlsl"

PSInput3D main(VSInput3D input)
{
    PSInput3D output;
    float normalLenSq = dot(input.Normal, input.Normal);
    float3 safeNormal = (normalLenSq > 0.000001f) ? input.Normal : float3(0.0f, 0.0f, 0.0f);
    float3 worldNormal = (normalLenSq > 0.000001f) ? normalize(mul(safeNormal, (float3x3)World)) : float3(0.0f, 1.0f, 0.0f);
    float outlineWidth = max(ToonOutlineWidth, 0.0f);
    float useScreenSpace = ToonOutlineUseScreenSpace != 0 ? 1.0f : 0.0f;
    float3 worldModePosition = input.Position + safeNormal * outlineWidth;
    float3 baseModePosition = lerp(worldModePosition, input.Position, useScreenSpace);

    float4 worldPos = mul(float4(baseModePosition, 1.0f), World);
    float4 viewPos = mul(worldPos, View);
    float4 clipPos = mul(viewPos, Projection);

    if (ToonOutlineUseScreenSpace != 0)
    {
        float normalProbeWidth = max(outlineWidth, 0.001f);
        float4 probeWorldPos = mul(float4(input.Position + safeNormal * normalProbeWidth, 1.0f), World);
        float4 probeClipPos = mul(mul(probeWorldPos, View), Projection);
        float2 viewport = max(ViewportSize, float2(1.0f, 1.0f));
        float2 baseNdc = clipPos.xy / max(abs(clipPos.w), 0.000001f);
        float2 probeNdc = probeClipPos.xy / max(abs(probeClipPos.w), 0.000001f);
        float2 screenDir = probeNdc - baseNdc;
        float dirLenSq = dot(screenDir, screenDir);
        float2 fallbackDir = normalize(worldNormal.xy + float2(0.0001f, 0.0f));
        screenDir = (dirLenSq > 0.00000001f) ? screenDir * rsqrt(dirLenSq) : fallbackDir;
        float2 ndcOffset = screenDir * (max(ToonOutlineScreenWidth, 0.0f) * 2.0f / viewport);
        clipPos.xy += ndcOffset * clipPos.w;
    }

    output.Position = clipPos;
    output.Normal = worldNormal;
    output.TexCoord = input.TexCoord;
    float isSelectionOutline = (ShaderClass == 99) ? 1.0f : 0.0f;
    float3 outlineColor = lerp(float3(0.0f, 0.0f, 0.0f), float3(1.0f, 0.42f, 0.0f), isSelectionOutline);
    // Keep the outline in the same opacity domain as its material.  Without this,
    // fading a toon model leaves an opaque black silhouette behind.
    output.Diffuse = float4(outlineColor, saturate(MaterialAlpha));
    output.WorldPos = worldPos.xyz;
    output.ViewPos = viewPos.xyz;
    output.CameraPos = CameraPos;
    return output;
}
