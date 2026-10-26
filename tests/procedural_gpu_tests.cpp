#include "procedural/proceduraltexturegpu.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <wrl.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;

namespace
{
    void Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct Harness
    {
        ComPtr<ID3D12Device> Device;
        ComPtr<ID3D12CommandQueue> Queue;
        ComPtr<ID3D12CommandAllocator> Allocator;
        ComPtr<ID3D12GraphicsCommandList> Commands;
        ComPtr<ID3D12Fence> Fence;
        ComPtr<ID3D12InfoQueue> Diagnostics;
        ProcStudio::TextureGpu Generator;
        UINT64 Submission = 0;
        HANDLE Completion = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        std::string Error;

        explicit Harness(bool warp)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
            }
            ComPtr<IDXGIFactory4> factory;
            Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
            ComPtr<IDXGIAdapter1> adapter;
            if (warp)
            {
                Require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
            }
            else
            {
                Require(SUCCEEDED(factory->EnumAdapters1(0, &adapter)), "Hardware adapter");
            }
            DXGI_ADAPTER_DESC1 description{};
            adapter->GetDesc1(&description);
            std::wcout << L"Adapter: " << description.Description << L'\n';
            Require(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device))),
                    "D3D12 device");
            Device.As(&Diagnostics);
            D3D12_COMMAND_QUEUE_DESC queue{};
            Require(SUCCEEDED(Device->CreateCommandQueue(&queue, IID_PPV_ARGS(&Queue))), "queue");
            Require(SUCCEEDED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Allocator))),
                    "allocator");
            Require(SUCCEEDED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(), nullptr,
                                                        IID_PPV_ARGS(&Commands))),
                    "commands");
            Require(SUCCEEDED(Commands->Close()), "initial close");
            Require(SUCCEEDED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence))), "fence");
            Require(Completion != nullptr, "completion event");
            Require(Generator.Initialize(Device.Get(), Error), Error);
        }

        ~Harness()
        {
            CloseHandle(Completion);
        }

        void Begin()
        {
            Require(SUCCEEDED(Allocator->Reset()), "allocator reset");
            Require(SUCCEEDED(Commands->Reset(Allocator.Get(), nullptr)), "list reset");
        }

        void Submit()
        {
            Require(SUCCEEDED(Commands->Close()), "list close");
            ID3D12CommandList* lists[] = {Commands.Get()};
            Queue->ExecuteCommandLists(1, lists);
            Require(SUCCEEDED(Queue->Signal(Fence.Get(), ++Submission)), "queue signal");
            Require(SUCCEEDED(Fence->SetEventOnCompletion(Submission, Completion)), "completion notification");
            Require(WaitForSingleObject(Completion, 60000) == WAIT_OBJECT_0, "GPU completion timeout");
            Require(Fence->GetCompletedValue() != UINT64_MAX, "GPU device removed");
        }

        void Compare(const ProcStudio::TextureParams& params, const std::string& label)
        {
            Begin();
            Require(Generator.Generate(Commands.Get(), params, 37, 29, Error), Error);
            Require(Generator.CopyToReadback(Commands.Get(), Error), Error);
            Submit();
            ProcStudio::Image actual;
            Require(Generator.Readback(actual, Error), Error);
            const auto reference = ProcStudio::GenerateTexture(params, 37, 29);
            Require(actual.Width == 37 && actual.Height == 29 && actual.Pixels.size() == reference.Pixels.size(),
                    "dimensions");
            float maximum = 0;
            for (size_t i = 0; i < actual.Pixels.size(); ++i)
            {
                const auto& a = actual.Pixels[i];
                const auto& b = reference.Pixels[i];
                Require(std::isfinite(a.X) && std::isfinite(a.Y) && std::isfinite(a.Z) && a.W == 1,
                        "finite opaque pixels");
                maximum = std::max({maximum, std::abs(a.X - b.X), std::abs(a.Y - b.Y), std::abs(a.Z - b.Z)});
            }
            Require(maximum < 0.003f, label + " maximum error: " + std::to_string(maximum));
        }

        void ComparePreview(const ProcStudio::TextureParams& params, int size, int tiles)
        {
            Begin();
            Require(Generator.Generate(Commands.Get(), params, 37, 29, Error), Error);
            auto* packed = Generator.Preview(Commands.Get(), size, tiles, Error);
            Require(packed != nullptr, Error);
            ComPtr<ID3D12Resource> readback;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            auto description = packed->GetDesc();
            description.Flags = D3D12_RESOURCE_FLAG_NONE;
            Require(SUCCEEDED(Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                              IID_PPV_ARGS(&readback))),
                    "preview readback allocation");
            Commands->CopyResource(readback.Get(), packed);
            Submit();
            const auto image = ProcStudio::GenerateTexture(params, 37, 29);
            const UINT pitch = (size * 4 + 255) & ~255;
            void* mapped = nullptr;
            D3D12_RANGE range{0, size_t(pitch) * size};
            Require(SUCCEEDED(readback->Map(0, &range, &mapped)), "preview readback map");
            int maximum = 0;
            for (int y = 0; y < size; ++y)
            {
                const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const char*>(mapped) + y * pitch);
                for (int x = 0; x < size; ++x)
                {
                    const auto sample = image.Sample((x + 0.5f) / size * tiles, (y + 0.5f) / size * tiles);
                    const float channels[] = {sample.X, sample.Y, sample.Z};
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        float value = std::clamp(channels[channel], 0.0f, 1.0f);
                        if (params.Output == ProcStudio::TextureOutput::Color)
                        {
                            value = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1 / 2.4f) - 0.055f;
                        }
                        const int expected = static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255 + 0.5f);
                        const int actual = (row[x] >> (channel * 8)) & 255;
                        maximum = std::max(maximum, std::abs(expected - actual));
                    }
                    Require((row[x] >> 24) == 255, "preview alpha");
                }
            }
            const D3D12_RANGE written{0, 0};
            readback->Unmap(0, &written);
            Require(maximum <= 1, "preview bilinear/tiles/sRGB error: " + std::to_string(maximum));
        }

        void CheckDiagnostics()
        {
            if (!Diagnostics)
            {
                return;
            }
            for (UINT64 index = 0; index < Diagnostics->GetNumStoredMessages(); ++index)
            {
                SIZE_T bytes = 0;
                Diagnostics->GetMessage(index, nullptr, &bytes);
                std::vector<char> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Diagnostics->GetMessage(index, message, &bytes);
                Require(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR &&
                            message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION,
                        message->pDescription);
            }
            std::cout << "D3D12 debug layer: " << (Diagnostics ? "checked" : "unavailable") << '\n';
        }
    };
}

