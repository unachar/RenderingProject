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

    namespace
    {
        ComPtr<ID3D12RootSignature> g_RasterRootSignature;
        ComPtr<ID3D12PipelineState> g_RasterPso;
        ComPtr<ID3D12RootSignature> g_CompositeRootSignature;
        ComPtr<ID3D12PipelineState> g_CompositePso;
        DXGI_FORMAT g_CompositeFormats[3] = {};

        bool CreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& description,
                                 ComPtr<ID3D12RootSignature>& output)
        {
            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> errors;
            if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
            {
                return false;
            }
            return SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                         IID_PPV_ARGS(&output)));
        }

        bool LoadShader(const wchar_t* path, ComPtr<ID3DBlob>& output)
        {
            return SUCCEEDED(D3DReadFileToBlob(path, &output));
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC BasePsoDesc()
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            desc.DepthStencilState.DepthEnable = FALSE;
            desc.DepthStencilState.StencilEnable = FALSE;
            desc.SampleMask = UINT_MAX;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.SampleDesc.Count = 1;
            return desc;
        }

        void EnableOverBlend(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, UINT target, UINT writeMask)
        {
            auto& rt = desc.BlendState.RenderTarget[target];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = static_cast<UINT8>(writeMask);
        }

        bool EnsureRasterPipeline(ID3D12Device* device)
        {
            if (g_RasterPso)
            {
                return true;
            }
            CD3DX12_DESCRIPTOR_RANGE depthRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE normalRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
            CD3DX12_DESCRIPTOR_RANGE textureRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 512, 0, 1);
            // Depth, GBuffer normal and the material texture heap are
            // three separate tables: they live in different regions of
            // the engine heap, so no single table base can span them.
            CD3DX12_ROOT_PARAMETER params[5]{};
            params[0].InitAsConstants(20, 0);
            params[1].InitAsConstants(28, 1);
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            params[2].InitAsDescriptorTable(1, &depthRange, D3D12_SHADER_VISIBILITY_PIXEL);
            params[3].InitAsDescriptorTable(1, &normalRange, D3D12_SHADER_VISIBILITY_PIXEL);
            params[4].InitAsDescriptorTable(1, &textureRange, D3D12_SHADER_VISIBILITY_PIXEL);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            CD3DX12_ROOT_SIGNATURE_DESC rootDesc;
            rootDesc.Init(_countof(params), params, 1, &sampler,
                          D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
            if (!CreateRootSignature(device, rootDesc, g_RasterRootSignature))
            {
                return false;
            }
            ComPtr<ID3DBlob> vs;
            ComPtr<ID3DBlob> ps;
            if (!LoadShader(L"shader/hlsl/build/DecalRasterVS.cso", vs) ||
                !LoadShader(L"shader/hlsl/build/DecalRasterPS.cso", ps))
            {
                return false;
            }
            auto desc = BasePsoDesc();
            desc.pRootSignature = g_RasterRootSignature.Get();
            desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            desc.NumRenderTargets = kDBufferCount;
            for (UINT i = 0; i < kDBufferCount; ++i)
            {
                desc.RTVFormats[i] = kDBufferFormats[i];
                EnableOverBlend(desc, i, D3D12_COLOR_WRITE_ENABLE_ALL);
            }
            desc.BlendState.AlphaToCoverageEnable = FALSE;
            desc.BlendState.IndependentBlendEnable = TRUE;
            return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_RasterPso)));
        }

        bool EnsureCompositePipeline(ID3D12Device* device, const DXGI_FORMAT formats[3])
        {
            if (g_CompositePso && std::memcmp(g_CompositeFormats, formats, sizeof(g_CompositeFormats)) == 0)
            {
                return true;
            }
            g_CompositePso.Reset();
            if (!g_CompositeRootSignature)
            {
                CD3DX12_DESCRIPTOR_RANGE dbufferRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
                CD3DX12_ROOT_PARAMETER params[1]{};
                params[0].InitAsDescriptorTable(1, &dbufferRange, D3D12_SHADER_VISIBILITY_PIXEL);
                CD3DX12_ROOT_SIGNATURE_DESC rootDesc;
                rootDesc.Init(_countof(params), params, 0, nullptr,
                              D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
                if (!CreateRootSignature(device, rootDesc, g_CompositeRootSignature))
                {
                    return false;
                }
            }
            ComPtr<ID3DBlob> vs;
            ComPtr<ID3DBlob> ps;
            if (!LoadShader(L"shader/hlsl/build/DecalRasterVS.cso", vs) ||
                !LoadShader(L"shader/hlsl/build/DecalCompositePS.cso", ps))
            {
                return false;
            }
            auto desc = BasePsoDesc();
            desc.pRootSignature = g_CompositeRootSignature.Get();
            desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            desc.NumRenderTargets = 3;
            const UINT writeMasks[3] = {D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALL,
                                        D3D12_COLOR_WRITE_ENABLE_GREEN};
            for (UINT i = 0; i < 3; ++i)
            {
                desc.RTVFormats[i] = formats[i];
                EnableOverBlend(desc, i, writeMasks[i]);
            }
            desc.BlendState.IndependentBlendEnable = TRUE;
            if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_CompositePso))))
            {
                return false;
            }
            std::memcpy(g_CompositeFormats, formats, sizeof(g_CompositeFormats));
            return true;
        }
    }

    bool Render(const RenderParams& params)
    {
        if (Count() == 0 || !params.Device || !params.List)
        {
            return true;
        }
        if (!EnsureTargets(params.Device, params.Width, params.Height) ||
            !EnsureRasterPipeline(params.Device) || !EnsureCompositePipeline(params.Device, params.GBufferFormats))
        {
            return false;
        }
        std::vector<GpuDecal> packed;
        PackAll(packed);
        if (packed.empty())
        {
            return true;
        }
        ID3D12GraphicsCommandList* list = params.List;
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(params.Width),
                                      static_cast<float>(params.Height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(params.Width), static_cast<LONG>(params.Height)};

        // Rasterize: DBuffer targets PSR -> RT, one fullscreen draw per
        // decal in packed (priority) order.
        D3D12_RESOURCE_BARRIER barriers[kDBufferCount]{};
        for (uint32_t i = 0; i < kDBufferCount; ++i)
        {
            barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(g_Targets[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        list->ResourceBarrier(kDBufferCount, barriers);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kDBufferCount];
        for (uint32_t i = 0; i < kDBufferCount; ++i)
        {
            rtvs[i] = TargetRtv(i);
            const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            list->ClearRenderTargetView(rtvs[i], clear, 0, nullptr);
        }
        list->OMSetRenderTargets(kDBufferCount, rtvs, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* engineHeap = params.EngineHeap;
        list->SetDescriptorHeaps(1, &engineHeap);
        list->SetGraphicsRootSignature(g_RasterRootSignature.Get());
        list->SetPipelineState(g_RasterPso.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        float frameConstants[20];
        std::memcpy(frameConstants, params.InvViewProjection, sizeof(params.InvViewProjection));
        std::memcpy(frameConstants + 16, params.CameraPosition, sizeof(params.CameraPosition));
        list->SetGraphicsRoot32BitConstants(0, 20, frameConstants, 0);
        list->SetGraphicsRootDescriptorTable(2, params.DepthSrv);
        list->SetGraphicsRootDescriptorTable(3, params.GBufferNormalSrv);
        list->SetGraphicsRootDescriptorTable(4, params.TextureTable);
        for (const GpuDecal& decal : packed)
        {
            list->SetGraphicsRoot32BitConstants(1, 28, &decal, 0);
            list->DrawInstanced(3, 1, 0, 0);
        }
        for (uint32_t i = 0; i < kDBufferCount; ++i)
        {
            barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(g_Targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        list->ResourceBarrier(kDBufferCount, barriers);

        // Composite: GBuffer targets PSR -> RT, one fullscreen draw,
        // then back to PSR (the state the caller keeps them in).
        D3D12_RESOURCE_BARRIER gbufferBarriers[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            gbufferBarriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                params.GBufferTargets[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        list->ResourceBarrier(3, gbufferBarriers);
        list->OMSetRenderTargets(3, params.GBufferRtvs, FALSE, nullptr);
        ID3D12DescriptorHeap* moduleHeap = TargetSrvHeap();
        list->SetDescriptorHeaps(1, &moduleHeap);
        list->SetGraphicsRootSignature(g_CompositeRootSignature.Get());
        list->SetPipelineState(g_CompositePso.Get());
        list->SetGraphicsRootDescriptorTable(0, TargetSrvGpuStart());
        list->DrawInstanced(3, 1, 0, 0);
        for (uint32_t i = 0; i < 3; ++i)
        {
            gbufferBarriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                params.GBufferTargets[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        list->ResourceBarrier(3, gbufferBarriers);
        return true;
    }
} // namespace SurfaceDecals
