#include "../shader/hlsl/VGGeometryDecode.hlsli"
ByteAddressBuffer Encoded : register(t0);
RWByteAddressBuffer Decoded : register(u0);
cbuffer Parameters : register(b0) { uint VertexCount; uint TriangleCount; }
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x < VertexCount)
    {
        VGDecodedVertex vertex = VGDecodeVertex(Encoded, 0, thread.x);
        uint address = thread.x*80u;
        Decoded.Store4(address, asuint(float4(vertex.Position, 0)));
        Decoded.Store4(address + 16u, asuint(float4(vertex.Normal, 0)));
        Decoded.Store4(address + 32u, asuint(vertex.Tangent));
        Decoded.Store4(address + 48u, asuint(float4(vertex.UV0, 0, 0)));
        Decoded.Store4(address + 64u, asuint(vertex.Color));
    }
    if (thread.x < TriangleCount)
    {
        Decoded.Store4(VertexCount*80u + thread.x*16u, uint4(VGLoadTriangle(Encoded, 0, thread.x), 0));
    }
}
