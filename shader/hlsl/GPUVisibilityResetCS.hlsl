#include "GPUVisibilityCommon.hlsli"
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 16) gStatistics.Store(id.x * 4, 0);
}
