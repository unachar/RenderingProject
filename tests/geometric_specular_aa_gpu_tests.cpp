#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;
#include "../source/dxccompiler.h"
#include "../source/d3dx12.h"

namespace
{
    void Check(HRESULT result, const char* message)
    {
        if (FAILED(result))
        {
            fprintf(stderr, "%s: HRESULT 0x%08lx\n", message, static_cast<unsigned long>(result));
            throw runtime_error(message);
        }
    }

    void Near(float actual, double expected, const char* message)
    {
        if (!std::isfinite(actual) || std::abs(double(actual) - expected) > 2e-6)
        {
            fprintf(stderr, "%s: expected %.9g, got %.9g\n", message, expected, actual);
            throw runtime_error(message);
        }
    }

    ComPtr<ID3DBlob> CompileFixture(const char* entry, const char* profile, unsigned enabled)
    {
        const char* mode = enabled ? "1" : "0";
        const D3D_SHADER_MACRO defines[] = {{"GEOMETRIC_SPECULAR_AA", mode}, {}};
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        HRESULT result = DxcCompileFromFileCompat(L"tests/shaders/geometric_specular_aa_fixture.hlsl",
            defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &code, &errors);
        if (FAILED(result) && errors)
        {
            fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        }
        Check(result, "production helper DXC compile");
        return code;
    }
}

#include "visibility_reconstruction_gpu_fixture.h"

