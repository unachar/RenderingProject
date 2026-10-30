#include "pch.h"
#include "surfacedecals.h"
#include "memoryallocator.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace SurfaceDecals
{
    namespace
    {
        std::vector<Decal> g_Decals;

        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D12Resource> g_Targets[kDBufferCount];
        ComPtr<ID3D12DescriptorHeap> g_RtvHeap;
        ComPtr<ID3D12DescriptorHeap> g_SrvHeap;
        uint32_t g_TargetWidth = 0;
        uint32_t g_TargetHeight = 0;
        UINT g_RtvIncrement = 0;
        UINT g_SrvIncrement = 0;

        constexpr DXGI_FORMAT kDBufferFormats[kDBufferCount] = {DXGI_FORMAT_R8G8B8A8_UNORM,
                                                                DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                                DXGI_FORMAT_R8G8B8A8_UNORM};
    }

    void Clear()
    {
        g_Decals.clear();
    }

    uint32_t Add(const Decal& decal)
    {
        if (g_Decals.size() >= kMaxDecals)
        {
            return kNoTexture;
        }
        g_Decals.push_back(decal);
        return static_cast<uint32_t>(g_Decals.size() - 1);
    }

    uint32_t Count()
    {
        return static_cast<uint32_t>(g_Decals.size());
    }

    const Decal& Get(uint32_t index)
    {
        return g_Decals[index];
    }

    uint32_t PackAll(std::vector<GpuDecal>& out)
    {
        std::vector<uint32_t> order(g_Decals.size());
        std::iota(order.begin(), order.end(), 0u);
        std::stable_sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) {
            return g_Decals[a].Priority < g_Decals[b].Priority;
        });
        out.clear();
        out.reserve(order.size());
        for (const uint32_t index : order)
        {
            const Decal& decal = g_Decals[index];
            GpuDecal packed{};
            std::memcpy(packed.WorldToLocal, decal.WorldToLocal, sizeof(packed.WorldToLocal));
            packed.ExtentsOpacity[0] = 1.0f;
            packed.ExtentsOpacity[1] = 1.0f;
            packed.ExtentsOpacity[2] = 1.0f;
            packed.ExtentsOpacity[3] = decal.Opacity;
            packed.Textures[0] = decal.AlbedoTexture;
            packed.Textures[1] = decal.NormalTexture;
            packed.Textures[2] = decal.RoughnessTexture;
            packed.Textures[3] = decal.LayerMask;
            packed.Fade[0] = decal.AngleFadeStartCos;
            packed.Fade[1] = decal.AngleFadeEndCos;
            packed.Fade[2] = decal.Roughness;
            packed.Fade[3] = 0.0f;
            out.push_back(packed);
        }
        return static_cast<uint32_t>(out.size());
    }

    bool EnsureTargets(ID3D12Device* device, uint32_t width, uint32_t height)
    {
        if (g_Targets[0] && g_TargetWidth == width && g_TargetHeight == height)
        {
            return true;
        }
        ReleaseTargets();
        if (!device || width == 0 || height == 0)
        {
            return false;
        }
        if (!g_RtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
            rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvDesc.NumDescriptors = kDBufferCount;
            if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_RtvHeap))))
            {
                return false;
            }
            D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
            srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvDesc.NumDescriptors = kDBufferCount;
            srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_SrvHeap))))
            {
                return false;
            }
            g_RtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            g_SrvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        for (uint32_t i = 0; i < kDBufferCount; ++i)
        {
            const auto description = CD3DX12_RESOURCE_DESC::Tex2D(kDBufferFormats[i], width, height, 1, 1, 1, 0,
                                                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            D3D12_CLEAR_VALUE clear{};
            clear.Format = kDBufferFormats[i];
            // Through D3D12MA when its flag is on; committed fallback.
            if (ID3D12Resource* allocated = MemoryAllocator::CreateResourceAutoLifetime(
                    description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear))
            {
                g_Targets[i].Attach(allocated);
            }
            else
            {
                auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                           IID_PPV_ARGS(&g_Targets[i]))))
                {
                    ReleaseTargets();
                    return false;
                }
            }
            g_Targets[i]->SetName(i == 0 ? L"DBuffer Albedo" : i == 1 ? L"DBuffer Normal" : L"DBuffer Roughness Mask");
            device->CreateRenderTargetView(g_Targets[i].Get(), nullptr,
                                           CD3DX12_CPU_DESCRIPTOR_HANDLE(g_RtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                                                         static_cast<INT>(i), g_RtvIncrement));
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = kDBufferFormats[i];
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(g_Targets[i].Get(), &srv,
                                             CD3DX12_CPU_DESCRIPTOR_HANDLE(g_SrvHeap->GetCPUDescriptorHandleForHeapStart(),
                                                                           static_cast<INT>(i), g_SrvIncrement));
        }
        g_TargetWidth = width;
        g_TargetHeight = height;
        return true;
    }

    void ReleaseTargets()
    {
        for (auto& target : g_Targets)
        {
            target.Reset();
        }
        g_TargetWidth = 0;
        g_TargetHeight = 0;
    }

    bool TargetsReady()
    {
        return g_Targets[0] != nullptr;
    }

    ID3D12Resource* TargetResource(uint32_t index)
    {
        return index < kDBufferCount ? g_Targets[index].Get() : nullptr;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE TargetRtv(uint32_t index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_RtvHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(index),
                                             g_RtvIncrement);
    }

    ID3D12DescriptorHeap* TargetSrvHeap()
    {
        return g_SrvHeap.Get();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE TargetSrvGpuStart()
    {
        return g_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    }
} // namespace SurfaceDecals
