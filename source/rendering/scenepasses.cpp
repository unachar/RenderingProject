#include "pch.h"
#include "memoryallocator.h"
#include "surfacedecals.h"
#include "camera.h"
#include "componentmanager.h"
#include "noisecachebindings.h"
#include "renderprofiler.h"
#include "megalights.h"
#include "transparencyeffects.h"
#include "envflags.h"
#include "transmittanceshadows.h"
#include "scenepasses.h"
#include "renderframe.h"
#include "lightingresources.h"
#include "materialbindings.h"
#include "psomanager.h"
#include "texturemanager.h"
#include "renderersettings.h"
#include "visibilitybuffer.h"
#include "occlusionculling.h"
#include "postprocesspass.h"
#include "raytracing.h"
#include "cloud/cloudrenderer.h"
#include "screenspaceeffects.h"
#include "graphicslog.h"
#include "gpumemorybudget.h"

namespace
{
    bool g_VelocityDepthReadActive = false;
    ComPtr<ID3D12Resource> g_StationaryShadowDepth, g_StationaryShadowOwner;
    ComPtr<ID3D12DescriptorHeap> g_StationaryShadowDsv, g_StationaryShadowSrv;
    ComPtr<ID3D12RootSignature> g_ShadowCopyRoot;
    ComPtr<ID3D12PipelineState> g_ShadowCopyPso;
} // 名前空間

void ScenePasses::ReleaseStaticShadowCache()
{
    g_StationaryShadowDepth.Reset();
    g_StationaryShadowOwner.Reset();
    g_StationaryShadowDsv.Reset();
    g_StationaryShadowSrv.Reset();
    g_ShadowCopyRoot.Reset();
    g_ShadowCopyPso.Reset();
    LightingResources::SetStaticShadowCacheAvailable(false);
}

bool ScenePasses::PrepareStaticShadowCache()
{
    if (!m_Device || !m_ShadowDepthBuffer || IsShadowReferenceDrawEnabled() ||
        RendererSettings::GetShadowMapMethod() != ShadowMapMethod::VirtualShadowMap ||
        Raytracing::UsesRaytracedShadows())
    {
        if (g_StationaryShadowDepth && GpuMemoryBudget::Options.Enabled && GpuMemoryBudget::Options.ReleaseInactive &&
            GraphicsDevice::WaitForGpuIdle())
        {
            ReleaseStaticShadowCache();
        }
        LightingResources::SetStaticShadowCacheAvailable(false);
        return false;
    }
    if (g_StationaryShadowDepth && g_StationaryShadowOwner.Get() == m_ShadowDepthBuffer.Get())
    {
        LightingResources::SetStaticShadowCacheAvailable(true);
        return true;
    }
    ReleaseStaticShadowCache();
    auto create = [&]() {
        const auto description = m_ShadowDepthBuffer->GetDesc();
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1;
        // Through D3D12MA when the flag is on; committed fallback otherwise.
        if (ID3D12Resource* allocated = MemoryAllocator::CreateResourceAutoLifetime(
                description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear))
        {
            g_StationaryShadowDepth.Attach(allocated);
        }
        else
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                         IID_PPV_ARGS(&g_StationaryShadowDepth))))
            {
                return false;
            }
        }
        g_StationaryShadowDepth->SetName(L"Stationary VSM Depth Cache");
        D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
        descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        descriptors.NumDescriptors = description.DepthOrArraySize;
        if (FAILED(m_Device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&g_StationaryShadowDsv))))
        {
            return false;
        }
        for (UINT layer = 0; layer < description.DepthOrArraySize; ++layer)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_D32_FLOAT;
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice = layer;
            view.Texture2DArray.ArraySize = 1;
            m_Device->CreateDepthStencilView(
                g_StationaryShadowDepth.Get(), &view,
                CD3DX12_CPU_DESCRIPTOR_HANDLE(
                    g_StationaryShadowDsv->GetCPUDescriptorHandleForHeapStart(), layer,
                    m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)));
        }
        descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptors.NumDescriptors = 1;
        descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_Device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&g_StationaryShadowSrv))))
        {
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.ArraySize = description.DepthOrArraySize;
        srv.Texture2DArray.MipLevels = 1;
        m_Device->CreateShaderResourceView(g_StationaryShadowDepth.Get(), &srv,
                                           g_StationaryShadowSrv->GetCPUDescriptorHandleForHeapStart());
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_ROOT_PARAMETER parameters[2];
        parameters[0].InitAsDescriptorTable(1, &range);
        parameters[1].InitAsConstants(1, 0);
        CD3DX12_ROOT_SIGNATURE_DESC root(2, parameters);
        ComPtr<ID3DBlob> serialized, errors, vs, ps;
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
            FAILED(m_Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                 IID_PPV_ARGS(&g_ShadowCopyRoot))) ||
            FAILED(D3DReadFileToBlob(L"shader/hlsl/build/ShadowCacheCopyVS.cso", &vs)) ||
            FAILED(D3DReadFileToBlob(L"shader/hlsl/build/ShadowCacheCopyPS.cso", &ps)))
        {
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = g_ShadowCopyRoot.Get();
        pso.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
        pso.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.SampleMask = UINT_MAX;
        pso.SampleDesc.Count = 1;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        return SUCCEEDED(m_Device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_ShadowCopyPso)));
    };
    if (!create())
    {
        ReleaseStaticShadowCache();
        WriteGraphicsLog("Stationary VSM cache unavailable; using complete page redraw\n");
        return false;
    }
    g_StationaryShadowOwner = m_ShadowDepthBuffer;
    LightingResources::SetStaticShadowCacheAvailable(true);
    return true;
}

