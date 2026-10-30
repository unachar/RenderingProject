#include "virtualgeometrypagepool.h"
#include "memoryallocator.h"
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <thread>

namespace VirtualGeometryStreaming
{
    namespace
    {
        void Check(HRESULT result)
        {
            if (FAILED(result))
            {
                throw std::runtime_error("Geometry Page Copy Queue operation failed");
            }
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, D3D12_HEAP_TYPE type,
                                                      D3D12_RESOURCE_STATES state)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = type;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = 65536;
            description.Height = description.DepthOrArraySize = description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            // Resident pages are many small same-size DEFAULT buffers - a
            // natural fit for the pooled allocator when the flag is on.
            if (type == D3D12_HEAP_TYPE_DEFAULT)
            {
                if (ID3D12Resource* allocated =
                        MemoryAllocator::CreateResourceAutoLifetime(description, state, nullptr))
                {
                    resource.Attach(allocated);
                    return resource;
                }
            }
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                                  IID_PPV_ARGS(&resource)));
            return resource;
        }
    }

    void PagePool::Initialize(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE descriptors, uint32_t increment)
    {
        if (m_Device)
        {
            return;
        }
        m_Device = device;
        m_Descriptors = descriptors;
        m_Increment = increment;
        D3D12_COMMAND_QUEUE_DESC description{};
        description.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        Check(device->CreateCommandQueue(&description, IID_PPV_ARGS(&m_Queue)));
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence)));
    }

    uint64_t PagePool::Submit(LoadTicket ticket, std::span<const uint8_t> payload)
    {
        if (!m_Device || payload.size() != 65536 || ticket.Slot >= 16384 || ticket.Generation > UINT32_MAX)
        {
            throw std::runtime_error("Invalid Geometry Page transfer");
        }
        // 常駐状態とフェンスに基づく受付判定は呼び出し元が管理する。
        // シェーダー読み取り中のリソースへコピーせず、リソースを交換してキューの所有権を分離する。
        auto page = Buffer(m_Device.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        Transfer transfer;
        transfer.Upload = Buffer(m_Device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        Check(transfer.Upload->Map(0, &noRead, &mapped));
        std::memcpy(mapped, payload.data(), payload.size());
        transfer.Upload->Unmap(0, nullptr);
        Check(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&transfer.Allocator)));
        Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, transfer.Allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&transfer.Commands)));
        transfer.Commands->CopyBufferRegion(page.Get(), 0, transfer.Upload.Get(), 0, 65536);
        Check(transfer.Commands->Close());
        transfer.Fence = ++m_Submitted;
        if (m_Pages.size() <= ticket.Slot)
        {
            m_Pages.resize(size_t(ticket.Slot) + 1);
        }
        m_Pages[ticket.Slot] = page;
        // Signal が失敗する場合も含め、送信前にすべてを保持する。
        m_Transfers.push_back(std::move(transfer));
        ID3D12CommandList* list = m_Transfers.back().Commands.Get();
        m_Queue->ExecuteCommandLists(1, &list);
        Check(m_Queue->Signal(m_Fence.Get(), m_Submitted));
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.NumElements = 65536 / 4;
        view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor{m_Descriptors.ptr + SIZE_T(ticket.Slot) * m_Increment};
        m_Device->CreateShaderResourceView(page.Get(), &view, descriptor);
        return m_Submitted;
    }

    uint64_t PagePool::CompletedCopyFence() const
    {
        if (!m_Fence)
        {
            return 0;
        }
        uint64_t completed = m_Fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            throw std::runtime_error("Geometry Page Copy device removed");
        }
        return completed;
    }

    void PagePool::Collect()
    {
        uint64_t completed = CompletedCopyFence();
        while (!m_Transfers.empty() && m_Transfers.front().Fence <= completed)
        {
            m_Transfers.pop_front();
        }
    }

    ID3D12Resource* PagePool::Resource(uint32_t slot) const
    {
        if (slot >= m_Pages.size())
        {
            return nullptr;
        }
        return m_Pages[slot].Get();
    }

    void PagePool::Shutdown()
    {
        if (m_Fence && m_Submitted && m_Fence->GetCompletedValue() < m_Submitted)
        {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            bool waitingOnEvent = event && SUCCEEDED(m_Fence->SetEventOnCompletion(m_Submitted, event));
            while (m_Fence->GetCompletedValue() < m_Submitted && SUCCEEDED(m_Device->GetDeviceRemovedReason()))
            {
                if (waitingOnEvent)
                {
                    WaitForSingleObject(event, 500);
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (event)
            {
                CloseHandle(event);
            }
        }
        m_Transfers.clear();
        m_Pages.clear();
        m_Queue.Reset();
        m_Fence.Reset();
        m_Device.Reset();
        m_Submitted = 0;
    }

    void PagePool::Release(uint32_t slot)
    {
        if (slot < m_Pages.size())
        {
            m_Pages[slot].Reset();
        }
    }

    uint64_t PagePool::MemoryBytes() const
    {
        uint64_t bytes = 0;
        for (const auto& page : m_Pages)
        {
            if (page)
            {
                bytes += 65536;
            }
        }
        return bytes;
    }

    PagePool::~PagePool()
    {
        Shutdown();
    }
} // 名前空間 VirtualGeometryStreaming の終わり。
