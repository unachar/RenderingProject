#define SHADER_3D
#include "common.hlsl"
#include "MaterialSurface.hlsli"

Texture2D<float4> BaseTexture : register(t0);
Texture2DArray<float> PreviousPeelDepth : register(t29);
SamplerState MaterialSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD,
    float vertexOpacity : COLOR, float3 worldPosition : TEXCOORD1,
    float3 normalWS : NORMAL) : SV_TARGET
{
    clip(0.5f - abs(PBRPadding.y - 3.0f));
    if (ShadowTransmissionParams.x > 0.5f)
    {
        float previousDepth = PreviousPeelDepth.Load(int4(int2(position.xy), 0, 0));
        clip(position.z - previousDepth - ShadowTransmissionParams.z);
    }
    AssetMaterialParameters material = LoadAssetMaterial(0);
    float4 color = UseTexture != 0 ? BaseTexture.Sample(MaterialSampler, uv) : 1.0f;
    float metallic = Metallic;
    float roughness = Roughness;
    float4 emissionAO;
    ApplyAssetMaterial(material, uv, MaterialSampler, color, metallic, roughness, emissionAO);
    float opacity = saturate(color.a * MaterialAlpha);
    clip(opacity - (1.0f / 255.0f));
    float3 toLight = ShadowLightPositionType.w < 0.5f ? ShadowLightDirection.xyz :
        ShadowLightPositionType.xyz - worldPosition;
    float cosIncident = dot(normalize(normalWS), normalize(toLight));
    // One entry surface represents a slab, including its exit interface.
    // Explicit two-sided materials also support open sheets lit from the back.
    if ((MaterialIsTransparent & 2) == 0 && ((material.Textures.w >> 2) & 3u) != 1u) clip(cosIncident);
    cosIncident = max(abs(cosIncident), 0.0001f);
    float eta = max(Transparent0.x, 1.0f);
    float cosTransmitted = sqrt(max(0.0f, 1.0f - (1.0f - cosIncident * cosIncident) / (eta * eta)));
    float rs = (cosIncident - eta * cosTransmitted) / max(cosIncident + eta * cosTransmitted, 0.0001f);
    float rp = (eta * cosIncident - cosTransmitted) / max(eta * cosIncident + cosTransmitted, 0.0001f);
    float fresnel = saturate(0.5f * (rs * rs + rp * rp));
    float thickness = max(Transparent1.x, 0.0f);
    float pathLength = thickness / max(cosTransmitted, 0.0001f);
    float3 absorption = max(Transparent1.yzw, 0.0f);
    // Base color is transmission tint at the authored normal-incidence thickness.
    float3 tintAbsorption = -log(clamp(color.rgb, 0.0001f, 1.0f)) / max(thickness, 0.0001f);
    float3 beer = exp(-(absorption + tintAbsorption) * pathLength);
    float3 slab = (1.0f - fresnel) * (1.0f - fresnel) * beer /
        max(1.0f - fresnel * fresnel * beer * beer, 0.0001f);
    float3 transmission = saturate(Transparent0.y) * (1.0f - saturate(metallic)) * slab;
    return float4(saturate(lerp(1.0f, transmission, opacity)), 1.0f);
}