void ScenePasses::EndStaticShadowPass()
{
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(g_StationaryShadowDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_CommandList->ResourceBarrier(1, &barrier);
}

bool ScenePasses::RestoreStaticShadowDepth(UINT index)
{
    if (!g_StationaryShadowDepth || !BeginShadowPass(index, true))
    {
        return false;
    }
    UINT layer;
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    bool clear;
    if (!LightingResources::GetShadowPassInfo(index, layer, viewport, scissor, clear))
    {
        return false;
    }
    ID3D12DescriptorHeap* heaps[] = {g_StationaryShadowSrv.Get()};
    m_CommandList->SetDescriptorHeaps(1, heaps);
    m_CommandList->SetGraphicsRootSignature(g_ShadowCopyRoot.Get());
    m_CommandList->SetPipelineState(g_ShadowCopyPso.Get());
    m_CommandList->SetGraphicsRootDescriptorTable(0, g_StationaryShadowSrv->GetGPUDescriptorHandleForHeapStart());
    m_CommandList->SetGraphicsRoot32BitConstant(1, layer, 0);
    m_CommandList->DrawInstanced(3, 1, 0, 0);
    heaps[0] = m_CbvHeap.Get();
    m_CommandList->SetDescriptorHeaps(1, heaps);
    return true;
}

void ScenePasses::BeginPass(ID3D12RootSignature* rootSignature, D3D_PRIMITIVE_TOPOLOGY topology)
{
    if (!m_CommandList)
    {
        return;
    }
    {
        // 内訳の計測（フレーム最初の呼び出しだけ重い：ライトの収集と影のキャッシュ再構築）
        RenderProfiler::ScopedEvent profile("BeginPass: Light Constants", m_CommandList.Get());
        LightingResources::UpdateLightConstantBuffer(1.35f);
    }
    {
        RenderProfiler::ScopedEvent profile("BeginPass: Shadow Constants", m_CommandList.Get());
        LightingResources::UpdateShadowConstantBuffer();
    }
    // Directly indexed root signatures require the resource heap to be set first.
    ID3D12DescriptorHeap* resourceHeap = m_CbvHeap.Get();
    m_CommandList->SetDescriptorHeaps(1, &resourceHeap);
    m_CommandList->SetGraphicsRootSignature(rootSignature);
    NoiseCacheBindings::BindMain(m_CommandList.Get());
    Raytracing::BindLighting(m_CommandList.Get(), 16, 17);
    Raytracing::BindGlass(m_CommandList.Get());
    if (m_LightConstantBuffer)
    {
        m_CommandList->SetGraphicsRootConstantBufferView(2, LightingResources::GetCurrentLightConstantBufferAddress());
    }
    if (m_PBRConstantBuffer)
    {
        m_CommandList->SetGraphicsRootConstantBufferView(3, MaterialBindings::GetPBRConstantBufferAddress());
    }
    if (m_ShadowDepthBuffer)
    {
        CD3DX12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                      RendererState::g_kSHADOW_SRV_INDEX, m_CbvIncrementSize);
        m_CommandList->SetGraphicsRootDescriptorTable(4, shadowSrvHandle);
    }
    if (m_ShadowConstantBuffer)
    {
        m_CommandList->SetGraphicsRootConstantBufferView(5, LightingResources::GetCurrentShadowConstantBufferAddress());
    }
    int environmentSrvIndex = GetActiveEnvironmentTextureSrvIndex();
    if (environmentSrvIndex < 0)
    {
        environmentSrvIndex = TextureManager::GetDefaultTextureIndex();
    }
    CD3DX12_GPU_DESCRIPTOR_HANDLE environmentSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                       environmentSrvIndex, m_CbvIncrementSize);
    m_CommandList->SetGraphicsRootDescriptorTable(7, environmentSrvHandle);
    m_CommandList->SetGraphicsRootDescriptorTable(19, m_GBufferSrvHandles[static_cast<UINT>(GBufferType::BASE_COLOR)]);
    m_CommandList->SetGraphicsRootDescriptorTable(20, m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
    int monitorIndex = m_MonitorTextureSrvIndex;
    if (monitorIndex < 0)
    {
        monitorIndex = TextureManager::GetDefaultTextureIndex();
    }
    m_CommandList->SetGraphicsRootDescriptorTable(
        21, CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), monitorIndex,
                                          m_CbvIncrementSize));
    m_CommandList->SetGraphicsRootDescriptorTable(22, Raytracing::GetResultSrv());
    m_CommandList->SetGraphicsRootDescriptorTable(23, Raytracing::GetReflectionSrv());
    if (m_PostProcessConstantBuffer)
    {
        m_CommandList->SetGraphicsRootConstantBufferView(26, m_PostProcessConstantBuffer->GetGPUVirtualAddress() +
                                                                 GetPostProcessFrameOffset());
    }
    CloudRenderer::BindForwardLighting(m_CommandList.Get(), m_FrameIndex);
    m_CommandList->SetGraphicsRootDescriptorTable(28, ScreenSpaceEffects::GetAoSrv());
    m_CommandList->SetGraphicsRootDescriptorTable(
        29, (Raytracing::IsGiActive() ? Raytracing::GetGiSrv() : ScreenSpaceEffects::GetGiSrv()));
    m_CommandList->SetGraphicsRootDescriptorTable(30, m_GBufferSrvHandles[static_cast<UINT>(GBufferType::ATMOSPHERE)]);
    TransmittanceShadows::BindLighting(m_CommandList.Get(), 31, 32);
    if (m_TransparentSceneCopy)
    {
        m_CommandList->SetGraphicsRootDescriptorTable(8, m_TransparentSceneSrvHandle);
    }
    const D3D12_GPU_VIRTUAL_ADDRESS lightTileAddress = LightingResources::GetCurrentLightTileIndexBufferAddress();
    if (lightTileAddress != 0)
    {
        m_CommandList->SetGraphicsRootShaderResourceView(10, lightTileAddress);
    }
    // MegaLights M7：Forward / 半透明のグリッドとライトのテクスチャ
    MegaLights::BindForward(m_CommandList.Get(), 34, 35,
                            CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                          RendererState::g_kTEXTURE_SRV_START_INDEX,
                                                          m_CbvIncrementSize));
    m_CommandList->IASetPrimitiveTopology(topology);
}

