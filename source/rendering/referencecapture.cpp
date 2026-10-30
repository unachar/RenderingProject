#include "pch.h"
#include "referencecapture.h"
#include "memoryallocator.h"

namespace ReferenceCapture
{
    namespace
    {
        Settings g_Settings{};
        State g_State = State::Idle;
        uint32_t g_AccumulatedSamples = 0;
        ComPtr<ID3D12Resource> g_Accumulation;
        uint32_t g_TargetWidth = 0;
        uint32_t g_TargetHeight = 0;

        // splitmix32 finalizer: strong avalanche for seed spreading.
        uint32_t Mix(uint32_t value)
        {
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            value ^= value >> 16;
            return value;
        }
    } // namespace

    bool Begin(const Settings& settings)
    {
        if (g_State == State::Running)
        {
            return false;
        }
        if (settings.Width == 0 || settings.Height == 0 || settings.SamplesPerPixel == 0 ||
            settings.SamplesPerDispatch == 0)
        {
            return false;
        }
        g_Settings = settings;
        g_AccumulatedSamples = 0;
        g_State = State::Running;
        return true;
    }

    void Cancel()
    {
        if (g_State == State::Running)
        {
            g_State = State::Cancelled;
        }
    }

    bool RecordDispatchComplete()
    {
        if (g_State != State::Running)
        {
            return g_State == State::Complete;
        }
        g_AccumulatedSamples += g_Settings.SamplesPerDispatch;
        if (g_AccumulatedSamples >= g_Settings.SamplesPerPixel)
        {
            g_AccumulatedSamples = g_Settings.SamplesPerPixel;
            g_State = State::Complete;
            return true;
        }
        return false;
    }

    State GetState()
    {
        return g_State;
    }

    const Settings& GetSettings()
    {
        return g_Settings;
    }

    uint32_t GetAccumulatedSamples()
    {
        return g_AccumulatedSamples;
    }

    float Progress()
    {
        if (g_Settings.SamplesPerPixel == 0)
        {
            return 0.0f;
        }
        return static_cast<float>(g_AccumulatedSamples) / static_cast<float>(g_Settings.SamplesPerPixel);
    }

    uint32_t DeriveSeed(uint32_t pixelIndex, uint32_t sampleIndex)
    {
        uint32_t value = g_Settings.Seed + 0x9e3779b9u;
        value = Mix(value ^ pixelIndex);
        value = Mix(value ^ sampleIndex);
        return value;
    }

    bool EnsureTargets(ID3D12Device* device)
    {
        if (device == nullptr || g_Settings.Width == 0 || g_Settings.Height == 0)
        {
            return false;
        }
        if (g_Accumulation && g_TargetWidth == g_Settings.Width && g_TargetHeight == g_Settings.Height)
        {
            return true;
        }
        ReleaseTargets();
        const auto description = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, g_Settings.Width,
                                                               g_Settings.Height, 1, 1, 1, 0,
                                                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (ID3D12Resource* allocated =
                MemoryAllocator::CreateResourceAutoLifetime(description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr))
        {
            g_Accumulation.Attach(allocated);
        }
        else
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&g_Accumulation))))
            {
                return false;
            }
        }
        g_TargetWidth = g_Settings.Width;
        g_TargetHeight = g_Settings.Height;
        return true;
    }

    void ReleaseTargets()
    {
        g_Accumulation.Reset();
        g_TargetWidth = 0;
        g_TargetHeight = 0;
    }

    ID3D12Resource* GetAccumulationResource()
    {
        return g_Accumulation.Get();
    }
} // namespace ReferenceCapture
