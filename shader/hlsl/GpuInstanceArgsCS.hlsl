ByteAddressBuffer LodCounts : register(t0);
RWByteAddressBuffer IndirectArguments : register(u0);

cbuffer ArgumentConstants : register(b0)
{
    uint DrawCount0;
    uint DrawCount1;
    uint DrawCount2;
    uint IndexedDraw;
};

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint drawCounts[3] = { DrawCount0, DrawCount1, DrawCount2 };
    [unroll]
    for (uint lod = 0; lod < 3; ++lod)
    {
        uint baseOffset = lod * 20;
        uint instanceCount = LodCounts.Load(lod * 4);
        IndirectArguments.Store(baseOffset + 0, drawCounts[lod]);
        IndirectArguments.Store(baseOffset + 4, instanceCount);
        IndirectArguments.Store(baseOffset + 8, 0);
        IndirectArguments.Store(baseOffset + 12, 0);
        IndirectArguments.Store(baseOffset + 16, 0);
    }
}
