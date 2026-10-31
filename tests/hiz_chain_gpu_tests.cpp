#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>
#include "../source/d3dx12.h"

using Microsoft::WRL::ComPtr;

namespace
{
    void Check(HRESULT result)
    {
        if (FAILED(result))
        {
            std::fprintf(stderr, "Hi-Z HRESULT: 0x%08lx\n", static_cast<unsigned long>(result));
            throw std::runtime_error("Hi-Z native chain failed");
        }
    }
}

int main()
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D12Device> device;
        for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
        {
            DXGI_ADAPTER_DESC1 description{};
            adapter->GetDesc1(&description);
            if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            {
                std::wprintf(L"Hi-Z native device: %ls\n", description.Description);
                break;
            }
            adapter.Reset();
        }
        if (!device) throw std::runtime_error("Hardware D3D12 device required");
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        Check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&commands)));
        Check(commands->Close());
        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        CD3DX12_ROOT_PARAMETER parameters[3];
        parameters[0].InitAsDescriptorTable(1, &ranges[0]);
        parameters[1].InitAsDescriptorTable(1, &ranges[1]);
        parameters[2].InitAsConstants(4, 0);
        CD3DX12_ROOT_SIGNATURE_DESC rootDescription(3, parameters);
        ComPtr<ID3DBlob> serialized;
        Check(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr));
        ComPtr<ID3D12RootSignature> root;
        Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                         IID_PPV_ARGS(&root)));
        ComPtr<ID3DBlob> code;
        Check(D3DReadFileToBlob(L"shader/hlsl/build/HiZReduceCS.cso", &code));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
        pipelineDescription.pRootSignature = root.Get();
        pipelineDescription.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pipeline;
        Check(device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&pipeline)));
        ComPtr<ID3D12Fence> fence;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completed) throw std::runtime_error("Fence event creation failed");
        UINT64 fenceValue = 0;
        for (const auto extent : {std::pair<UINT, UINT>{5, 3}, {192, 128}, {480, 320}, {960, 640}})
        {
            const UINT width = extent.first, height = extent.second;
            UINT mipCount = 1;
            for (UINT size = std::max(width, height); size > 1; size >>= 1) ++mipCount;
            auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            auto description = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, width, height, 1,
                static_cast<UINT16>(mipCount), 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            ComPtr<ID3D12Resource> texture;
            Check(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texture)));
            D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
            heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDescription.NumDescriptors = mipCount * 2;
            heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ComPtr<ID3D12DescriptorHeap> heap;
            Check(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap)));
            heapDescription.NumDescriptors = 1;
            heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ComPtr<ID3D12DescriptorHeap> clearHeap;
            Check(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&clearHeap)));
            const UINT increment = device->GetDescriptorHandleIncrementSize(heapDescription.Type);
            auto cpu = [&](UINT index) { return CD3DX12_CPU_DESCRIPTOR_HANDLE(
                heap->GetCPUDescriptorHandleForHeapStart(), index, increment); };
            auto gpu = [&](UINT index) { return CD3DX12_GPU_DESCRIPTOR_HANDLE(
                heap->GetGPUDescriptorHandleForHeapStart(), index, increment); };
            for (UINT mip = 0; mip < mipCount; ++mip)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R32_FLOAT;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D.MostDetailedMip = mip;
                srv.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(texture.Get(), &srv, cpu(mip));
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.Format = DXGI_FORMAT_R32_FLOAT;
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Texture2D.MipSlice = mip;
                device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, cpu(mipCount + mip));
                if (mip == 0)
                {
                    device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav,
                        clearHeap->GetCPUDescriptorHandleForHeapStart());
                }
            }
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&description, mipCount - 1, 1, 0, &footprint, nullptr, nullptr, &bytes);
            auto readbackDescription = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
            ComPtr<ID3D12Resource> readback;
            Check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
            Check(allocator->Reset());
            Check(commands->Reset(allocator.Get(), pipeline.Get()));
            ID3D12DescriptorHeap* heaps[] = {heap.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(root.Get());
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0);
            commands->ResourceBarrier(1, &barrier);
            const float depths[4] = {.25f, .25f, .25f, .25f};
            commands->ClearUnorderedAccessViewFloat(gpu(mipCount), clearHeap->GetCPUDescriptorHandleForHeapStart(),
                texture.Get(), depths, 0, nullptr);
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0);
            commands->ResourceBarrier(1, &barrier);
            for (UINT mip = 1; mip < mipCount; ++mip)
            {
                barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mip);
                commands->ResourceBarrier(1, &barrier);
                commands->SetComputeRootDescriptorTable(0, gpu(mip - 1));
                commands->SetComputeRootDescriptorTable(1, gpu(mipCount + mip));
                const UINT constants[] = {std::max(width >> mip, 1u), std::max(height >> mip, 1u), 0, 0};
                commands->SetComputeRoot32BitConstants(2, 4, constants, 0);
                commands->Dispatch((constants[0] + 7) / 8, (constants[1] + 7) / 8, 1);
                barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mip);
                commands->ResourceBarrier(1, &barrier);
            }
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE, mipCount - 1);
            commands->ResourceBarrier(1, &barrier);
            const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprint);
            const CD3DX12_TEXTURE_COPY_LOCATION source(texture.Get(), mipCount - 1);
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            Check(commands->Close());
            ID3D12CommandList* lists[] = {commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), ++fenceValue));
            Check(fence->SetEventOnCompletion(fenceValue, completed));
            if (WaitForSingleObject(completed, 30000) != WAIT_OBJECT_0 ||
                FAILED(device->GetDeviceRemovedReason())) throw std::runtime_error("Hi-Z chain GPU stopped");
            void* mapped = nullptr;
            Check(readback->Map(0, nullptr, &mapped));
            const float depth = *static_cast<const float*>(mapped);
            readback->Unmap(0, nullptr);
            if (depth != .25f) throw std::runtime_error("Hi-Z minimum differs from constant source");
            std::printf("Hi-Z %ux%u, %u mips: last mip %.2f PASS\n", width, height, mipCount, depth);
        }
        CloseHandle(completed);
        ComPtr<ID3D12InfoQueue> messages;
        Check(device.As(&messages));
        for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index)
        {
            SIZE_T size = 0;
            Check(messages->GetMessage(index, nullptr, &size));
            std::vector<char> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            Check(messages->GetMessage(index, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                std::fputs(message->pDescription, stderr);
                throw std::runtime_error("Hi-Z D3D12 validation error");
            }
        }
        std::puts("Native prebuilt Hi-Z reduction chain: PASS (debug layer, hardware)");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
