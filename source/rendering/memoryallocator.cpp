#include "pch.h"
#include "memoryallocator.h"
#include "graphicslog.h"
#include "renderersettings.h"
#include "../../External/D3D12MemoryAllocator/D3D12MemAlloc.h"
#include <mutex>
#include <unordered_map>

namespace MemoryAllocator
{
    namespace
    {
        D3D12MA::Allocator* g_Allocator = nullptr;
        std::mutex g_Mutex;
        std::unordered_map<ID3D12Resource*, D3D12MA::Allocation*> g_Allocations;
    } // namespace

    bool Initialize(ID3D12Device* device, IDXGIAdapter* adapter)
    {
        if (g_Allocator)
        {
            return true;
        }
        if (!device)
        {
            return false;
        }
        ComPtr<IDXGIAdapter> resolvedAdapter;
        if (adapter)
        {
            resolvedAdapter = adapter;
        }
        else
        {
            ComPtr<IDXGIDevice> dxgiDevice;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
            {
                dxgiDevice->GetAdapter(&resolvedAdapter);
            }
        }
        if (!resolvedAdapter)
        {
            WriteGraphicsLog("D3D12MA: no adapter available, allocator disabled\n");
            return false;
        }
        D3D12MA::ALLOCATOR_DESC description{};
        description.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        description.pDevice = device;
        description.pAdapter = resolvedAdapter.Get();
        if (FAILED(D3D12MA::CreateAllocator(&description, &g_Allocator)))
        {
            WriteGraphicsLog("D3D12MA: CreateAllocator failed, allocator disabled\n");
            g_Allocator = nullptr;
            return false;
        }
        WriteGraphicsLog("D3D12MA: allocator created\n");
        return true;
    }

    void Shutdown()
    {
        std::lock_guard lock(g_Mutex);
        for (auto& [resource, allocation] : g_Allocations)
        {
            if (allocation)
            {
                allocation->Release();
            }
            if (resource)
            {
                resource->Release();
            }
        }
        g_Allocations.clear();
        if (g_Allocator)
        {
            g_Allocator->Release();
            g_Allocator = nullptr;
        }
    }

    bool Available()
    {
        return g_Allocator != nullptr;
    }

    bool Enabled()
    {
        return g_Allocator != nullptr && RendererSettings::GetUseD3D12MemoryAllocator();
    }

    ID3D12Resource* CreateResource(const D3D12_RESOURCE_DESC& description, D3D12_RESOURCE_STATES initialState,
                                   const D3D12_CLEAR_VALUE* clearValue)
    {
        if (!Enabled())
        {
            return nullptr;
        }
        D3D12MA::ALLOCATION_DESC allocationDescription{};
        allocationDescription.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
        allocationDescription.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12MA::Allocation* allocation = nullptr;
        ID3D12Resource* resource = nullptr;
        const HRESULT result = g_Allocator->CreateResource(&allocationDescription, &description, initialState,
                                                           clearValue, &allocation, IID_PPV_ARGS(&resource));
        if (FAILED(result) || !resource || !allocation)
        {
            if (allocation)
            {
                allocation->Release();
            }
            if (resource)
            {
                resource->Release();
            }
            return nullptr;
        }
        std::lock_guard lock(g_Mutex);
        g_Allocations[resource] = allocation;
        return resource;
    }

    void ReleaseResource(ID3D12Resource* resource)
    {
        if (!resource)
        {
            return;
        }
        D3D12MA::Allocation* allocation = nullptr;
        {
            std::lock_guard lock(g_Mutex);
            auto found = g_Allocations.find(resource);
            if (found != g_Allocations.end())
            {
                allocation = found->second;
                g_Allocations.erase(found);
            }
        }
        if (allocation)
        {
            allocation->Release();
        }
        resource->Release();
    }

    Statistics GetStatistics()
    {
        Statistics statistics{};
        if (!g_Allocator)
        {
            return statistics;
        }
        D3D12MA::TotalStatistics totals{};
        g_Allocator->CalculateStatistics(&totals);
        for (const auto& heapType : totals.HeapType)
        {
            statistics.AllocatedBytes += heapType.Stats.AllocationBytes;
            statistics.HeapBytes += heapType.Stats.BlockBytes;
            statistics.AllocationCount += heapType.Stats.AllocationCount;
            statistics.HeapCount += heapType.Stats.BlockCount;
        }
        return statistics;
    }
} // namespace MemoryAllocator
