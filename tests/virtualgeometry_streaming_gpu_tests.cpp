#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "virtualgeometry/virtualgeometrypagepool.h"

using Microsoft::WRL::ComPtr;

namespace
{
    void Check(HRESULT result, const char* message)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(message);
        }
    }

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
} // 無名名前空間

int main(int argc, char** argv)
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12 debug layer required for fence validation");
        debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapters1(0, &adapter), "Hardware adapter");
        DXGI_ADAPTER_DESC1 adapterDescription{};
        adapter->GetDesc1(&adapterDescription);
        std::wcout << L"Streaming GPU: " << adapterDescription.Description << L'\n';
        if (argc == 3 && std::string(argv[1]) == "--admission")
        {
            uint64_t triangles = std::stoull(argv[2]);
            uint64_t assets = (triangles + 1638399) / 1638400;
            auto capacity = [](uint64_t count) {
                uint64_t result = 1024;
                while (result < count)
                {
                    result *= 2;
                }
                return result;
            };
            uint64_t metadataBytes = capacity(assets * 37449) * 96 + capacity(assets * 4681) * 64 +
                                     capacity(assets * 42129) * 4 + capacity(assets) * 52;
            uint64_t renderingReserve = 768ull * 1024 * 1024;
            uint64_t required = metadataBytes + renderingReserve + assets * 65536;
            ComPtr<IDXGIAdapter3> memoryAdapter;
            Check(adapter.As(&memoryAdapter), "Query memory adapter");
            DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
            Check(memoryAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory),
                  "Query VRAM budget");
            uint64_t available = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
            std::cout << "Stress admission: requested triangles=" << triangles << ", metadata bytes=" << metadataBytes
                      << ", required with frame reserve=" << required << ", available VRAM budget=" << available
                      << '\n';
            if (required > available)
            {
                std::cout
                    << "SKIP: scene metadata and frame reserve exceed available VRAM; geometry streaming cannot page resident hierarchy metadata\n";
                return 78;
            }
            std::cout << "PASS: stress scene memory admission\n";
            return 0;
        }
        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12 device");
        ComPtr<ID3D12InfoQueue> info;
        Check(device.As(&info), "Debug info queue");
        D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
        heapDescription.NumDescriptors = 2;
        heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        Check(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap)), "Page descriptor heap");
        VirtualGeometryStreaming::PagePool pool;
        pool.Initialize(device.Get(), heap->GetCPUDescriptorHandleForHeapStart(),
                        device->GetDescriptorHandleIncrementSize(heapDescription.Type));
        VirtualGeometryStreaming::ResidencyManager residency;
        Require(residency.AddPages(std::vector<uint8_t>{1, 0}, 2 * 65536), "Page admission");
        residency.Request(0, 1);
        auto ticket = residency.BeginLoad(0);
        Require(bool(ticket), "Root reservation");
        std::vector<uint8_t> expected(65536);
        for (size_t offset = 0; offset < expected.size(); ++offset)
        {
            expected[offset] = uint8_t(offset * 37);
        }
        uint64_t copyFence = pool.Submit(*ticket, expected);
        residency.CompleteCopy(*ticket, copyFence);
        residency.Publish(copyFence - 1);
        Require(residency.Snapshot()[0].Flags == 0, "Unfinished transfer published");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (pool.CompletedCopyFence() < copyFence && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        Require(pool.CompletedCopyFence() >= copyFence, "Copy Queue timeout");
        residency.Publish(pool.CompletedCopyFence());
        Require(residency.Snapshot()[0].Flags == 1, "Completed Page not published");
        pool.Collect();

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeap.CreationNodeMask = readbackHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 65536;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        Check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
              "Readback buffer");
        ComPtr<ID3D12CommandQueue> graphics;
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        Check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&graphics)), "Graphics queue");
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
              "Graphics allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commands)),
              "Graphics list");
        commands->CopyBufferRegion(readback.Get(), 0, pool.Resource(ticket->Slot), 0, 65536);
        Check(commands->Close(), "Close readback");
        ID3D12CommandList* list = commands.Get();
        graphics->ExecuteCommandLists(1, &list);
        ComPtr<ID3D12Fence> fence;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Graphics fence");
        Check(graphics->Signal(fence.Get(), 1), "Signal readback");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(event != nullptr, "Readback event");
        Check(fence->SetEventOnCompletion(1, event), "Readback fence event");
        DWORD waited = WaitForSingleObject(event, 20000);
        CloseHandle(event);
        Require(waited == WAIT_OBJECT_0, "Graphics readback timeout");
        void* mapped = nullptr;
        D3D12_RANGE range{0, 65536};
        Check(readback->Map(0, &range, &mapped), "Map readback");
        bool matches = std::memcmp(mapped, expected.data(), expected.size()) == 0;
        D3D12_RANGE noWrite{0, 0};
        readback->Unmap(0, &noWrite);
        Require(matches, "Page byte comparison differs");
        auto waitCopy = [&](uint64_t value) {
            auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (pool.CompletedCopyFence() < value && std::chrono::steady_clock::now() < limit)
            {
                std::this_thread::yield();
            }
            Require(pool.CompletedCopyFence() >= value, "Replacement Copy timed out");
        };
        auto waitGraphics = [&](uint64_t value) {
            HANDLE completion = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            Require(completion != nullptr, "Retirement event");
            Check(fence->SetEventOnCompletion(value, completion), "Retirement fence event");
            DWORD completed = WaitForSingleObject(completion, 20000);
            CloseHandle(completion);
            Require(completed == WAIT_OBJECT_0, "Retirement graphics timeout");
        };
        Require(residency.AddPages(std::vector<uint8_t>{0}, 2 * 65536), "Replacement Page registration");
        residency.Request(1, 2);
        auto child = residency.BeginLoad(1);
        Require(bool(child), "Child slot reservation");
        std::vector<uint8_t> childBytes(65536, 0xa5);
        uint64_t childCopy = pool.Submit(*child, childBytes);
        residency.CompleteCopy(*child, childCopy);
        waitCopy(childCopy);
        residency.Publish(pool.CompletedCopyFence());
        ComPtr<ID3D12Fence> gate;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "Incomplete graphics gate");
        Check(graphics->Wait(gate.Get(), 1), "Block graphics before its Page read");
        Check(allocator->Reset(), "Reset retirement allocator");
        Check(commands->Reset(allocator.Get(), nullptr), "Reset retirement commands");
        commands->CopyBufferRegion(readback.Get(), 0, pool.Resource(child->Slot), 0, 65536);
        Check(commands->Close(), "Close retirement read");
        list = commands.Get();
        graphics->ExecuteCommandLists(1, &list);
        Check(graphics->Signal(fence.Get(), 2), "Signal pending graphics read");
        residency.MarkSnapshotUsed(2);
        Require(residency.Retire(1, 2), "Start child retirement");
        residency.Request(2, 3);
        residency.Collect(fence->GetCompletedValue(), pool.CompletedCopyFence());
        Require(!residency.BeginLoad(2), "Reused Page while graphics queue was blocked");
        Require(pool.Resource(child->Slot) != nullptr, "Released Page before its queued graphics read");
        Check(gate->Signal(1), "Release graphics gate");
        waitGraphics(2);
        Check(readback->Map(0, &range, &mapped), "Map old-generation read");
        matches = std::memcmp(mapped, childBytes.data(), childBytes.size()) == 0;
        readback->Unmap(0, &noWrite);
        Require(matches, "Old graphics frame saw replacement bytes");
        residency.Collect(fence->GetCompletedValue(), pool.CompletedCopyFence());
        pool.Release(child->Slot);
        Require(pool.MemoryBytes() == 65536, "Retired physical Page memory retained");
        auto replacement = residency.BeginLoad(2);
        Require(replacement && replacement->Slot == child->Slot && replacement->Generation != child->Generation,
                "Safe retired slot did not advance generation");
        std::vector<uint8_t> replacementBytes(65536, 0x5a);
        uint64_t replacementCopy = pool.Submit(*replacement, replacementBytes);
        residency.CompleteCopy(*replacement, replacementCopy);
        residency.CompleteCopy(*child, childCopy);
        residency.Publish(replacementCopy - 1);
        Require(residency.Snapshot()[2].Flags == 0, "Stale completion published new generation early");
        waitCopy(replacementCopy);
        residency.Publish(pool.CompletedCopyFence());
        Require(residency.Snapshot()[2].Flags == 1, "Replacement did not become Resident");
        Check(allocator->Reset(), "Reset replacement allocator");
        Check(commands->Reset(allocator.Get(), nullptr), "Reset replacement read");
        commands->CopyBufferRegion(readback.Get(), 0, pool.Resource(replacement->Slot), 0, 65536);
        Check(commands->Close(), "Close replacement read");
        list = commands.Get();
        graphics->ExecuteCommandLists(1, &list);
        Check(graphics->Signal(fence.Get(), 3), "Signal replacement read");
        waitGraphics(3);
        Check(readback->Map(0, &range, &mapped), "Map replacement bytes");
        matches = std::memcmp(mapped, replacementBytes.data(), replacementBytes.size()) == 0;
        readback->Unmap(0, &noWrite);
        Require(matches, "Replacement generation byte mismatch");
        pool.Shutdown();
        uint64_t errors = 0;
        for (uint64_t index = 0; index < info->GetNumStoredMessages(); ++index)
        {
            SIZE_T size = 0;
            info->GetMessage(index, nullptr, &size);
            std::vector<uint8_t> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            Check(info->GetMessage(index, message, &size), "Read debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                ++errors;
                std::cerr << message->pDescription << '\n';
            }
        }
        Require(errors == 0, "D3D12 Copy/graphics validation errors");
        std::cout
            << "PASS: production Copy Queue, publication fence, blocked-graphics retirement/reuse, replacement bytes, D3D12 errors=0\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
