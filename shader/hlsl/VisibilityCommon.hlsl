#ifndef VISIBILITY_COMMON_HLSL
#define VISIBILITY_COMMON_HLSL

uint PackVisibilityUnorm4(float4 value)
{
    uint4 packed = (uint4)round(saturate(value) * 255.0f);
    return packed.x | (packed.y << 8u) | (packed.z << 16u) | (packed.w << 24u);
}

float2 EncodeVisibilityOctahedron(float3 normal)
{
    normal /= max(abs(normal.x) + abs(normal.y) + abs(normal.z), 0.000001f);
    float2 encoded = normal.xy;
    if (normal.z < 0.0f)
    {
        encoded = (1.0f - abs(encoded.yx)) * (encoded.xy >= 0.0f ? 1.0f : -1.0f);
    }
    return encoded * 0.5f + 0.5f;
}

uint4 PackVisibilityPayload(
    float3 baseColor,
    float3 normal,
    float depth,
    float metallic,
    float roughness,
    float fresnel,
    float materialPartId,
    float shadowThreshold,
    float shadowSoftness,
    float shadowStrength)
{
    float2 octahedron = EncodeVisibilityOctahedron(normalize(normal));
    uint2 packedNormal = (uint2)round(saturate(octahedron) * 65535.0f);
    uint packedDepth = (uint)round(saturate(depth) * 65535.0f);
    uint packedMetallic = (uint)round(saturate(metallic) * 255.0f);
    uint packedRoughness = (uint)round(saturate(roughness) * 255.0f);
    uint packedFresnel = (uint)round(saturate(fresnel) * 255.0f);
    uint packedPart = (uint)round(clamp(materialPartId, 0.0f, 255.0f));
    uint packedThreshold = (uint)round(saturate(shadowThreshold) * 255.0f);
    uint packedSoftness = (uint)round(saturate(shadowSoftness) * 127.0f);

    return uint4(
        PackVisibilityUnorm4(float4(baseColor, saturate(shadowStrength))),
        packedNormal.x | (packedNormal.y << 16u),
        packedDepth | (packedMetallic << 16u) | (packedRoughness << 24u),
        packedFresnel | (packedPart << 8u) | (packedThreshold << 16u) |
            (packedSoftness << 24u) | 0x80000000u);
}

#endif
