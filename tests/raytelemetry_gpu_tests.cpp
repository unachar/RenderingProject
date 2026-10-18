#include <windows.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include "rendering/raytelemetry.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
void Check(bool value, const char* message)
{
    if (!value)
    {
        throw std::runtime_error(message);
    }
}

void Hr(HRESULT result, const char* message)
{
    if (FAILED(result))
    {
        std::cerr << "HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}
} // namespace

int main()
{
    HANDLE event = nullptr;
    try
    {
        ComPtr<ID3D12Debug> debug;
        const bool debugEnabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if (debugEnabled)
        {
            debug->EnableDebugLayer();
        }
        ComPtr<ID3D12Device> device;
        Hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "create device");
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        Hr(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "create queue");
        ComPtr<ID3D12CommandAllocator> allocator;
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
           "create allocator");
        ComPtr<ID3D12GraphicsCommandList> commands;
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&commands)),
           "create list");
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor.RegisterSpace = 9;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 1;
        signature.pParameters = &parameter;
        ComPtr<ID3DBlob> blob, errors;
        Hr(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "serialize root");
        ComPtr<ID3D12RootSignature> root;
        Hr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
           "create root");
        ComPtr<ID3DBlob> shader;
        Hr(D3DReadFileToBlob(L"bin/validation/raytelemetry_test.cso", &shader), "read fixture shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pipeline;
        Hr(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "create fixture pipeline");
        ComPtr<ID3D12Fence> fence;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "create fence event");
        RayTelemetry::Collector collector;
        for (uint64_t serial = 1; serial <= 4; ++serial)
        {
            const UINT slot = serial == 2 ? 1u : 0u;
            const bool enabled = serial != 3;
            Check(collector.BeginFrame(device.Get(), commands.Get(), serial, slot, enabled) == enabled,
                  "collector matches enabled state");
            if (enabled)
            {
                collector.RecordProvider(RayTelemetry::Radiance, true);
                collector.RecordProvider(RayTelemetry::ShadowAo, true);
                commands->SetComputeRootSignature(root.Get());
                commands->SetPipelineState(pipeline.Get());
                commands->SetComputeRootUnorderedAccessView(0, collector.Address());
                commands->Dispatch(1, 1, 1);
            }
            collector.EndFrame(commands.Get());
            Hr(commands->Close(), "close fixture list");
            ID3D12CommandList* lists[] = {commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            Hr(queue->Signal(fence.Get(), serial), "signal fixture fence");
            Hr(fence->SetEventOnCompletion(serial, event), "arm fixture fence");
            Check(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "fixture fence completed");
            const auto counts = collector.Consume(slot, serial);
            Check(counts.Valid == enabled, "disabled slot cannot reuse stale GPU data");
            if (enabled)
            {
                Check(counts.Rays[RAY_TELEMETRY_REFLECTION] == (uint64_t(1) << 32) + 32,
                      "GPU low-word overflow carries once");
                Check(counts.Rays[RAY_TELEMETRY_GI] == 64, "slot reuse clears the previous frame");
                Check(counts.Rays[RAY_TELEMETRY_SHADOW] == 64 && counts.Rays[RAY_TELEMETRY_AO] == 64,
                      "shadow and AO use separate counter channels");
                Check(counts.ProviderKnown(RayTelemetry::Radiance), "producer coverage matches readback");
            }
            Check(!collector.Consume(slot, serial + 1).Valid, "GPU readback is matched by serial");
            Hr(allocator->Reset(), "reset allocator");
            Hr(commands->Reset(allocator.Get(), nullptr), "reset list");
        }
        Hr(commands->Close(), "close unused list");
        collector.Shutdown();
        if (debugEnabled)
        {
            ComPtr<ID3D12InfoQueue> info;
            Hr(device.As(&info), "read debug queue");
            uint64_t errorCount = 0;
            for (UINT64 index = 0; index < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
            {
                SIZE_T bytes = 0;
                Hr(info->GetMessage(index, nullptr, &bytes), "query debug message");
                std::vector<uint8_t> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Hr(info->GetMessage(index, message, &bytes), "read debug message");
                if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                    message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
                {
                    if (errorCount < 8)
                    {
                        std::cerr << message->pDescription << '\n';
                    }
                    ++errorCount;
                }
            }
            Check(errorCount == 0, "D3D12 errors must be zero");
        }
        CloseHandle(event);
        std::cout << "raytelemetry GPU tests: PASS; debug=" << debugEnabled << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        if (event)
        {
            CloseHandle(event);
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
