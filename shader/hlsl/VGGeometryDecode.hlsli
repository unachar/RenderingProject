#ifndef VG_GEOMETRY_DECODE
#define VG_GEOMETRY_DECODE
// VGC1 wire layout is defined by virtualgeometrycompression.cpp. Every raw Load is aligned.
uint VGLoadU8(ByteAddressBuffer buffer, uint address)
{
    return (buffer.Load(address & ~3u) >> ((address & 3u)*8u)) & 255u;
}
uint VGLoadU16(ByteAddressBuffer buffer, uint address)
{
    // Works even when the half-word crosses a DWORD boundary.
    return VGLoadU8(buffer, address) | (VGLoadU8(buffer, address + 1u) << 8u);
}
float VGSign(float value)
{
    return value < 0 ? -1.0 : 1.0;
}
float3 VGNormalize(float3 value)
{
    float scale = max(abs(value.x), max(abs(value.y), abs(value.z)));
    if (scale <= 1e-30)
    {
        return float3(0, 1, 0);
    }
    return normalize(value / scale);
}
int VGSigned(uint value, uint bits)
{
    uint signBit = 1u << (bits - 1u);
    return int(value & (signBit - 1u)) - int(value & signBit);
}
float3 VGOctDecode(uint packed)
{
    float2 xy = max(-1.0, float2(VGSigned(packed & 65535u, 16), VGSigned(packed >> 16u, 16)) / 32767.0);
    float z = 1.0 - abs(xy.x) - abs(xy.y);
    if (z < 0)
    {
        xy = (1.0 - abs(xy.yx)) * float2(VGSign(xy.x), VGSign(xy.y));
    }
    return VGNormalize(float3(xy, z));
}
// Returns codec, byte offset, stride; absent attributes return codec UINT_MAX.
uint3 VGStream(ByteAddressBuffer buffer, uint base, uint semantic)
{
    uint count = buffer.Load(base + 28u);
    for (uint stream = 0; stream < count; ++stream)
    {
        uint4 descriptor = buffer.Load4(base + 56u + stream*16u);
        if (descriptor.x == semantic)
        {
            return descriptor.yzw;
        }
    }
    return uint3(0xffffffffu, 0, 0);
}
float3 VGDecodePosition(ByteAddressBuffer buffer, uint base, uint vertex)
{
    // Position is always descriptor zero, avoiding a directory search on the hot path.
    uint3 stream = buffer.Load3(base + 60u);
    uint address = base + stream.y + vertex*stream.z;
    if (stream.x == 0)
    {
        return asfloat(buffer.Load3(address));
    }
    uint3 quantized = uint3(VGLoadU16(buffer, address), VGLoadU16(buffer, address + 2u), VGLoadU16(buffer, address + 4u));
    precise float3 delta = float3(quantized) * asfloat(buffer.Load3(base + 44u));
    precise float3 position = asfloat(buffer.Load3(base + 32u)) + delta;
    return position;
}
float3 VGDecodeNormal(ByteAddressBuffer buffer, uint base, uint vertex)
{
    uint3 stream = VGStream(buffer, base, 1);
    if (stream.x == 0xffffffffu)
    {
        return float3(0, 1, 0);
    }
    uint address = base + stream.y + vertex*stream.z;
    if (stream.x == 2)
    {
        return VGOctDecode(buffer.Load(address));
    }
    return VGNormalize(asfloat(buffer.Load3(address)));
}
float4 VGDecodeTangent(ByteAddressBuffer buffer, uint base, uint vertex, float3 normal)
{
    uint3 stream = VGStream(buffer, base, 2);
    float4 tangent = float4(1, 0, 0, 1);
    uint address = base + stream.y + vertex*stream.z;
    if (stream.x == 0)
    {
        tangent = asfloat(buffer.Load4(address));
    }
    else if (stream.x == 3)
    {
        uint packed = buffer.Load(address);
        tangent.xyz = VGNormalize(float3(VGSigned(packed & 1023u, 10),
            VGSigned((packed >> 10u) & 1023u, 10), VGSigned((packed >> 20u) & 1023u, 10)) / 511.0);
        tangent.w = (packed >> 30u) == 3u ? -1.0 : 1.0;
    }
    tangent.xyz -= normal * dot(normal, tangent.xyz);
    if (dot(abs(tangent.xyz), float3(1, 1, 1)) < 1e-6)
    {
        float3 helper = float3(0, 0, 1);
        if (abs(normal.z) > 0.9)
        {
            helper = float3(0, 1, 0);
        }
        tangent.xyz = cross(helper, normal);
    }
    tangent.xyz = VGNormalize(tangent.xyz);
    return tangent;
}
float2 VGDecodeTexcoord(ByteAddressBuffer buffer, uint base, uint vertex, uint channel)
{
    uint3 stream = VGStream(buffer, base, 8u + channel);
    if (stream.x == 0xffffffffu)
    {
        return 0;
    }
    uint address = base + stream.y + vertex*stream.z;
    if (stream.x == 4)
    {
        uint packed = buffer.Load(address);
        return float2(f16tof32(packed & 65535u), f16tof32(packed >> 16u));
    }
    return asfloat(buffer.Load2(address));
}
float4 VGDecodeColor(ByteAddressBuffer buffer, uint base, uint vertex)
{
    uint3 stream = VGStream(buffer, base, 3);
    if (stream.x == 0xffffffffu)
    {
        return 1;
    }
    uint address = base + stream.y + vertex*stream.z;
    if (stream.x == 5)
    {
        uint packed = buffer.Load(address);
        return float4(packed & 255u, (packed >> 8u) & 255u, (packed >> 16u) & 255u, packed >> 24u) / 255.0;
    }
    return asfloat(buffer.Load4(address));
}
uint3 VGLoadTriangle(ByteAddressBuffer buffer, uint base, uint triangle)
{
    uint width = buffer.Load(base + 20u);
    uint address = base + buffer.Load(base + 24u) + triangle*3u*width;
    if (width == 1u)
    {
        return uint3(VGLoadU8(buffer, address), VGLoadU8(buffer, address + 1u), VGLoadU8(buffer, address + 2u));
    }
    return uint3(VGLoadU16(buffer, address), VGLoadU16(buffer, address + 2u), VGLoadU16(buffer, address + 4u));
}
struct VGDecodedVertex
{
    float3 Position;
    float3 Normal;
    float4 Tangent;
    float2 UV0;
    float4 Color;
};
VGDecodedVertex VGDecodeVertex(ByteAddressBuffer buffer, uint base, uint vertex)
{
    VGDecodedVertex result;
    result.Position = VGDecodePosition(buffer, base, vertex);
    result.Normal = VGDecodeNormal(buffer, base, vertex);
    result.Tangent = VGDecodeTangent(buffer, base, vertex, result.Normal);
    result.UV0 = VGDecodeTexcoord(buffer, base, vertex, 0);
    result.Color = VGDecodeColor(buffer, base, vertex);
    return result;
}
#endif
