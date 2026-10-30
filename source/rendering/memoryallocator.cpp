#include "pch.h"
#include "memoryallocator.h"
#include "graphicslog.h"
#include "renderersettings.h"
#include "../../External/D3D12MemoryAllocator/D3D12MemAlloc.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace MemoryAllocator
{
    namespace
    {
        D3D12MA::Allocator* g_Allocator = nullptr;
        std::mutex g_Mutex;
        std::unordered_map<ID3D12Resource*, D3D12MA::Allocation*> g_Allocations;

        // Owns one allocation for exactly as long as the resource it is
        // attached to lives (released by the resource's private-data slot).
        class AllocationLifetime final : public IUnknown
        {
          public:
            explicit AllocationLifetime(D3D12MA::Allocation* allocation) : m_Allocation(allocation) {}

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
            {
                if (!output)
                {
                    return E_POINTER;
                }
                *output = nullptr;
                if (iid != __uuidof(IUnknown))
                {
                    return E_NOINTERFACE;
                }
                *output = static_cast<IUnknown*>(this);
                AddRef();
                return S_OK;
            }

            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return ++m_References;
            }

            ULONG STDMETHODCALLTYPE Release() override
            {
                const ULONG references = --m_References;
                if (references == 0)
                {
                    if (m_Allocation)
                    {
                        m_Allocation->Release();
                    }
                    delete this;
                }
                return references;
            }

          private:
            std::atomic<ULONG> m_References{1};
            D3D12MA::Allocation* m_Allocation = nullptr;
        };

        constexpr GUID g_AllocationLifetimeId = {
            0x51d12a11, 0x0c5e, 0x4a7b, {0x9d, 0x3f, 0x2a, 0x71, 0xc4, 0x88, 0x0b, 0x5e}};
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

    ID3D12Resource* CreateResourceAutoLifetime(const D3D12_RESOURCE_DESC& description,
                                               D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue)
    {
        ID3D12Resource* resource = CreateResource(description, initialState, clearValue);
        if (!resource)
        {
            return nullptr;
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
        if (!allocation)
        {
            resource->Release();
            return nullptr;
        }
        auto* lifetime = new AllocationLifetime(allocation);
        const HRESULT attached = resource->SetPrivateDataInterface(g_AllocationLifetimeId, lifetime);
        lifetime->Release();
        if (FAILED(attached))
        {
            // The holder released the allocation when its last reference went
            // away above; the just-created resource was never used, so drop
            // it too and let the caller fall back to a committed resource.
            resource->Release();
            return nullptr;
        }
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
