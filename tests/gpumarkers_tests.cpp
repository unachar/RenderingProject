#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include "../source/rendering/gpumarkers.h"

int main()
{
#if defined(_DEBUG) && (defined(_M_X64) || defined(_M_ARM64))
    assert(GpuMarkers::Encode(nullptr).Bytes == 0);
    assert(GpuMarkers::Encode("").Bytes == 0);
    const auto record = GpuMarkers::Encode("Material Resolve");
    assert(D3D12_EVENT_METADATA == 2);
    assert(record.Bytes > 24 && record.Bytes <= sizeof(record.Words));
    assert((record.Bytes % sizeof(UINT64)) == 0);
    const auto eventType = (record.Words[0] & PixEventsLegacy::PIXEventsTypeReadMask) >>
        PixEventsLegacy::PIXEventsTypeBitShift;
    assert(eventType == PixEventsLegacy::PIXEvent_SetMarker_NoArgs);
    assert(record.Words[1] == 0); // No color override.
    assert((record.Words[2] & PixEventsLegacy::PIXEventsStringIsANSIReadMask) != 0);
    assert(std::strcmp(reinterpret_cast<const char*>(record.Words + 3), "Material Resolve") == 0);
    const auto longRecord = GpuMarkers::Encode(std::string(2048, 'A').c_str());
    assert(longRecord.Bytes <= sizeof(longRecord.Words));
    std::puts("PIX GPU marker framing, label roundtrip, empty/truncated inputs: PASS (no PIX runtime linked)");
#endif
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Debug> debug;
    const bool debugAvailable = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (debugAvailable)
    {
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    assert(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
    assert(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    assert(SUCCEEDED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))));
    assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&commands))));
    GpuMarkers::Write(commands.Get(), "Material Resolve");
    GpuMarkers::Write(commands.Get(), "Custom TSR");
    GpuMarkers::Write(commands.Get(), "");
    GpuMarkers::Write(nullptr, "No command list");
    assert(SUCCEEDED(commands->Close()));
    ID3D12CommandList* lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    assert(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    assert(completed);
    assert(SUCCEEDED(queue->Signal(fence.Get(), 1)));
    assert(SUCCEEDED(fence->SetEventOnCompletion(1, completed)));
    assert(WaitForSingleObject(completed, 30000) == WAIT_OBJECT_0);
    CloseHandle(completed);
    ComPtr<ID3D12InfoQueue> messages;
    if (SUCCEEDED(device.As(&messages)))
    {
        const auto count = messages->GetNumStoredMessages();
        for (UINT64 index = 0; index < count; ++index)
        {
            SIZE_T size = 0;
            assert(SUCCEEDED(messages->GetMessage(index, nullptr, &size)));
            std::string bytes(size, '\0');
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(&bytes[0]);
            assert(SUCCEEDED(messages->GetMessage(index, message, &size)));
            assert(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR &&
                   message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION);
        }
    }
    std::printf("Native D3D12 marker submission: PASS (debug layer=%d)\n", int(debugAvailable));
}
