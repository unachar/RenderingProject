#include "rendering/gpudriven/indirectcommand.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>

int main()
{
    static_assert(sizeof(GPUIndirectDrawCommand) == 24);
    static_assert(offsetof(GPUIndirectDrawCommand, Draw) == 4);
    static_assert(sizeof(GPUDrivenGeometryData) == 16);
    assert(GpuDrivenCapacity(0, 0) == 1024);
    assert(GpuDrivenCapacity(1024, 1024) == 1024);
    assert(GpuDrivenCapacity(1024, 10000) == 16384);
    assert(GpuDrivenCapacity(16384, 50000) == 65536);
    bool overflow = false;
    try { GpuDrivenCapacity(0, UINT32_MAX); }
    catch (const std::overflow_error&) { overflow = true; }
    assert(overflow);
    GPUDrivenGeometryData first = ReserveGpuDrivenGeometry(0, 0, 4, 6);
    GPUDrivenGeometryData second = ReserveGpuDrivenGeometry(4, 6, 100, 300);
    assert(first.BaseVertex == 0 && first.StartIndex == 0 && first.IndexCount == 6);
    assert(second.BaseVertex == 4 && second.StartIndex == 6 && second.IndexCount == 300);
    overflow = false;
    try { ReserveGpuDrivenGeometry(INT32_MAX, 0, 10, 3); }
    catch (const std::overflow_error&) { overflow = true; }
    assert(overflow);
    overflow = false;
    try { ReserveGpuDrivenGeometry(0, UINT32_MAX, 3, 3); }
    catch (const std::overflow_error&) { overflow = true; }
    assert(overflow);
    std::puts("GPU Driven layout/capacity/geometry tests: PASS");
}
