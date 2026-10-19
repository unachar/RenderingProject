#include "MeshletCullingCommon.hlsli"

[numthreads(64, 1, 1)] void Reset(uint3 id : SV_DispatchThreadID) {
    if (id.x < 16)
    {
        Statistics.Store(id.x * 4, 0);
    }
}[numthreads(1, 1, 1)] void main()
{
    uint count = min(Statistics.Load(4), Capacity);
    uint x = min(count, 65535u);
    uint y = count ? (count + x - 1) / x : 1;
    Arguments.Store3(0, uint3(x, y, 1));
}
