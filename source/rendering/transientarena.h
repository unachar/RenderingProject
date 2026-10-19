#pragma once
#include "pch.h"
#include <algorithm>
#include <vector>

// 所有者は、送信した GPU 処理が終わるまでこのアリーナを保持する必要がある。
// Build/Reset は所有者のフレームフェンスが完了した後だけ呼び出せる。
class TransientArena final
{
  public:
    struct Request
    {
        D3D12_RESOURCE_DESC Desc{};
        D3D12_RESOURCE_STATES Initial = D3D12_RESOURCE_STATE_COMMON;
        UINT First = 0, Last = 0;
    };

    bool Build(ID3D12Device* device, const std::vector<Request>& requests)
    {
        Reset();
        if (!device)
        {
            return false;
        }

        struct Slot
        {
            UINT Last;
            UINT64 Size, Alignment, Offset;
        };

        std::vector<Slot> slots;
        std::vector<UINT> assignment(requests.size()), order(requests.size());
        for (UINT i = 0; i < order.size(); ++i)
        {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](UINT a, UINT b) {
            return requests[a].First < requests[b].First;
        });
        for (UINT i : order)
        {
            const auto& request = requests[i];
            if (request.First > request.Last)
            {
                return false;
            }
            const auto info = device->GetResourceAllocationInfo(0, 1, &request.Desc);
            if (info.SizeInBytes == UINT64_MAX || !info.Alignment)
            {
                return false;
            }
            UINT slot = UINT(slots.size());
            for (UINT j = 0; j < slots.size(); ++j)
            {
                if (slots[j].Last < request.First)
                {
                    slot = j;
                    break;
                }
            }
            if (slot == slots.size())
            {
                slots.push_back({request.Last, info.SizeInBytes, info.Alignment, 0});
            }
            else
            {
                slots[slot].Last = request.Last;
                slots[slot].Size = (std::max)(slots[slot].Size, info.SizeInBytes);
                slots[slot].Alignment = (std::max)(slots[slot].Alignment, info.Alignment);
            }
            assignment[i] = slot;
        }
        UINT64 bytes = 0;
        for (auto& slot : slots)
        {
            bytes = (bytes + slot.Alignment - 1) & ~(slot.Alignment - 1);
            slot.Offset = bytes;
            bytes += slot.Size;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const bool tier2 =
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
            options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
        D3D12_HEAP_DESC heap{};
        heap.SizeInBytes = bytes;
        heap.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        heap.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        const bool placed = tier2 && bytes && SUCCEEDED(device->CreateHeap(&heap, IID_PPV_ARGS(&m_Heap)));
        m_Resources.resize(requests.size());
        m_Slots = assignment;
        m_Active.assign(slots.size(), nullptr);
        for (UINT i = 0; i < requests.size(); ++i)
        {
            const auto& request = requests[i];
            HRESULT hr = E_FAIL;
            if (placed)
            {
                hr = device->CreatePlacedResource(m_Heap.Get(), slots[assignment[i]].Offset, &request.Desc,
                                                  request.Initial, nullptr, IID_PPV_ARGS(&m_Resources[i]));
            }
            if (!placed)
            {
                hr = device->CreateCommittedResource(&heap.Properties, D3D12_HEAP_FLAG_NONE, &request.Desc,
                                                     request.Initial, nullptr, IID_PPV_ARGS(&m_Resources[i]));
            }
            if (FAILED(hr))
            {
                Reset();
                return false;
            }
        }
        m_Placed = placed;
        return true;
    }

    ID3D12Resource* Get(UINT index) const
    {
        return index < m_Resources.size() ? m_Resources[index].Get() : nullptr;
    }

    void Activate(UINT index, ID3D12GraphicsCommandList* list)
    {
        if (!m_Placed || index >= m_Resources.size() || !list)
        {
            return;
        }
        auto*& before = m_Active[m_Slots[index]];
        auto* after = m_Resources[index].Get();
        if (before != after)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Aliasing(before, after);
            list->ResourceBarrier(1, &barrier);
            before = after;
        }
    }

    void Reset()
    {
        m_Resources.clear();
        m_Heap.Reset();
        m_Slots.clear();
        m_Active.clear();
        m_Placed = false;
    }

  private:
    ComPtr<ID3D12Heap> m_Heap;
    std::vector<ComPtr<ID3D12Resource>> m_Resources;
    std::vector<UINT> m_Slots;
    std::vector<ID3D12Resource*> m_Active;
    bool m_Placed = false;
};
