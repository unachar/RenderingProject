#pragma once
#include <d3d12.h>
#include <algorithm>
#include <cstdint>
#include <dxgi1_4.h>
#include <wrl/client.h>

// Updated before recording each frame. Budgets describe this process, not total GPU usage.
namespace GpuMemoryBudget
{
    struct Settings
    {
        bool Enabled = true;
        uint32_t TextureMiB = 512;
        uint32_t TargetPercent = 85;
        uint32_t IdleTextureFrames = 300;
        uint32_t UploadMiBPerFrame = 8;
        uint32_t ShadowResolution = 1024;
        bool ReleaseInactive = true;
        bool TransientAliasing = true;
    };

    struct Statistics
    {
        uint64_t Usage = 0;
        uint64_t Budget = 0;
        uint64_t Target = 0;
        uint64_t TextureBytes = 0;
        uint64_t TextureBudget = 0;
        uint64_t EvictedBytes = 0;
        uint64_t CompressedReplacements = 0;
        bool Valid = false;
    };

    inline Settings Options;
    inline Statistics Stats;
    inline Microsoft::WRL::ComPtr<IDXGIAdapter3> Adapter;

    inline void Initialize(ID3D12Device* device)
    {
        Adapter.Reset();
        Stats = {};
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        if (device && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&Adapter));
        }
    }

    inline void Update()
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        Stats.Valid = Adapter && SUCCEEDED(Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info));
        Stats.Usage = info.CurrentUsage;
        Stats.Budget = info.Budget;
        Stats.Target = info.Budget * std::clamp(Options.TargetPercent, 50u, 95u) / 100;
        const uint64_t limit = uint64_t(std::clamp(Options.TextureMiB, 32u, 4096u)) * 1024 * 1024;
        Stats.TextureBudget = limit;
        if (Options.Enabled && Stats.Valid)
        {
            const uint64_t other = Stats.Usage > Stats.TextureBytes ? Stats.Usage - Stats.TextureBytes : 0;
            const uint64_t available = Stats.Target > other ? Stats.Target - other : 0;
            Stats.TextureBudget = (std::min)(limit, available);
        }
    }

    inline bool UnderPressure()
    {
        return Options.Enabled && Stats.Valid && Stats.Usage > Stats.Target;
    }
}
