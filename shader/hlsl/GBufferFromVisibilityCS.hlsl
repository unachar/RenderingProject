Texture2D<uint4> VisibilityBuffer : register(t0);
RWTexture2D<float4> OutBaseColor : register(u0);
RWTexture2D<float4> OutNormal : register(u1);
RWTexture2D<float> OutDepth : register(u2);
RWTexture2D<float4> OutMaterial : register(u3);
RWTexture2D<float4> OutShadow : register(u4);

cbuffer VisibilityConstants : register(b0)
{
    uint2 OutputExtent;
    uint2 Padding;
};

float2 UnpackUnorm16x2(uint value)
{
    return float2(value & 0xffffu, value >> 16u) / 65535.0f;
}

float3 DecodeOctahedron(float2 encoded)
{
    float2 f = encoded * 2.0f - 1.0f;
    float3 normal = float3(f, 1.0f - abs(f.x) - abs(f.y));
    if (normal.z < 0.0f)
    {
        normal.xy = (1.0f - abs(normal.yx)) * (normal.xy >= 0.0f ? 1.0f : -1.0f);
    }
    return normalize(normal);
}

[numthreads(8, 4, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= OutputExtent)) return;

    uint4 payload = VisibilityBuffer.Load(int3(pixel, 0));
    if ((payload.w & 0x80000000u) == 0u)
    {
        OutBaseColor[pixel] = float4(0.1f, 0.2f, 0.4f, 1.0f);
        OutNormal[pixel] = float4(0.5f, 1.0f, 0.5f, 1.0f);
        OutDepth[pixel] = 1.0f;
        OutMaterial[pixel] = 0.0f;
        OutShadow[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float4 packedColor = float4(
        payload.x & 0xffu,
        (payload.x >> 8u) & 0xffu,
        (payload.x >> 16u) & 0xffu,
        (payload.x >> 24u) & 0xffu) / 255.0f;
    float3 normal = DecodeOctahedron(UnpackUnorm16x2(payload.y));
    float depth = (payload.z & 0xffffu) / 65535.0f;
    float metallic = ((payload.z >> 16u) & 0xffu) / 255.0f;
    float roughness = ((payload.z >> 24u) & 0xffu) / 255.0f;
    float fresnel = (payload.w & 0xffu) / 255.0f;
    float materialPart = (float)((payload.w >> 8u) & 0xffu);
    float shadowThreshold = ((payload.w >> 16u) & 0xffu) / 255.0f;
    float shadowSoftness = ((payload.w >> 24u) & 0x7fu) / 127.0f;

    OutBaseColor[pixel] = float4(packedColor.rgb, 1.0f);
    OutNormal[pixel] = float4(normal * 0.5f + 0.5f, 1.0f);
    OutDepth[pixel] = depth;
    OutMaterial[pixel] = float4(metallic, roughness, fresnel, materialPart);
    OutShadow[pixel] = float4(shadowThreshold, shadowSoftness, packedColor.a, 1.0f);
}