int main()
{
    HANDLE completed = nullptr;
    try
    {
        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "hardware D3D12 device");
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        Check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)), "adapter identity");
        DXGI_ADAPTER_DESC1 description{};
        Check(adapter->GetDesc1(&description), "adapter description");
        wprintf(L"Geometric AA device: %ls\n", description.Description);
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        ComPtr<ID3D12CommandQueue> queue;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "command queue");
        ComPtr<ID3D12CommandAllocator> allocator;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        ComPtr<ID3D12GraphicsCommandList> commands;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&commands)), "command list");
        Check(commands->Close(), "initial command list close");
        ComPtr<ID3D12Fence> fence;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completed)
        {
            throw runtime_error("completion event");
        }
        constexpr UINT Count = 128;
        constexpr UINT Bytes = Count * 4 * sizeof(float);
        const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto outputDesc = CD3DX12_RESOURCE_DESC::Buffer(Bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(Bytes);
        ComPtr<ID3D12Resource> output;
        ComPtr<ID3D12Resource> readback;
        Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &outputDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output)), "output buffer");
        Check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback buffer");
        for (unsigned enabled = 0; enabled <= 1; ++enabled)
        {
            auto code = CompileFixture("Compute", "cs_6_0", enabled);
            ComPtr<ID3D12RootSignature> root;
            Check(device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(),
                                             IID_PPV_ARGS(&root)), "root signature");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = root.Get();
            pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            ComPtr<ID3D12PipelineState> pso;
            Check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)), "compute PSO");
            Check(allocator->Reset(), "allocator reset");
            Check(commands->Reset(allocator.Get(), pso.Get()), "command list reset");
            commands->SetComputeRootSignature(root.Get());
            commands->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
            commands->Dispatch(2, 1, 1);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->ResourceBarrier(1, &barrier);
            commands->CopyResource(readback.Get(), output.Get());
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->ResourceBarrier(1, &barrier);
            Check(commands->Close(), "command list close");
            ID3D12CommandList* lists[] = {commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            const UINT64 serial = enabled + 1;
            Check(queue->Signal(fence.Get(), serial), "queue signal");
            Check(fence->SetEventOnCompletion(serial, completed), "fence event");
            if (WaitForSingleObject(completed, 30000) != WAIT_OBJECT_0)
            {
                throw runtime_error("GPU completion timeout");
            }
            const D3D12_RANGE range{0, Bytes};
            float* values = nullptr;
            Check(readback->Map(0, &range, reinterpret_cast<void**>(&values)), "readback map");
            for (unsigned index = 0; index < Count; ++index)
            {
                const unsigned scenario = index / 16;
                const unsigned material = index % 16;
                const float roughness = float(material) / 15.0f;
                double gradient = 0;
                if (scenario == 1 || scenario == 7)
                {
                    gradient = 2.0 - std::sqrt(2.0);
                }
                if (scenario == 6)
                {
                    gradient = 4;
                }
                double filtered = roughness;
                if (gradient > 0)
                {
                    filtered = std::pow(std::fmin(1.0, std::pow(double(roughness), 4.0) +
                                           std::fmin(0.3 * gradient, 0.2)), 0.25);
                }
                double expectedMaterial = roughness;
                if (enabled && material == 11)
                {
                    expectedMaterial = filtered;
                }
                Near(values[index * 4], expectedMaterial, "material filtering / OFF / Toon");
                Near(values[index * 4 + 1], filtered, "GPU roughness reference");
                Near(values[index * 4 + 2], 0.25, "non-finite derivative rejection");
                Near(values[index * 4 + 3], gradient, "same-triangle normal difference/failure fallback");
            }
            const D3D12_RANGE noWrites{0, 0};
            readback->Unmap(0, &noWrites);
            printf("Geometric AA=%u: %u native D3D12 material/gradient cases PASS\n", enabled, Count);
        }

        // Exercise real pixel quad derivatives with flat PBR, Toon, Unlit, curved PBR bands.
        constexpr UINT Width = 32;
        constexpr UINT Height = 16;
        const auto targetDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, Width, Height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ComPtr<ID3D12Resource> target;
        Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)), "raster target");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 textureBytes = 0;
        device->GetCopyableFootprints(&targetDesc, 0, 1, 0, &footprint, nullptr, nullptr, &textureBytes);
        const auto textureReadbackDesc = CD3DX12_RESOURCE_DESC::Buffer(textureBytes);
        ComPtr<ID3D12Resource> textureReadback;
        Check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &textureReadbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&textureReadback)), "raster readback");
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        Check(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "RTV heap");
        const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target.Get(), nullptr, rtv);
        for (unsigned enabled = 0; enabled <= 1; ++enabled)
        {
            auto vertex = CompileFixture("Vertex", "vs_6_0", enabled);
            auto pixel = CompileFixture("Pixel", "ps_6_0", enabled);
            ComPtr<ID3D12RootSignature> root;
            Check(device->CreateRootSignature(0, vertex->GetBufferPointer(), vertex->GetBufferSize(),
                IID_PPV_ARGS(&root)), "raster root signature");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
            graphics.pRootSignature = root.Get();
            graphics.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
            graphics.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
            graphics.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            graphics.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            graphics.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            graphics.DepthStencilState.DepthEnable = FALSE;
            graphics.DepthStencilState.StencilEnable = FALSE;
            graphics.SampleMask = UINT_MAX;
            graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            graphics.NumRenderTargets = 1;
            graphics.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
            graphics.SampleDesc.Count = 1;
            ComPtr<ID3D12PipelineState> pso;
            Check(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&pso)), "raster PSO");
            Check(allocator->Reset(), "raster allocator reset");
            Check(commands->Reset(allocator.Get(), pso.Get()), "raster list reset");
            commands->SetGraphicsRootSignature(root.Get());
            commands->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
            const D3D12_VIEWPORT viewport{0, 0, float(Width), float(Height), 0, 1};
            const D3D12_RECT scissor{0, 0, Width, Height};
            commands->RSSetViewports(1, &viewport);
            commands->RSSetScissorRects(1, &scissor);
            commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commands->DrawInstanced(3, 1, 0, 0);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                               D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->ResourceBarrier(1, &barrier);
            const CD3DX12_TEXTURE_COPY_LOCATION source(target.Get(), 0);
            const CD3DX12_TEXTURE_COPY_LOCATION destination(textureReadback.Get(), footprint);
            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
            commands->ResourceBarrier(1, &barrier);
            Check(commands->Close(), "raster list close");
            ID3D12CommandList* lists[] = {commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), enabled + 3), "raster signal");
            Check(fence->SetEventOnCompletion(enabled + 3, completed), "raster completion event");
            if (WaitForSingleObject(completed, 30000) != WAIT_OBJECT_0)
            {
                throw runtime_error("raster completion timeout");
            }
            const D3D12_RANGE readRange{0, size_t(textureBytes)};
            unsigned char* data = nullptr;
            Check(textureReadback->Map(0, &readRange, reinterpret_cast<void**>(&data)), "raster map");
            ofstream capture("bin/validation/technology/geometric-aa" + to_string(enabled) + ".rgba32f", ios::binary);
            unsigned changed = 0;
            for (UINT y = 0; y < Height; ++y)
            {
                const auto* row = reinterpret_cast<const float*>(data + footprint.Offset +
                    size_t(y) * footprint.Footprint.RowPitch);
                capture.write(reinterpret_cast<const char*>(row), Width * 4 * sizeof(float));
                for (UINT x = 0; x < Width; ++x)
                {
                    const float* pixelValue = row + x * 4;
                    const bool filtered = enabled && x >= 24;
                    double expected = 0.04f;
                    if (filtered)
                    {
                        expected = std::pow(std::pow(double(0.04f), 4.0) +
                            std::fmin(0.3 * double(pixelValue[2]), 0.2), 0.25);
                    }
                    Near(pixelValue[0], expected, "raster derivative/material result");
                    if (x < 8)
                    {
                        Near(pixelValue[2], 0, "flat geometry derivative");
                    }
                    if (filtered && pixelValue[0] > 0.04f)
                    {
                        ++changed;
                    }
                }
            }
            const D3D12_RANGE noWrites{0, 0};
            textureReadback->Unmap(0, &noWrites);
            if (!capture || (enabled && changed != 8 * Height))
            {
                throw runtime_error("raster capture or PBR filtering coverage");
            }
            printf("Geometric AA=%u raster: %u pixels checked, %u curved PBR pixels filtered\n",
                   enabled, Width * Height, changed);
        }
        TestVisibilityReconstruction(device.Get(), queue.Get(), allocator.Get(), commands.Get(),
            output.Get(), readback.Get(), fence.Get(), completed);
        CloseHandle(completed);
        return 0;
    }
    catch (const exception& error)
    {
        if (completed)
        {
            CloseHandle(completed);
        }
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
