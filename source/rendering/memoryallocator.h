#pragma once
#include <cstdint>
#include <d3d12.h>

// Thin adapter over D3D12MemoryAllocator (External/D3D12MemoryAllocator, MIT).
// Routes persistent resources through pooled heaps instead of one committed
// heap per resource. Disabled by default
// (RendererSettings::GetUseD3D12MemoryAllocator); callers must keep their
// committed-resource path as the fallback for a nullptr return.
namespace MemoryAllocator
{
    bool Initialize(ID3D12Device* device, IDXGIAdapter* adapter);
    void Shutdown();
    bool Available();
    bool Enabled();

    // Creates a resource owned by the allocator. The returned resource holds
    // one reference and must be released with ReleaseResource() (which also
    // frees the allocation), not by a bare Release(). Returns nullptr when
    // disabled, unavailable, or on failure.
    ID3D12Resource* CreateResource(const D3D12_RESOURCE_DESC& description, D3D12_RESOURCE_STATES initialState,
                                   const D3D12_CLEAR_VALUE* clearValue = nullptr);
    void ReleaseResource(ID3D12Resource* resource);

    struct Statistics
    {
        uint64_t AllocatedBytes = 0;
        uint64_t HeapBytes = 0;
        uint32_t AllocationCount = 0;
        uint32_t HeapCount = 0;
    };
    Statistics GetStatistics();
} // namespace MemoryAllocator
