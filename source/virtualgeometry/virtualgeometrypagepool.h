#pragma once
#include "virtualgeometryresidency.h"
#include <d3d12.h>
#include <wrl.h>
#include <deque>
#include <span>

namespace VirtualGeometryStreaming
{
class PagePool
{
public:
    void Initialize(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE descriptors, uint32_t increment);
    uint64_t Submit(LoadTicket ticket, std::span<const uint8_t> payload);
    uint64_t CompletedCopyFence() const;
    void Collect();
    ID3D12Resource* Resource(uint32_t slot) const;
    void Release(uint32_t slot);
    uint64_t MemoryBytes() const;
    uint64_t UploadBytes() const { return m_Transfers.size() * 65536ull; }
    void Shutdown();
    ~PagePool();
private:
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Transfer
    {
        Ptr<ID3D12CommandAllocator> Allocator;
        Ptr<ID3D12GraphicsCommandList> Commands;
        Ptr<ID3D12Resource> Upload;
        uint64_t Fence = 0;
    };
    Ptr<ID3D12Device> m_Device;
    Ptr<ID3D12CommandQueue> m_Queue;
    Ptr<ID3D12Fence> m_Fence;
    std::vector<Ptr<ID3D12Resource>> m_Pages;
    std::deque<Transfer> m_Transfers;
    D3D12_CPU_DESCRIPTOR_HANDLE m_Descriptors{};
    uint32_t m_Increment = 0;
    uint64_t m_Submitted = 0;
};
}
