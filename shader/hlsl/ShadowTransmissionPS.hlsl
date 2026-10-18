#define SHADER_3D
#include "common.hlsl"
#include "MaterialSurface.hlsli"
#include "PhysicalGlass.hlsli"

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
    float fresnel = GlassFresnel(cosIncident,1.0f,eta);
    float thickness = max(Transparent1.x,0.0f);
    float pathLength = thickness / max(cosTransmitted,0.0001f);
    float3 beer = GlassBeer(color.rgb,Transparent1.yzw,thickness,pathLength);
    float3 reflected,slab;
    GlassSlabWeights(fresnel,beer,reflected,slab);
    float3 transmission = saturate(Transparent0.y) * (1.0f - saturate(metallic)) * slab;
    return float4(saturate(lerp(1.0f, transmission, opacity)), 1.0f);
}
