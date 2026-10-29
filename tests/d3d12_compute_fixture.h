#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;
#include "dxccompiler.h"

namespace D3D12Test
{
    using Microsoft::WRL::ComPtr;

    inline void Check(HRESULT result)
    {
        if (FAILED(result))
        {
            char message[96];
            std::snprintf(message, sizeof(message), "D3D12 GPU fixture failed (HRESULT 0x%08lX)", static_cast<unsigned long>(result));
            throw std::runtime_error(message);
        }
    }

    inline ComPtr<ID3DBlob> Compile(const wchar_t* path, const D3D_SHADER_MACRO* defines = nullptr)
    {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT result = DxcCompileFromFileCompat(path, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "main", "cs_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(result) && errors)
        {
            std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        }
        Check(result);
        return code;
    }

    struct Texture
    {
        ComPtr<ID3D12Resource> Resource;
        UINT Width = 0;
        UINT Height = 0;
        DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
        UINT Stride = 0; // Nonzero for a structured buffer.
    };

    struct ConstantData
    {
        const void* Data;
        size_t Size;
    };

    class Fixture
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12DescriptorHeap> descriptors;
        struct Pipeline
        {
            ComPtr<ID3DBlob> Code;
            ComPtr<ID3D12PipelineState> State;
        };
        std::map<ID3DBlob*, Pipeline> pipelines;
        UINT64 fenceValue = 0;
        UINT descriptorSize = 0;

        ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE heap,
                                     D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
        {
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = heap;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = flags;
            ComPtr<ID3D12Resource> resource;
            Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state,
                nullptr, IID_PPV_ARGS(&resource)));
            return resource;
        }

        void Begin()
        {
            Check(allocator->Reset());
            Check(commands->Reset(allocator.Get(), nullptr));
        }

        void Execute()
        {
            Check(commands->Close());
            ID3D12CommandList* lists[] = {commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), ++fenceValue));
            if (fence->GetCompletedValue() < fenceValue)
            {
                const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!event)
                {
                    Check(HRESULT_FROM_WIN32(GetLastError()));
                }
                const HRESULT result = fence->SetEventOnCompletion(fenceValue, event);
                if (FAILED(result))
                {
                    CloseHandle(event);
                    Check(result);
                }
                const DWORD waited = WaitForSingleObject(event, 60000);
                CloseHandle(event);
                if (waited != WAIT_OBJECT_0)
                {
                    throw std::runtime_error("GPU fixture fence timed out");
                }
            }
            Check(device->GetDeviceRemovedReason());
        }

        void Transition(Texture& texture, D3D12_RESOURCE_STATES state)
        {
            if (texture.State == state)
            {
                return;
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texture.Resource.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = texture.State;
            barrier.Transition.StateAfter = state;
            commands->ResourceBarrier(1, &barrier);
            texture.State = state;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index)
        {
            auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * descriptorSize;
            return handle;
        }

    public:
        explicit Fixture(bool warp = true)
        {
            ComPtr<IDXGIFactory4> factory;
            Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            ComPtr<IDXGIAdapter> adapter;
            if (warp)
            {
                Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
            }
            Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
            Check(commands->Close());
            Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = 24;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors)));
            descriptorSize = device->GetDescriptorHandleIncrementSize(heap.Type);
            D3D12_DESCRIPTOR_RANGE ranges[2]{};
            ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0, 0, 0};
            ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0, 0, 0};
            D3D12_ROOT_PARAMETER parameters[4]{};
            for (UINT index = 0; index < 2; ++index)
            {
                parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                parameters[index].Descriptor.ShaderRegister = index;
                parameters[index + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[index + 2].DescriptorTable = {1, &ranges[index]};
            }
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxAnisotropy = 1;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            D3D12_ROOT_SIGNATURE_DESC signature{};
            signature.NumParameters = 4;
            signature.pParameters = parameters;
            signature.NumStaticSamplers = 1;
            signature.pStaticSamplers = &sampler;
            ComPtr<ID3DBlob> serialized, errors;
            Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
            Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
        }

        Texture MakeTexture(UINT width, UINT height, DXGI_FORMAT format, bool writable)
        {
            Texture texture;
            texture.Width = width;
            texture.Height = height;
            texture.Format = format;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Format = format;
            if (writable)
            {
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, texture.State,
                nullptr, IID_PPV_ARGS(&texture.Resource)));
            return texture;
        }

        Texture MakeStructuredBuffer(UINT elements, UINT stride)
        {
            Texture texture;
            texture.Width = elements;
            texture.Height = 1;
            texture.Stride = stride;
            texture.Resource = Buffer(UINT64(elements) * stride, D3D12_HEAP_TYPE_DEFAULT,
                texture.State, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            return texture;
        }

        void Upload(Texture& texture, const void* data, UINT rowBytes)
        {
            if (texture.Stride)
            {
                const UINT64 bytes = UINT64(texture.Width) * texture.Stride;
                if (rowBytes != bytes) throw std::runtime_error("Structured upload size mismatch");
                auto upload = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                void* mapped = nullptr;
                D3D12_RANGE noRead{0, 0};
                Check(upload->Map(0, &noRead, &mapped));
                std::memcpy(mapped, data, static_cast<size_t>(bytes));
                upload->Unmap(0, nullptr);
                Begin();
                Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
                commands->CopyBufferRegion(texture.Resource.Get(), 0, upload.Get(), 0, bytes);
                Execute();
                return;
            }
            const auto desc = texture.Resource->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 totalBytes = 0;
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
            auto upload = Buffer(totalBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped = nullptr;
            D3D12_RANGE noRead{0, 0};
            Check(upload->Map(0, &noRead, &mapped));
            for (UINT row = 0; row < texture.Height; ++row)
            {
                std::memcpy(static_cast<BYTE*>(mapped) + footprint.Offset + size_t(row) * footprint.Footprint.RowPitch,
                    static_cast<const BYTE*>(data) + size_t(row) * rowBytes, rowBytes);
            }
            upload->Unmap(0, nullptr);
            Begin();
            Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.Resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprint;
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            Execute();
        }

        void DispatchConstants(ID3DBlob* code, const std::vector<ConstantData>& constants,
            const std::vector<Texture*>& srvs, const std::vector<Texture*>& uavs, UINT x, UINT y, UINT z = 1)
        {
            if (constants.size() > 2 || srvs.size() > 16 || uavs.size() > 8)
            {
                throw std::runtime_error("GPU fixture binding count exceeded");
            }
            auto& pipeline = pipelines[code];
            if (!pipeline.State)
            {
                pipeline.Code = code;
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
                desc.pRootSignature = root.Get();
                desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
                Check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline.State)));
            }
            std::vector<ComPtr<ID3D12Resource>> buffers;
            Begin();
            commands->SetComputeRootSignature(root.Get());
            commands->SetPipelineState(pipeline.State.Get());
            for (UINT index = 0; index < static_cast<UINT>(constants.size()); ++index)
            {
                const auto& constant = constants[index];
                auto buffer = Buffer((constant.Size + 255u) & ~size_t(255u), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                void* mapped = nullptr;
                D3D12_RANGE noRead{0, 0};
                Check(buffer->Map(0, &noRead, &mapped));
                std::memcpy(mapped, constant.Data, constant.Size);
                buffer->Unmap(0, nullptr);
                commands->SetComputeRootConstantBufferView(index, buffer->GetGPUVirtualAddress());
                buffers.push_back(buffer);
            }
            for (UINT index = 0; index < static_cast<UINT>(srvs.size()); ++index)
            {
                auto& texture = *srvs[index];
                Transition(texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.Format = texture.Format;
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.Texture2D.MipLevels = 1;
                if (texture.Stride)
                {
                    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    desc.Buffer = {};
                    desc.Buffer.NumElements = texture.Width;
                    desc.Buffer.StructureByteStride = texture.Stride;
                }
                device->CreateShaderResourceView(texture.Resource.Get(), &desc, Cpu(index));
            }
            for (UINT index = 0; index < static_cast<UINT>(uavs.size()); ++index)
            {
                auto& texture = *uavs[index];
                Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
                desc.Format = texture.Format;
                desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                if (texture.Stride)
                {
                    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    desc.Buffer.NumElements = texture.Width;
                    desc.Buffer.StructureByteStride = texture.Stride;
                }
                device->CreateUnorderedAccessView(texture.Resource.Get(), nullptr, &desc, Cpu(16 + index));
            }
            ID3D12DescriptorHeap* heaps[] = {descriptors.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
            commands->SetComputeRootDescriptorTable(2, gpu);
            gpu.ptr += UINT64(16) * descriptorSize;
            commands->SetComputeRootDescriptorTable(3, gpu);
            commands->Dispatch(x, y, z);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            commands->ResourceBarrier(1, &barrier);
            Execute();
        }

        void Dispatch(ID3DBlob* code, const void* constants, size_t constantsSize,
            const std::vector<Texture*>& srvs, const std::vector<Texture*>& uavs, UINT x, UINT y, UINT z = 1)
        {
            DispatchConstants(code, {{constants, constantsSize}}, srvs, uavs, x, y, z);
        }

        std::vector<BYTE> Readback(Texture& texture)
        {
            const auto desc = texture.Resource->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 totalBytes = 0;
            UINT64 rowBytes = 0;
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, &rowBytes, &totalBytes);
            if (texture.Stride)
            {
                rowBytes = UINT64(texture.Width) * texture.Stride;
                totalBytes = rowBytes;
            }
            auto readback = Buffer(totalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            Begin();
            Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (texture.Stride)
            {
                commands->CopyBufferRegion(readback.Get(), 0, texture.Resource.Get(), 0, totalBytes);
            }
            else
            {
                D3D12_TEXTURE_COPY_LOCATION source{};
                D3D12_TEXTURE_COPY_LOCATION destination{};
                source.pResource = texture.Resource.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.pResource = readback.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint = footprint;
                commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
            Execute();
            void* mapped = nullptr;
            D3D12_RANGE range{0, static_cast<SIZE_T>(totalBytes)};
            Check(readback->Map(0, &range, &mapped));
            std::vector<BYTE> packed(static_cast<size_t>(rowBytes) * texture.Height);
            for (UINT row = 0; row < texture.Height; ++row)
            {
                std::memcpy(packed.data() + size_t(row) * static_cast<size_t>(rowBytes),
                    static_cast<const BYTE*>(mapped) + footprint.Offset + size_t(row) * footprint.Footprint.RowPitch,
                    static_cast<size_t>(rowBytes));
            }
            D3D12_RANGE noWrite{0, 0};
            readback->Unmap(0, &noWrite);
            return packed;
        }
    };
}
