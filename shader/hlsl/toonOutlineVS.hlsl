#define SHADER_3D
#include "common.hlsl"
#include "ToonOutlineProjection.hlsli"

PSInput3D main(VSInput3D input)
{
    PSInput3D output;
    float normalLenSq = dot(input.Normal, input.Normal);
    float3 safeNormal = (normalLenSq > 0.000001f) ? input.Normal : float3(0.0f, 0.0f, 0.0f);
    float3 extrusionVector = mul(safeNormal, (float3x3)World);
    float3 worldNormal = SafeNormalizeCommon(extrusionVector, float3(0.0f, 1.0f, 0.0f));
    float outlineWidth = max(ToonOutlineWidth, 0.0f);
    float useScreenSpace = ToonOutlineUseScreenSpace != 0 ? 1.0f : 0.0f;
    float3 worldModePosition = input.Position + safeNormal * outlineWidth;
    float3 baseModePosition = lerp(worldModePosition, input.Position, useScreenSpace);

    float4 worldPos = mul(float4(baseModePosition, 1.0f), World);
    float4 viewPos = mul(worldPos, View);
    float4 clipPos = mul(viewPos, Projection);

    if (ToonOutlineUseScreenSpace != 0)
    {
        float4 normalClip = mul(mul(float4(extrusionVector, 0.0f), View), Projection);
        clipPos.xy += ToonOutlineClipOffset(clipPos, normalClip, ViewportSize, ToonOutlineScreenWidth);
    }

    output.Position = clipPos;
    output.Normal = worldNormal;
    output.TexCoord = input.TexCoord;

    output.Diffuse = float4(0.0f, 0.0f, 0.0f, saturate(MaterialAlpha));
    output.WorldPos = worldPos.xyz;
    output.ViewPos = viewPos.xyz;
    output.CameraPos = CameraPos;
    return output;
}