// Drive the editor's actual job/preview functions on this real D3D12 test queue.
struct RendererState
{
    static constexpr UINT g_kFRAME_COUNT = 3;
};

class GraphicsDevice
{
  public:
    inline static Harness* Active = nullptr;

    static ID3D12Device* GetDevice()
    {
        return Active->Device.Get();
    }

    static ID3D12GraphicsCommandList* GetCommandList()
    {
        return Active->Commands.Get();
    }

    static UINT64 GetCompletedFence()
    {
        return Active->Fence->GetCompletedValue();
    }

    static UINT64 GetPendingSubmissionFence()
    {
        return Active->Submission + 1;
    }

    static bool WaitForGpuIdle()
    {
        return GetCompletedFence() == Active->Submission;
    }
};

struct EditorDescriptorLease
{
    ComPtr<ID3D12DescriptorHeap> Heap;
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu{};
};

namespace EditorDescriptors
{
    std::shared_ptr<EditorDescriptorLease> Allocate()
    {
        auto lease = std::make_shared<EditorDescriptorLease>();
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = 1;
        Require(SUCCEEDED(GraphicsDevice::GetDevice()->CreateDescriptorHeap(&description, IID_PPV_ARGS(&lease->Heap))),
                "editor descriptor heap");
        lease->Cpu = lease->Heap->GetCPUDescriptorHandleForHeapStart();
        return lease;
    }
}

#include "editor-compute-under-test.h"

