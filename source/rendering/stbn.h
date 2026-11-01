#pragma once
#include "pch.h"
#include "graphicslog.h"
#include <cstdint>
#include <fstream>

// One immutable GPU buffer shared by shadow/AO and radiance passes. Root SRVs
// avoid reserving another slot in either engine or per-frame descriptor heaps.
class Stbn final
{
  public:
    static bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        Shutdown();
        constexpr size_t payloadBytes = 128 * 128 * 64 * 2;
        std::vector<uint32_t> data(1 + payloadBytes / sizeof(uint32_t), 0);
        char option[16]{};
        const bool disabled = GetEnvironmentVariableA("DX12_STBN", option, sizeof(option)) && option[0] == '0';
        if (!disabled)
        {
            std::ifstream input("asset/Noise/stbn-vec2-128x128x64.rg8", std::ios::binary | std::ios::ate);
            if (input && input.tellg() == static_cast<std::streamoff>(payloadBytes))
            {
                input.seekg(0);
                if (input.read(reinterpret_cast<char*>(data.data() + 1), payloadBytes))
                {
                    data[0] = 1;
                }
            }
        }
        const bool available = data[0] != 0;
        if (!available)
        {
            data.resize(1); // A valid bound buffer also serves the R2 fallback.
        }
        const UINT64 bytes = data.size() * sizeof(uint32_t);
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> upload;
        if (!device || !queue ||
            FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_Buffer))) ||
            FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        {
            return false;
        }
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(upload->Map(0, &noRead, &mapped)))
        {
            return false;
        }
        memcpy(mapped, data.data(), static_cast<size_t>(bytes));
        upload->Unmap(0, nullptr);
        s_Buffer->SetName(L"STBN uniform Float2 RG8");

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            return false;
        }
        list->CopyBufferRegion(s_Buffer.Get(), 0, upload.Get(), 0, bytes);
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(s_Buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close()))
        {
            return false;
        }
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!event)
        {
            return false;
        }
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const HRESULT signal = queue->Signal(fence.Get(), 1);
        const HRESULT completion = fence->SetEventOnCompletion(1, event);
        bool complete = false;
        if (SUCCEEDED(signal) && SUCCEEDED(completion))
        {
            complete = WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0;
        }
        CloseHandle(event);
        WriteGraphicsLog("STBN: %s (%llu GPU buffer bytes)\n",
            available ? "uniform Float2 128x128x64" : "R2 fallback (disabled or asset missing/invalid)", bytes);
        return complete;
    }

    static void Bind(ID3D12GraphicsCommandList* commands, UINT root)
    {
        commands->SetComputeRootShaderResourceView(root, s_Buffer->GetGPUVirtualAddress());
    }

    static void Shutdown()
    {
        s_Buffer.Reset();
    }

  private:
    inline static ComPtr<ID3D12Resource> s_Buffer;
};
