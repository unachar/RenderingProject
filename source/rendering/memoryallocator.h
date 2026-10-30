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

    // Like CreateResource, but the allocation's lifetime is bound to the
    // resource itself through ID3D12Object private data (the same pattern
    // TextureManager uses for TextureHeapLifetime): the allocation is freed
    // when the resource's last reference goes away, so callers can hold the
    // result in a plain ComPtr and must NOT call ReleaseResource() on it.
    ID3D12Resource* CreateResourceAutoLifetime(const D3D12_RESOURCE_DESC& description,
                                               D3D12_RESOURCE_STATES initialState,
                                               const D3D12_CLEAR_VALUE* clearValue = nullptr);

    struct Statistics
    {
        uint64_t AllocatedBytes = 0;
        uint64_t HeapBytes = 0;
        uint32_t AllocationCount = 0;
        uint32_t HeapCount = 0;
    };
    Statistics GetStatistics();

    // Writes the current statistics to the graphics log (no-op when the
    // allocator holds nothing). LogStatisticsPeriodically() throttles itself
    // to roughly once per 900 frames for per-frame call sites; both exist so
    // the D3D12MA A/B (heap count / heap bytes vs committed) can be read off
    // the log without a debugger.
    void LogStatistics();
    void LogStatisticsPeriodically();
} // namespace MemoryAllocator