void ScenePasses::BeginSpritePass()
{
    BeginPass(m_RootSignature.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ScenePasses::BeginModelPass()
{
    BeginPass(m_ModelRootSignature.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ScenePasses::BeginLinePass()
{
    BeginPass(m_ModelRootSignature.Get(), D3D_PRIMITIVE_TOPOLOGY_LINELIST);
}

bool ScenePasses::BeginShadowPass(UINT shadowIndex, bool preserveDepth, bool stationaryTarget)
{
    if (!m_CommandList || !m_ShadowDepthBuffer || !m_DsvHeap || shadowIndex >= LightingResources::GetShadowLightCount())
    {
        return false;
    }
    LightingResources::SetCurrentShadowPassIndex(shadowIndex);
    LightingResources::UpdateShadowConstantBuffer();
    UINT shadowLayer = 0;
    D3D12_VIEWPORT shadowViewport{};
    D3D12_RECT shadowScissor{};
    bool clearShadowLayer = true;
    if (!LightingResources::GetShadowPassInfo(shadowIndex, shadowLayer, shadowViewport, shadowScissor,
                                              clearShadowLayer))
    {
        return false;
    }
    const UINT dsvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), 1 + shadowLayer,
                                                  dsvIncrement);
    if (stationaryTarget)
    {
        if (!g_StationaryShadowDepth)
        {
            return false;
        }
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(g_StationaryShadowDepth.Get(),
                                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_CommandList->ResourceBarrier(1, &barrier);
        shadowDsvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(g_StationaryShadowDsv->GetCPUDescriptorHandleForHeapStart(),
                                                        shadowLayer, dsvIncrement);
    }
    m_CommandList->RSSetViewports(1, &shadowViewport);
    m_CommandList->RSSetScissorRects(1, &shadowScissor);
    if (!preserveDepth && clearShadowLayer)
    {
        m_CommandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
    else if (!preserveDepth)
    {

        m_CommandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &shadowScissor);
    }
    m_CommandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsvHandle);
    ID3D12DescriptorHeap* resourceHeap = m_CbvHeap.Get();
    m_CommandList->SetDescriptorHeaps(1, &resourceHeap);
    m_CommandList->SetGraphicsRootSignature(m_ModelRootSignature.Get());
    NoiseCacheBindings::BindMain(m_CommandList.Get());
    if (m_ShadowConstantBuffer)
    {
        m_CommandList->SetGraphicsRootConstantBufferView(
            5, LightingResources::GetShadowConstantBufferAddress(shadowIndex));
    }
    m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    return true;
}

void ScenePasses::EndShadowPass() {}

void ScenePasses::EndShadowPassBatch()
{
    if (!m_CommandList || !m_ShadowDepthBuffer)
    {
        return;
    }
    m_CommandList->RSSetViewports(1, &m_Viewport);
    m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
}

void ScenePasses::BeginEditorSceneOverlayPass()
{
    if (!m_CommandList || !GetRenderTarget(RenderTargetType::EditorScene) || !m_DsvHeap)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT barrierCount = 0;
    barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
        GetRenderTarget(RenderTargetType::EditorScene).Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    if (m_DepthStencilState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(m_DepthStencilBuffer.Get(), m_DepthStencilState,
                                                                        D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_DepthStencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    m_CommandList->ResourceBarrier(barrierCount, barriers);
    m_IsDeferredGeometryPass = false;
    m_IsSceneColorForwardPass = true;

    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
    if (m_UseLowResDepth)
    {
        m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

        ID3D12PipelineState* depthPso = PsoManager::GetUpscaleDepthPso();
        if (depthPso && m_UpscaleRootSignature)
        {
            m_CommandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
            m_CommandList->SetPipelineState(depthPso);
            m_CommandList->SetGraphicsRootSignature(m_UpscaleRootSignature.Get());
            RenderFrame::SetDescriptorHeap();
            m_CommandList->SetGraphicsRootDescriptorTable(0, GetRenderTargetSrvHandle(RenderTargetType::Scene));
            m_CommandList->SetGraphicsRootDescriptorTable(1,
                                                          m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
            if (m_PostProcessConstantBuffer)
            {
                m_CommandList->SetGraphicsRootConstantBufferView(
                    2, m_PostProcessConstantBuffer->GetGPUVirtualAddress() + GetPostProcessFrameOffset());
            }
            m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_CommandList->DrawInstanced(3, 1, 0, 0);
        }
    }
    m_CommandList->OMSetRenderTargets(1, &GetRenderTargetRtvHandle(RenderTargetType::EditorScene), FALSE, &dsvHandle);

    m_CommandList->RSSetViewports(1, &m_FullViewport);
    m_CommandList->RSSetScissorRects(1, &m_FullScissorRect);
}

void ScenePasses::BeginHdrTransparentPass()
{
    RenderTargets::ActivateTransparentCopy(m_CommandList.Get());
    Raytracing::BeginGlassRead(m_CommandList.Get());
    m_UseLowResDepth = m_RenderMode == RenderMode::DEFERRED && m_LowResDepthBuffer != nullptr;
    auto* depth = m_UseLowResDepth ? m_LowResDepthBuffer.Get() : m_DepthStencilBuffer.Get();
    D3D12_RESOURCE_STATES depthState =
        m_UseLowResDepth ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : m_DepthStencilState;
    auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        depth, depthState,
        static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_DEPTH_READ |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    m_CommandList->ResourceBarrier(1, &depthBarrier);
    if (!m_UseLowResDepth)
    {
        m_DepthStencilState = static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_DEPTH_READ |
                                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    UINT depthIndex = m_UseLowResDepth ? g_kLOW_DEPTH_READ_DSV_INDEX : g_kFULL_DEPTH_READ_DSV_INDEX;
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), depthIndex,
                                      m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV));
    m_IsDeferredGeometryPass = false;
    m_IsSceneColorForwardPass = true;
    // OIT はシーンの代わりに蓄積値と透過残率のターゲットをバインドし、
    // レイトレース屈折用に不透明シーンもコピーする。
    if (!TransparencyEffects::BeginTransparentPass(m_CommandList.Get(), GetRenderTarget(RenderTargetType::Scene).Get(),
                                                   GetRenderTargetRtvHandle(RenderTargetType::Scene),
                                                   m_TransparentSceneCopy.Get(), dsv))
    {
        m_CommandList->OMSetRenderTargets(1, &GetRenderTargetRtvHandle(RenderTargetType::Scene), FALSE, &dsv);
    }
    m_CommandList->RSSetViewports(1, &m_Viewport);
    m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
}

void ScenePasses::EndHdrTransparentPass()
{
    Raytracing::EndGlassRead(m_CommandList.Get());
    if (TransparencyEffects::IsOitPassActive())
    {
        TransparencyEffects::EndTransparentPass(m_CommandList.Get(), GetRenderTargetRtvHandle(RenderTargetType::Scene),
                                                m_Viewport, m_ScissorRect);
        RenderFrame::SetDescriptorHeap();
    }
    auto* depth = m_UseLowResDepth ? m_LowResDepthBuffer.Get() : m_DepthStencilBuffer.Get();
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        depth,
        static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_DEPTH_READ |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_CommandList->ResourceBarrier(1, &barrier);
    if (!m_UseLowResDepth)
    {
        m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    m_IsSceneColorForwardPass = false;
}

void ScenePasses::EndEditorSceneOverlayPass()
{
    if (!m_CommandList || !GetRenderTarget(RenderTargetType::EditorScene))
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::EditorScene).Get(),
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_DepthStencilBuffer.Get(), m_DepthStencilState,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_CommandList->ResourceBarrier(2, barriers);
    m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_IsSceneColorForwardPass = false;
}

void ScenePasses::BeginScenePass()
{
    if (!GetRenderTarget(RenderTargetType::Scene))
    {
        return;
    }
    OcclusionCulling::BeginPhaseOne();
    m_IsSceneColorForwardPass = true;
    m_UseLowResDepth = false;

    if (m_DepthStencilState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        D3D12_RESOURCE_BARRIER depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_DepthStencilBuffer.Get(), m_DepthStencilState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_CommandList->ResourceBarrier(1, &depthBarrier);
        m_DepthStencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    if (m_RenderMode == RenderMode::DEFERRED)
    {
        m_IsDeferredGeometryPass = true;
        m_UseLowResDepth = (m_LowResDepthBuffer != nullptr);
        const bool useVisibilityBuffer =
            RendererSettings::GetComputeGBufferEnabled() && VisibilityBuffer::IsAvailable();
        if (useVisibilityBuffer)
        {
            const UINT visibilityIndex = static_cast<UINT>(GBufferType::VISIBILITY);
            if (!m_GBufferTargets[visibilityIndex])
            {
                m_IsDeferredGeometryPass = false;
                return;
            }
            D3D12_RESOURCE_BARRIER visibilityToRt = CD3DX12_RESOURCE_BARRIER::Transition(
                m_GBufferTargets[visibilityIndex].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_CommandList->ResourceBarrier(1, &visibilityToRt);
            if (m_UseLowResDepth)
            {
                D3D12_RESOURCE_BARRIER depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    m_LowResDepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);
                m_CommandList->ResourceBarrier(1, &depthBarrier);
            }
            const float clearVisibility[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            m_CommandList->ClearRenderTargetView(m_GBufferRtvHandles[visibilityIndex], clearVisibility, 0, nullptr);
            CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle;
            if (m_UseLowResDepth)
            {
                dsvHandle = m_LowResDsvHandle;
            }
            else
            {
                dsvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
            }
            m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            m_CommandList->OMSetRenderTargets(1, &m_GBufferRtvHandles[visibilityIndex], TRUE, &dsvHandle);
            return;
        }

        for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
        {
            if (!m_GBufferTargets[i])
            {
                m_IsDeferredGeometryPass = false;
                return;
            }
        }

        D3D12_RESOURCE_BARRIER barriers[g_kGEOMETRY_GBUFFER_COUNT]{};
        for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
        {
            barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(m_GBufferTargets[i].Get(),
                                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        if (m_UseLowResDepth)
        {
            D3D12_RESOURCE_BARRIER depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                m_LowResDepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
            m_CommandList->ResourceBarrier(1, &depthBarrier);
        }

        m_CommandList->ResourceBarrier(g_kGEOMETRY_GBUFFER_COUNT, barriers);

        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle;
        if (m_UseLowResDepth)
        {
            dsvHandle = m_LowResDsvHandle;
        }
        else
        {
            dsvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
        }
        for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
        {
            float clearColor[4]{};
            GetGBufferClearColor(i, clearColor);
            m_CommandList->ClearRenderTargetView(m_GBufferRtvHandles[i], clearColor, 0, nullptr);
        }
        m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

        D3D12_CPU_DESCRIPTOR_HANDLE handles[g_kGEOMETRY_GBUFFER_COUNT]{};
        for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
        {
            handles[i] = m_GBufferRtvHandles[i];
        }
        m_CommandList->OMSetRenderTargets(g_kGEOMETRY_GBUFFER_COUNT, handles, TRUE, &dsvHandle);
        return;
    }

    m_IsDeferredGeometryPass = false;

    D3D12_RESOURCE_BARRIER sceneBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        GetRenderTarget(RenderTargetType::Scene).Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_CommandList->ResourceBarrier(1, &sceneBarrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_CommandList->ClearRenderTargetView(GetRenderTargetRtvHandle(RenderTargetType::Scene), m_kSceneClearColor, 0,
                                         nullptr);
    m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    m_CommandList->OMSetRenderTargets(1, &GetRenderTargetRtvHandle(RenderTargetType::Scene), TRUE, &dsvHandle);
}

bool ScenePasses::BuildOcclusionHierarchyAndBeginPhaseTwo()
{
    if (!m_CommandList || OcclusionCulling::GetPhase() != 1u)
    {
        return false;
    }
    ID3D12Resource* depth;
    if (m_UseLowResDepth)
    {
        depth = m_LowResDepthBuffer.Get();
    }
    else
    {
        depth = m_DepthStencilBuffer.Get();
    }
    if (!depth)
    {
        return false;
    }
    m_CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    auto depthToRead =
        CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_CommandList->ResourceBarrier(1, &depthToRead);
    if (!m_UseLowResDepth)
    {
        m_DepthStencilState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    OcclusionCulling::BuildCurrent(m_CommandList.Get(), depth);
    auto depthToWrite =
        CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    m_CommandList->ResourceBarrier(1, &depthToWrite);
    if (!m_UseLowResDepth)
    {
        m_DepthStencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv;
    if (m_UseLowResDepth)
    {
        dsv = m_LowResDsvHandle;
    }
    else
    {
        dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
    }
    const bool useVisibilityBuffer = RendererSettings::GetComputeGBufferEnabled() && VisibilityBuffer::IsAvailable();
    if (useVisibilityBuffer)
    {
        const UINT visibilityIndex = static_cast<UINT>(GBufferType::VISIBILITY);
        m_CommandList->OMSetRenderTargets(1, &m_GBufferRtvHandles[visibilityIndex], TRUE, &dsv);
    }
    else
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handles[g_kGEOMETRY_GBUFFER_COUNT]{};
        for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
        {
            handles[i] = m_GBufferRtvHandles[i];
        }
        m_CommandList->OMSetRenderTargets(g_kGEOMETRY_GBUFFER_COUNT, handles, TRUE, &dsv);
    }
    OcclusionCulling::BeginPhaseTwo();
    return OcclusionCulling::GetPhase() == 2u;
}

void ScenePasses::ApplySurfaceDecals()
{
    if (!RendererSettings::GetUseDBufferDecals())
    {
        return;
    }
    SurfaceDecals::SyncFromScene();
    if (SurfaceDecals::Count() == 0)
    {
        return;
    }
    SurfaceDecals::RenderParams params{};
    params.Device = m_Device.Get();
    params.List = m_CommandList.Get();
    params.EngineHeap = m_CbvHeap.Get();
    params.Width = m_SceneWidth;
    params.Height = m_SceneHeight;

    const EntityID camera = Camera::GetCameraEntity();
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX projection = XMMatrixIdentity();
    Camera::GetCameraMatrices(camera, view, projection);
    XMFLOAT4X4 invViewProjection{};
    XMStoreFloat4x4(&invViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, view * projection)));
    memcpy(params.InvViewProjection, &invViewProjection, sizeof(invViewProjection));
    XMFLOAT3 cameraPosition{0.0f, 0.0f, 5.0f};
    if (camera != g_kINVALID_ENTITY && ComponentManager::HasComponent<TransformComponent>(camera))
    {
        cameraPosition = ComponentManager::GetComponentUnchecked<TransformComponent>(camera).Position;
    }
    params.CameraPosition[0] = cameraPosition.x;
    params.CameraPosition[1] = cameraPosition.y;
    params.CameraPosition[2] = cameraPosition.z;
    params.CameraPosition[3] = 1.0f;

    const UINT increment = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto gpuHandle = [&](UINT index) {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), index, increment);
    };
    params.DepthSrv = gpuHandle(m_UseLowResDepth ? g_kLOW_RES_DEPTH_SRV_INDEX : g_kDEPTH_SRV_INDEX);
    params.GBufferNormalSrv = m_GBufferSrvHandles[static_cast<UINT>(GBufferType::NORMAL)];
    params.TextureTable = gpuHandle(g_kTEXTURE_SRV_START_INDEX);
    const GBufferType types[3] = {GBufferType::BASE_COLOR, GBufferType::NORMAL, GBufferType::MATERIAL};
    for (UINT i = 0; i < 3; ++i)
    {
        const UINT type = static_cast<UINT>(types[i]);
        params.GBufferTargets[i] = m_GBufferTargets[type].Get();
        params.GBufferRtvs[i] = m_GBufferRtvHandles[type];
        params.GBufferFormats[i] = m_kDeferredRtvFormats[type];
    }
    SurfaceDecals::Render(params);
}

void ScenePasses::EndScenePass()
{
    if (!GetRenderTarget(RenderTargetType::Scene))
    {
        return;
    }
    OcclusionCulling::EndFrame();

    if (m_RenderMode == RenderMode::DEFERRED)
    {
        const bool useVisibilityBuffer =
            RendererSettings::GetComputeGBufferEnabled() && VisibilityBuffer::IsAvailable();
        if (useVisibilityBuffer)
        {
            const UINT visibilityIndex = static_cast<UINT>(GBufferType::VISIBILITY);
            D3D12_RESOURCE_BARRIER beforeCompute[g_kGEOMETRY_GBUFFER_COUNT + 2]{};
            UINT beforeCount = 0;
            beforeCompute[beforeCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
                m_GBufferTargets[visibilityIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
            {
                beforeCompute[beforeCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
                    m_GBufferTargets[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (m_UseLowResDepth)
            {
                beforeCompute[beforeCount++] =
                    CD3DX12_RESOURCE_BARRIER::Transition(m_LowResDepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            else
            {
                beforeCompute[beforeCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
                    m_DepthStencilBuffer.Get(), m_DepthStencilState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                m_DepthStencilState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            m_CommandList->ResourceBarrier(beforeCount, beforeCompute);
            {
                RenderProfiler::ScopedEvent profile("Compute GBuffer", m_CommandList.Get());
                const UINT depthIndex = m_UseLowResDepth ? g_kLOW_RES_DEPTH_SRV_INDEX : g_kDEPTH_SRV_INDEX;
                const CD3DX12_GPU_DESCRIPTOR_HANDLE depthSrv(
                    m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), depthIndex,
                    m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
                VisibilityBuffer::GenerateGBuffer(m_CommandList.Get(), m_GBufferSrvHandles[visibilityIndex], depthSrv,
                                                  m_SceneWidth, m_SceneHeight);
            }
            D3D12_RESOURCE_BARRIER afterCompute[g_kGEOMETRY_GBUFFER_COUNT + 2]{};
            afterCompute[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_GBufferTargets[visibilityIndex].Get(),
                                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
            {
                afterCompute[i + 1] = CD3DX12_RESOURCE_BARRIER::Transition(m_GBufferTargets[i].Get(),
                                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            auto* sceneDepth = m_UseLowResDepth ? m_LowResDepthBuffer.Get() : m_DepthStencilBuffer.Get();
            afterCompute[g_kGEOMETRY_GBUFFER_COUNT + 1] = CD3DX12_RESOURCE_BARRIER::Transition(
                sceneDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_CommandList->ResourceBarrier(_countof(afterCompute), afterCompute);
            if (!m_UseLowResDepth)
            {
                m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            ApplySurfaceDecals();
            m_IsDeferredGeometryPass = false;
            m_IsSceneColorForwardPass = false;
            return;
        }

        for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
        {
            if (!m_GBufferTargets[i])
            {
                m_IsDeferredGeometryPass = false;
                return;
            }
        }

        UINT barrierCount = 0;
        D3D12_RESOURCE_BARRIER barriers[g_kGEOMETRY_GBUFFER_COUNT + 2]{};
        for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
        {
            barriers[barrierCount++] =
                CD3DX12_RESOURCE_BARRIER::Transition(m_GBufferTargets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        if (m_UseLowResDepth)
        {
            barriers[barrierCount++] =
                CD3DX12_RESOURCE_BARRIER::Transition(m_LowResDepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else
        {
            barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
                m_DepthStencilBuffer.Get(), m_DepthStencilState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        m_CommandList->ResourceBarrier(barrierCount, barriers);
        ApplySurfaceDecals();
        m_IsDeferredGeometryPass = false;
        m_IsSceneColorForwardPass = false;
        return;
    }

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(GetRenderTarget(RenderTargetType::Scene).Get(),
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_DepthStencilBuffer.Get(), m_DepthStencilState,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_CommandList->ResourceBarrier(2, barriers);
    m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_IsDeferredGeometryPass = false;
    m_IsSceneColorForwardPass = false;
}

void ScenePasses::RenderVelocityBuffer()
{
    g_VelocityDepthReadActive = false;
    PostProcessPass::UpdateCameraConstants();
    if (m_RenderMode != RenderMode::DEFERRED || !m_CommandList || !m_AaRootSignature)
    {
        return;
    }

    const UINT velocityIndex = static_cast<UINT>(GBufferType::VELOCITY);
    ID3D12PipelineState* velocityPso = PsoManager::GetVelocityPso();
    if (!velocityPso || !m_GBufferTargets[velocityIndex])
    {
        return;
    }

    D3D12_RESOURCE_BARRIER toVelocityRt = CD3DX12_RESOURCE_BARRIER::Transition(
        m_GBufferTargets[velocityIndex].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_CommandList->ResourceBarrier(1, &toVelocityRt);

    const float clearVelocity[4] = {0.5f, 0.5f, 0.0f, 0.0f};
    m_CommandList->ClearRenderTargetView(m_GBufferRtvHandles[velocityIndex], clearVelocity, 0, nullptr);
    m_CommandList->OMSetRenderTargets(1, &m_GBufferRtvHandles[velocityIndex], FALSE, nullptr);
    m_CommandList->SetPipelineState(velocityPso);
    m_CommandList->SetGraphicsRootSignature(m_AaRootSignature.Get());
    RenderFrame::SetDescriptorHeap();
    m_CommandList->RSSetViewports(1, &m_Viewport);
    m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

    if (m_PostProcessConstantBuffer)
    {
        m_CommandList->SetGraphicsRootConstantBufferView(0, m_PostProcessConstantBuffer->GetGPUVirtualAddress() +
                                                                GetPostProcessFrameOffset());
    }
    m_CommandList->SetGraphicsRootDescriptorTable(1, m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
    m_CommandList->SetGraphicsRootDescriptorTable(2, m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
    m_CommandList->SetGraphicsRootDescriptorTable(3, m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);

    XMMATRIX prevView = XMLoadFloat4x4(&m_PrevViewMatrix);
    XMMATRIX prevProj = XMLoadFloat4x4(&m_PrevProjMatrix);
    XMMATRIX prevViewProj = XMMatrixTranspose(prevView * prevProj);
    float constants[20] = {};
    memcpy(constants, &prevViewProj, sizeof(XMFLOAT4X4));
    constants[16] = 1.0f / max(static_cast<float>(m_SceneWidth), 1.0f);
    constants[17] = 1.0f / max(static_cast<float>(m_SceneHeight), 1.0f);
    m_CommandList->SetGraphicsRoot32BitConstants(4, 20, constants, 0);
    m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_CommandList->DrawInstanced(3, 1, 0, 0);
    ID3D12Resource* depth = m_UseLowResDepth ? m_LowResDepthBuffer.Get() : m_DepthStencilBuffer.Get();
    if (depth)
    {
        const auto readDepth = CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                                    D3D12_RESOURCE_STATE_DEPTH_READ);
        m_CommandList->ResourceBarrier(1, &readDepth);
        const UINT index = m_UseLowResDepth ? g_kLOW_DEPTH_READ_DSV_INDEX : g_kFULL_DEPTH_READ_DSV_INDEX;
        const auto readOnlyDsv =
            CD3DX12_CPU_DESCRIPTOR_HANDLE(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), index,
                                          m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV));
        m_CommandList->OMSetRenderTargets(1, &m_GBufferRtvHandles[velocityIndex], FALSE, &readOnlyDsv);
        g_VelocityDepthReadActive = true;
        if (!m_UseLowResDepth)
        {
            m_DepthStencilState = D3D12_RESOURCE_STATE_DEPTH_READ;
        }
    }
}

void ScenePasses::EndVelocityBuffer()
{
    if (m_RenderMode != RenderMode::DEFERRED || !m_CommandList)
    {
        return;
    }
    if (g_VelocityDepthReadActive)
    {
        m_CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        ID3D12Resource* depth = m_UseLowResDepth ? m_LowResDepthBuffer.Get() : m_DepthStencilBuffer.Get();
        const auto restoreDepth = CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_DEPTH_READ,
                                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &restoreDepth);
        if (!m_UseLowResDepth)
        {
            m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        g_VelocityDepthReadActive = false;
    }
    const UINT velocityIndex = static_cast<UINT>(GBufferType::VELOCITY);
    if (!m_CommandList || !m_GBufferTargets[velocityIndex])
    {
        return;
    }
    D3D12_RESOURCE_BARRIER toVelocitySrv =
        CD3DX12_RESOURCE_BARRIER::Transition(m_GBufferTargets[velocityIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_CommandList->ResourceBarrier(1, &toVelocitySrv);
}
