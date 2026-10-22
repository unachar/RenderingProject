// GpuInstanceCullLodCS の LOD ごとの件数から、ExecuteIndirect の引数（D3D12_DRAW_INDEXED_ARGUMENTS × 3）を作る。
ByteAddressBuffer LodCounts : register(t0);
RWByteAddressBuffer IndirectArguments : register(u0);

cbuffer ArgumentConstants : register(b0)
{
    uint DrawCount0;
    uint DrawCount1;
    uint DrawCount2;
    uint IndexedDraw;
    uint CountOffset;     // バイト
    uint ArgumentsOffset; // バイト
};

void BuildInstanceArguments()
{
    uint drawCounts[3] = {DrawCount0, DrawCount1, DrawCount2};
    [unroll] for (uint lod = 0; lod < 3; ++lod)
    {
        uint baseOffset = ArgumentsOffset + lod * 20;
        uint instanceCount = LodCounts.Load(CountOffset + lod * 4);
        IndirectArguments.Store(baseOffset + 0, drawCounts[lod]);
        IndirectArguments.Store(baseOffset + 4, instanceCount);
        IndirectArguments.Store(baseOffset + 8, 0);
        IndirectArguments.Store(baseOffset + 12, 0);
        IndirectArguments.Store(baseOffset + 16, 0);
    }
}
[numthreads(1, 1, 1)] void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    BuildInstanceArguments();
}
