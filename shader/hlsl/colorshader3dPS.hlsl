#define SHADER_3D
#include "common.hlsl"

Texture2D g_Texture : register(t0);
Texture2D g_NormalTexture : register(t2);
SamplerState g_SamplerState : register(s0);

void main(in PSInput3D In, out float4 outDiffuse : SV_Target)
{
    float4 baseColor = In.Diffuse;
    float3 normal = normalize(-In.Normal.xyz);
    if (UseNormalMap != 0)
    {
        float3 tangentNormal = g_NormalTexture.Sample(g_SamplerState, In.TexCoord).xyz * 2.0f - 1.0f;
        float3 tangent = SafeNormalizeCommon(mul(float3(1.0f, 0.0f, 0.0f), (float3x3) World), float3(1.0f, 0.0f, 0.0f));
        float3 bitangent = SafeNormalizeCommon(mul(float3(0.0f, -1.0f, 0.0f), (float3x3) World), float3(0.0f, 0.0f, 1.0f));
        normal = SafeNormalizeCommon(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * normal, normal);
    }
    
    float3 lightDirection;
    float3 lightColor;
    float lightAttenuation;
    float volumeScatter;
    float rangeBlend;
    ResolveLightAggregate(In.WorldPos, In.Position.xy, lightDirection, lightColor, lightAttenuation, volumeScatter, rangeBlend);
    float lightIntensity = 1.0f;
    
    if (UseTexture)
    {
        baseColor *= g_Texture.Sample(g_SamplerState, In.TexCoord);
    }
    baseColor.a *= saturate(MaterialAlpha);
    
    float lighting = saturate(dot(normal, lightDirection));
    float shadowVisibility = SampleShadowMap(In.WorldPos, normal, lightDirection);
    float castShadow = saturate(1.0f - shadowVisibility);
    float shadowMask = 1.0f - castShadow * ShadowStrength * lightAttenuation;
    float toonLit = smoothstep(ShadowThreshold - ShadowSoftness, ShadowThreshold + ShadowSoftness, lighting);
    float directLight = lerp(0.35f, 1.0f, toonLit) * shadowMask;
    float3 lit = baseColor.rgb * (0.18f + lightColor * directLight * lightIntensity);
    lit += lightColor * volumeScatter;
    outDiffuse = float4(lit * baseColor.a, baseColor.a);
}
