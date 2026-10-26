#include "pch.h"
#include "proceduraltexturegpu.h"
#include <cstring>
#include <chrono>
#include <thread>

namespace ProcStudio
{
    namespace
    {
        struct Constants
        {
            uint32_t Width, Height, Kind, Output;
            uint32_t BasisType, Fractal, Seed, Tileable;
            float Frequency;
            int Octaves;
            float Lacunarity, Gain;
            float WarpStrength, RidgeOffset, Contrast, Bias;
            Float4 ColorA, ColorB, ColorC;
            float Turbulence, RingCount, Mortar, Time;
            int BricksX, BricksY, Pattern, RepeatX;
            int RepeatY;
            float PatternSize, PatternSoftness, NormalStrength;
            float Choppiness, Coverage, CloudSoftness, Padding = 0;
            uint32_t PreviewSize = 0, Tiles = 1, Padding1 = 0, Padding2 = 0;
        };

        static_assert(sizeof(Constants) == 192);

        void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
            commands->ResourceBarrier(1, &barrier);
        }

        bool CreateBuffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
                          ComPtr<ID3D12Resource>& output, std::string& error)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(heapType);
            auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if (heapType == D3D12_HEAP_TYPE_DEFAULT)
            {
                description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                                       IID_PPV_ARGS(&output))))
            {
                error = "GPU texture buffer allocation failed";
                return false;
            }
            return true;
        }
    }

    struct TextureGpu::Impl
    {
        ComPtr<ID3D12Device> Device;
        ComPtr<ID3D12RootSignature> Root;
        ComPtr<ID3D12PipelineState> GeneratePipeline, PreviewPipeline, NormalPipeline;
        ComPtr<ID3D12Resource> Pixels, Heights, PackedPreview, ReadbackBuffer;
        Constants Params{};
        int PreviewExtent = 0;

        void Bind(ID3D12GraphicsCommandList* commands, ID3D12PipelineState* pipeline)
        {
            commands->SetComputeRootSignature(Root.Get());
            commands->SetPipelineState(pipeline);
            commands->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &Params, 0);
        }
    };

    TextureGpu::TextureGpu() : m_Impl(std::make_unique<Impl>()) {}

    TextureGpu::~TextureGpu() = default;

    void TextureGpu::Release()
    {
        m_Impl = std::make_unique<Impl>();
    }

    bool TextureGpu::Supports(const TextureParams& params)
    {
        const int kind = static_cast<int>(params.Kind);
        return kind >= 0 && kind < static_cast<int>(TextureKind::Count) && params.Kind != TextureKind::Mandelbrot &&
               params.Kind != TextureKind::Julia;
    }

    bool TextureGpu::Initialize(ID3D12Device* device, std::string& error)
    {
        auto& impl = *m_Impl;
        if (impl.GeneratePipeline && impl.PreviewPipeline && impl.NormalPipeline && impl.Device.Get() == device)
        {
            return true;
        }
        if (!device)
        {
            error = "GPU device is unavailable";
            return false;
        }
        impl.Device = device;
        CD3DX12_ROOT_PARAMETER parameters[4];
        parameters[0].InitAsConstants(sizeof(Constants) / 4, 0);
        parameters[1].InitAsUnorderedAccessView(0);
        parameters[2].InitAsShaderResourceView(0);
        parameters[3].InitAsUnorderedAccessView(1);
        const CD3DX12_ROOT_SIGNATURE_DESC description(4, parameters);
        ComPtr<ID3DBlob> signature, diagnostics;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics)) ||
            FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                               IID_PPV_ARGS(&impl.Root))))
        {
            error = "GPU texture root signature creation failed";
            return false;
        }
        const auto compile = [&](const char* entry, ComPtr<ID3D12PipelineState>& pipeline) {
            ComPtr<ID3DBlob> shader;
            diagnostics.Reset();
            // The project's DXC compatibility layer promotes this profile to SM6 at runtime.
            if (FAILED(D3DCompileFromFile(
                    L"shader/hlsl/ProceduralTextureCS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry,
                    "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &shader, &diagnostics)))
            {
                error = diagnostics ? static_cast<const char*>(diagnostics->GetBufferPointer())
                                    : "GPU texture shader compilation failed";
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
            pipelineDescription.pRootSignature = impl.Root.Get();
            pipelineDescription.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
            if (FAILED(device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&pipeline))))
            {
                error = "GPU texture pipeline creation failed";
                return false;
            }
            return true;
        };
        return compile("Generate", impl.GeneratePipeline) && compile("Preview", impl.PreviewPipeline) &&
               compile("Normals", impl.NormalPipeline);
    }

    bool TextureGpu::Generate(ID3D12GraphicsCommandList* commands, const TextureParams& params, int width, int height,
                              std::string& error)
    {
        auto& impl = *m_Impl;
        if (!commands || !impl.GeneratePipeline || !Supports(params))
        {
            error = "Texture requires CPU generation or GPU initialization";
            return false;
        }
        width = std::clamp(width, 1, 8192);
        height = std::clamp(height, 1, 8192);
        if (!impl.Pixels || impl.Params.Width != static_cast<uint32_t>(width) ||
            impl.Params.Height != static_cast<uint32_t>(height))
        {
            ComPtr<ID3D12Resource> pixels;
            if (!CreateBuffer(impl.Device.Get(), UINT64(width) * height * sizeof(Float4), D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, pixels, error))
            {
                return false;
            }
            impl.Pixels = std::move(pixels);
            impl.ReadbackBuffer.Reset();
            impl.Heights.Reset();
        }
        const bool normalOutput = params.Output == TextureOutput::Normal;
        if (normalOutput && !impl.Heights &&
            !CreateBuffer(impl.Device.Get(), UINT64(width) * height * sizeof(Float4), D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, impl.Heights, error))
        {
            return false;
        }
        const auto& n = params.Noise;
        impl.Params = {static_cast<uint32_t>(width),
                       static_cast<uint32_t>(height),
                       static_cast<uint32_t>(params.Kind),
                       static_cast<uint32_t>(params.Output),
                       static_cast<uint32_t>(n.BasisType),
                       static_cast<uint32_t>(n.Fractal),
                       n.Seed,
                       params.Tileable ? 1u : 0u,
                       n.Frequency,
                       n.Octaves,
                       n.Lacunarity,
                       n.Gain,
                       n.WarpStrength,
                       n.RidgeOffset,
                       params.Contrast,
                       params.Bias,
                       {params.ColorA[0], params.ColorA[1], params.ColorA[2], 1},
                       {params.ColorB[0], params.ColorB[1], params.ColorB[2], 1},
                       {params.ColorC[0], params.ColorC[1], params.ColorC[2], 1},
                       params.Turbulence,
                       params.RingCount,
                       params.Mortar,
                       params.Time,
                       params.BricksX,
                       params.BricksY,
                       static_cast<int>(params.Pattern),
                       params.RepeatX,
                       params.RepeatY,
                       params.PatternSize,
                       params.PatternSoftness,
                       params.NormalStrength,
                       params.Choppiness,
                       params.Coverage,
                       params.Softness};
        auto* generated = normalOutput ? impl.Heights.Get() : impl.Pixels.Get();
        Transition(commands, generated, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl.Bind(commands, impl.GeneratePipeline.Get());
        commands->SetComputeRootUnorderedAccessView(1, generated->GetGPUVirtualAddress());
        commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        Transition(commands, generated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (normalOutput)
        {
            Transition(commands, impl.Pixels.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            impl.Bind(commands, impl.NormalPipeline.Get());
            commands->SetComputeRootShaderResourceView(2, impl.Heights->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(1, impl.Pixels->GetGPUVirtualAddress());
            commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            Transition(commands, impl.Pixels.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        return true;
    }

    ID3D12Resource* TextureGpu::Preview(ID3D12GraphicsCommandList* commands, int size, int tiles, std::string& error)
    {
        auto& impl = *m_Impl;
        if (!commands || !impl.Pixels || size < 1 || size > 8192)
        {
            error = "GPU preview has no generated image or invalid dimensions";
            return nullptr;
        }
        if (!impl.PackedPreview || impl.PreviewExtent != size)
        {
            const UINT64 rowPitch = (UINT64(size) * 4 + 255) & ~UINT64(255);
            if (!CreateBuffer(impl.Device.Get(), rowPitch * size, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_COPY_SOURCE, impl.PackedPreview, error))
            {
                return nullptr;
            }
            impl.PreviewExtent = size;
        }
        impl.Params.PreviewSize = size;
        impl.Params.Tiles = std::clamp(tiles, 1, 4);
        Transition(commands, impl.PackedPreview.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl.Bind(commands, impl.PreviewPipeline.Get());
        commands->SetComputeRootShaderResourceView(2, impl.Pixels->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(3, impl.PackedPreview->GetGPUVirtualAddress());
        commands->Dispatch((size + 7) / 8, (size + 7) / 8, 1);
        Transition(commands, impl.PackedPreview.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        return impl.PackedPreview.Get();
    }

    bool TextureGpu::CopyToReadback(ID3D12GraphicsCommandList* commands, std::string& error)
    {
        auto& impl = *m_Impl;
        if (!commands || !impl.Pixels)
        {
            error = "GPU readback has no generated image";
            return false;
        }
        const UINT64 bytes = UINT64(impl.Params.Width) * impl.Params.Height * sizeof(Float4);
        if (!impl.ReadbackBuffer && !CreateBuffer(impl.Device.Get(), bytes, D3D12_HEAP_TYPE_READBACK,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, impl.ReadbackBuffer, error))
        {
            return false;
        }
        Transition(commands, impl.Pixels.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(impl.ReadbackBuffer.Get(), 0, impl.Pixels.Get(), 0, bytes);
        Transition(commands, impl.Pixels.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }

    bool TextureGpu::Readback(Image& image, std::string& error) const
    {
        const auto& impl = *m_Impl;
        if (!impl.ReadbackBuffer)
        {
            error = "GPU readback was not requested";
            return false;
        }
        const size_t count = size_t(impl.Params.Width) * impl.Params.Height;
        const D3D12_RANGE range{0, count * sizeof(Float4)};
        void* mapped = nullptr;
        if (FAILED(impl.ReadbackBuffer->Map(0, &range, &mapped)))
        {
            error = "GPU texture readback mapping failed";
            return false;
        }
        image.Width = impl.Params.Width;
        image.Height = impl.Params.Height;
        image.Pixels.resize(count);
        std::memcpy(image.Pixels.data(), mapped, range.End);
        const D3D12_RANGE written{0, 0};
        impl.ReadbackBuffer->Unmap(0, &written);
        return true;
    }

    bool GenerateTextureGpu(ID3D12Device* device, const TextureParams& params, int width, int height, Image& image,
                            std::string& error)
    {
        TextureGpu generator;
        if (!TextureGpu::Supports(params) || !generator.Initialize(device, error))
        {
            return false;
        }
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&commands))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            error = "GPU texture export queue creation failed";
            return false;
        }
        if (!generator.Generate(commands.Get(), params, width, height, error) ||
            !generator.CopyToReadback(commands.Get(), error) || FAILED(commands->Close()))
        {
            return false;
        }
        // Create the completion event before submission so an allocation failure cannot release live resources.
        HANDLE completion = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!completion)
        {
            error = "GPU texture completion event creation failed";
            return false;
        }
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        HRESULT signal = queue->Signal(fence.Get(), 1);
        // A transient signaling failure must not release resources already submitted.
        while (FAILED(signal) && SUCCEEDED(device->GetDeviceRemovedReason()))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            signal = queue->Signal(fence.Get(), 1);
        }
        if (SUCCEEDED(signal))
        {
            const HRESULT notify = fence->SetEventOnCompletion(1, completion);
            const bool notified = SUCCEEDED(notify) && WaitForSingleObject(completion, INFINITE) == WAIT_OBJECT_0;
            if (!notified)
            {
                // Notification allocation can fail even when GPU submission succeeds.
                // Keep every resource alive until the fence completes or the device is removed.
                while (fence->GetCompletedValue() < 1 && SUCCEEDED(device->GetDeviceRemovedReason()))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        CloseHandle(completion);
        if (FAILED(signal) || FAILED(device->GetDeviceRemovedReason()) || fence->GetCompletedValue() == UINT64_MAX)
        {
            error = "GPU device was removed during texture export";
            return false;
        }
        return generator.Readback(image, error);
    }
}
