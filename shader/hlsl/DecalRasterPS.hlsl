// Stage 1D: DBuffer decal rasterization. One fullscreen pass per decal:
// reconstruct the receiver's world position from the scene depth,
// transform it into decal-local (unit box) space, clip outside the box,
// sample the decal material and write albedo / world normal /
// roughness+mask into the DBuffer. The decal record arrives as root
// constants (112 bytes = 28 DWORDs); blending (configured in the PSO)
// accumulates decals in priority order, and the composite pass merges
// the DBuffer into the GBuffer before lighting.
//
// Conventions (see docs/rendering-technology-stage1d-design.md):
// - WorldToLocal is row-major; world positions transform with
//   mul(float4(p, 1), M). Local +Z is the axis pointing out of the
//   receiver surface toward the projector.
// - Scene depth is reverse-Z (0 = far/sky). GBuffer normals are stored
//   as worldNormal * 0.5 + 0.5.
#include "MaterialTextureHeap.hlsli"

cbuffer DecalFrame : register(b0)
{
    float4x4 InvViewProjection;
    float4 CameraPosition;
}

cbuffer DecalData : register(b1)
{
    float4x4 DecalWorldToLocal;
    float4 DecalExtentsOpacity;
    uint4 DecalTextures;
    float4 DecalFade;
}

Texture2D<float> SceneDepth : register(t0);
Texture2D GBufferNormal : register(t1);
SamplerState LinearClamp : register(s0);

struct DecalPSInput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

struct DecalPSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 RoughnessMask : SV_Target2;
};

DecalPSOutput main(DecalPSInput input)
{
    int2 pixel = int2(input.Position.xy);
    float depth = SceneDepth.Load(int3(pixel, 0));
    clip(depth - 1e-7f);

    float4 clipPosition = float4(input.TexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), depth, 1.0f);
    float4 world = mul(clipPosition, InvViewProjection);
    float3 worldPosition = world.xyz / world.w;

    float3 local = mul(float4(worldPosition, 1.0f), DecalWorldToLocal).xyz;
    clip(1.0f - abs(local.x));
    clip(1.0f - abs(local.y));
    clip(1.0f - abs(local.z));
    float2 uv = local.xy * 0.5f + 0.5f;

    float3 receiverNormal = normalize(GBufferNormal.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    // Gradient of local z in world space is the third row of the 3x3.
    float3 projectionAxis = normalize(float3(DecalWorldToLocal[2][0], DecalWorldToLocal[2][1],
                                             DecalWorldToLocal[2][2]));
    float facing = dot(receiverNormal, projectionAxis);
    float angleFade = smoothstep(DecalFade[1], DecalFade[0], facing);
    clip(angleFade - 1.0f / 255.0f);

    float edgeFade = (1.0f - smoothstep(0.85f, 1.0f, abs(local.x))) *
                     (1.0f - smoothstep(0.85f, 1.0f, abs(local.y))) *
                     (1.0f - smoothstep(0.85f, 1.0f, abs(local.z)));

    float4 albedo = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (DecalTextures[0] != 0xFFFFFFFFu)
    {
        albedo = LoadMaterialTexture(DecalTextures[0]).Sample(LinearClamp, uv);
    }
    float strength = albedo.a * DecalExtentsOpacity[3] * angleFade * edgeFade;
    clip(strength - 1.0f / 255.0f);

    float3 decalNormal = receiverNormal;
    if (DecalTextures[1] != 0xFFFFFFFFu)
    {
        float3 tangentNormal = LoadMaterialTexture(DecalTextures[1]).Sample(LinearClamp, uv).xyz * 2.0f - 1.0f;
        // Approximate local->world with the transposed 3x3 (exact for
        // the rotation part of the projector transform).
        float3x3 worldToLocal3 = float3x3(DecalWorldToLocal[0].xyz, DecalWorldToLocal[1].xyz,
                                          DecalWorldToLocal[2].xyz);
        decalNormal = normalize(mul(tangentNormal, transpose(worldToLocal3)));
    }

    float roughness = DecalFade[2];
    if (DecalTextures[2] != 0xFFFFFFFFu)
    {
        roughness = LoadMaterialTexture(DecalTextures[2]).Sample(LinearClamp, uv).r;
    }

    DecalPSOutput output;
    output.Albedo = float4(albedo.rgb, strength);
    output.Normal = float4(decalNormal, strength);
    output.RoughnessMask = float4(roughness, 0.0f, 0.0f, strength);
    return output;
}