void TestEditorTransitions(Harness& harness)
{
    using namespace ProcStudioEditorDetail;
    GraphicsDevice::Active = &harness;
    State state;
    state.Texture.Kind = TextureKind::FractalWater;
    const auto frame = [&] {
        harness.Begin();
        PumpJobs(state);
        RenderPreview(state);
        harness.Submit();
    };
    frame();
    Require(state.GpuTextureValid && !state.ReadbackPending && state.TextureImage.Pixels.empty(),
            "flat preview must stay on GPU without CPU readback");
    state.View = ViewMode::Model3D;
    state.PreviewDirty = true;
    bool animatedPreviewStarted = false;
    for (int index = 0; index < 20; ++index)
    {
        state.Texture.Time += 0.17f;
        state.TextureJob.Dirty = true;
        frame();
        animatedPreviewStarted |= state.PreviewJob.Running || !state.Pixels.empty();
    }
    Require(animatedPreviewStarted, "animated 3D preview must render while newer GPU readbacks are pending");
    state.View = ViewMode::Flat2D;
    state.PreviewDirty = true;
    if (state.PreviewJob.Running)
    {
        state.PreviewJob.Result.wait();
    }
    frame();
    Require(!state.PreviewJob.Running, "returning to GPU flat view must drain completed software jobs");
    state.Texture.Kind = TextureKind::Mandelbrot;
    state.TextureJob.Dirty = true;
    frame();
    if (state.ReadbackPending)
    {
        frame();
    }
    if (state.TextureJob.Running)
    {
        state.TextureJob.Result.wait();
    }
    frame();
    Require(!state.GpuTextureValid && state.CpuTextureValid && !state.TextureImage.Pixels.empty(),
            "escape fractal must fall back to CPU after switching from GPU");
    state.GpuAvailable = false;
    state.Texture.Kind = TextureKind::Geometric;
    state.TextureJob.Dirty = true;
    frame();
    if (state.TextureJob.Running)
    {
        state.TextureJob.Result.wait();
    }
    frame();
    Require(!state.GpuTextureValid && state.CpuTextureValid && !state.TextureImage.Pixels.empty(),
            "unavailable GPU must preserve CPU texture generation");
    state.Gpu.Release();
    state.TextureCompute.Release();
    std::cout << "Editor transitions: flat GPU / animated 3D / CPU escape and unavailable GPU fallback PASS\n";
}

int main(int argc, char** argv)
{
    try
    {
        using namespace ProcStudio;
        Harness harness(argc > 1 && std::string(argv[1]) == "--warp");
        TextureParams params;
        for (auto kind : {TextureKind::Mandelbrot, TextureKind::Julia})
        {
            params.Kind = kind;
            Require(!TextureGpu::Supports(params), "double-precision fractals must use CPU");
        }
        int comparisons = 0;
        for (int kind = 0; kind < static_cast<int>(TextureKind::Count); ++kind)
        {
            params = {};
            params.Kind = static_cast<TextureKind>(kind);
            params.Time = 1.37f;
            if (!TextureGpu::Supports(params))
            {
                continue;
            }
            for (int output = 0; output < 3; ++output)
            {
                params.Output = static_cast<TextureOutput>(output);
                harness.Compare(params, "kind=" + std::to_string(kind) + " output=" + std::to_string(output));
                ++comparisons;
            }
        }
        for (int basis = 0; basis < static_cast<int>(Basis::Count); ++basis)
        {
            for (int fractal = 0; fractal < static_cast<int>(FractalType::Count); ++fractal)
            {
                for (bool tileable : {false, true})
                {
                    params = {};
                    params.Noise.BasisType = static_cast<Basis>(basis);
                    params.Noise.Fractal = static_cast<FractalType>(fractal);
                    params.Noise.Seed = 0xfedcba98;
                    params.Noise.Frequency = 3.5f;
                    params.Noise.Lacunarity = 1.7f;
                    params.Noise.Direction = 0.73f;
                    params.Noise.Anisotropy = 2.3f;
                    params.Noise.KernelFrequency = 1.25f;
                    params.Noise.Phase = 0.41f;
                    params.Tileable = tileable;
                    harness.Compare(params, "basis=" + std::to_string(basis) + " fractal=" + std::to_string(fractal));
                    ++comparisons;
                }
            }
        }
        for (int pattern = 0; pattern < 6; ++pattern)
        {
            params = {};
            params.Kind = TextureKind::Geometric;
            params.Pattern = static_cast<GeometricPattern>(pattern);
            params.PatternSize = 0.95f;
            params.PatternSoftness = 0.15f;
            params.Contrast = 1.3f;
            params.Bias = -0.1f;
            harness.Compare(params, "geometric=" + std::to_string(pattern));
            ++comparisons;
        }
        for (int output = 0; output < 3; ++output)
        {
            params = {};
            params.Output = static_cast<TextureOutput>(output);
            harness.ComparePreview(params, 67, 4);
            harness.ComparePreview(params, 512, 1);
        }
        Image exported;
        std::string error;
        Require(GenerateTextureGpu(harness.Device.Get(), params, 53, 41, exported, error), error);
        Require(exported.Width == 53 && exported.Height == 41, "export dimensions");
        TestEditorTransitions(harness);
        harness.CheckDiagnostics();
        std::cout << "GPU texture parity: PASS (" << comparisons
                  << " image cases, 6 previews, separate export queue)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
