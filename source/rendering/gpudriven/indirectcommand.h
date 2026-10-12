#pragma once
#include <d3d12.h>
#include <cstdint>
#include <cstddef>
#include <stdexcept>

struct GPUIndirectDrawCommand
{
    uint32_t InstanceID;
    D3D12_DRAW_INDEXED_ARGUMENTS Draw;
};
static_assert(sizeof(GPUIndirectDrawCommand) == 24);
static_assert(offsetof(GPUIndirectDrawCommand, Draw) == 4);
static_assert(offsetof(GPUIndirectDrawCommand, Draw.BaseVertexLocation) == 16);

// Separate from the Phase 2 mesh record: existing geometry addresses remain valid.
struct GPUDrivenGeometryData
{
    uint32_t StartIndex = 0;
    uint32_t IndexCount = 0;
    int32_t BaseVertex = 0;
    uint32_t Ready = 0;
};
static_assert(sizeof(GPUDrivenGeometryData) == 16);

inline uint32_t GpuDrivenCapacity(uint32_t current, uint32_t required)
{
    uint32_t capacity = current > 1024 ? current : 1024;
    while (capacity < required)
    {
        if (capacity > UINT32_MAX / 2)
            throw std::overflow_error("GPU driven capacity overflow");
        capacity *= 2;
    }
    return capacity;
}

inline GPUDrivenGeometryData ReserveGpuDrivenGeometry(uint64_t vertices, uint64_t indices,
                                                      uint32_t vertexCount, uint32_t indexCount)
{
    // IA view byte sizes are UINT; vertex format is the existing 48-byte StaticModelVertex.
    if ((vertices + vertexCount) * 48 > UINT32_MAX || (indices + indexCount) * 4 > UINT32_MAX ||
        vertices + vertexCount > INT32_MAX || indices + indexCount > UINT32_MAX)
        throw std::overflow_error("GPU driven geometry arena exceeds IA limits");
    return {static_cast<uint32_t>(indices), indexCount, static_cast<int32_t>(vertices), 1};
}
