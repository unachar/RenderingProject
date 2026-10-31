#include "pch.h"
#include "materialpreview.h"
#include "animationpreviewwindow.h"
#include "modelmanager.h"
#include "assetthumbnailmanager.h"
#include "renderframe.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "renderconfiguration.h"
#include "graphicsdevice.h"
#include "lightingresources.h"
#include "texturemanager.h"
#include "imguimanager.h"
#include "world.h"
#include "atmospherelut.h"
#include "temporalhistorycontext.h"
#include "renderprofiler.h"
#include "gpumemorybudget.h"
#include "memoryallocator.h"
#include "advancedeffects.h"
#include "rayreflections.h"
#include "rendertargets.h"
#include "noisecachebindings.h"
#include "reflectionprobes.h"
#include "raytracing.h"
#include "screenspacereflections.h"

void RenderFrame::BeginDraw()
{
    TemporalHistory::Context::BeginFrame(m_Width, m_Height);
    if (m_FrameLatencyWaitableObject)
    {
        WaitForSingleObjectEx(m_FrameLatencyWaitableObject, 1000, TRUE);
    }
    GpuMemoryBudget::Update();
    MemoryAllocator::LogStatisticsPeriodically();
    ModelManager::CollectRetiredAnimModels();
    RenderTargets::UpdateMemoryPolicy();
    Raytracing::ReleaseInactiveResources();
    RayReflections::ReleaseInactiveResources();
    AdvancedEffects::ReleaseInactiveResources();

    m_CommandAllocator[m_FrameIndex]->Reset();
    m_CommandList->Reset(m_CommandAllocator[m_FrameIndex].Get(), nullptr);
    RenderProfiler::BeginFrame(m_Device.Get(), m_CommandQueue.Get(), m_CommandList.Get(), m_FrameIndex);
    TextureManager::UpdateStreaming(m_CommandList.Get());
    // After the command list reset: probe sync may record prefilter
    // dispatches into it (they would be wiped by an earlier call).
    ReflectionProbes::UpdateActiveEnvironment(m_CommandList.Get());
    ScreenSpaceReflections::UpdateFrameSelection(Raytracing::AreReflectionsActive(),
                                                 m_RenderMode == RenderMode::DEFERRED);
    NoiseCacheBindings::BeginFrame(m_CommandList.Get());

    m_CommandList->RSSetViewports(1, &m_Viewport);
    m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_RenderTargets[m_FrameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_CommandList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), m_FrameIndex,
                                            m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());

    m_CommandList->ClearRenderTargetView(rtvHandle, m_kSceneClearColor, 0, nullptr);
    m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    m_DynamicVertexOffset = 0;

    SetDescriptorHeap();
    static uint64_t editorFrame = 0;
    AssetThumbnailManager::BeginFrame(++editorFrame);
    ImGuiManager::Update();

    // UI でシーンを読み替えるとライトの EntityID も変わるため、更新後のシーンからキャッシュを作る。
    LightingResources::BeginFrame();
    // このフレームの空 LUT は直ちに非同期コンピュートキューへ記録・送信し、前フレームの
    // グラフィックス処理と重ねる。グラフィックスキューは GPU 上で完了を待つ。
    AtmosphereLut::Update(m_CommandList.Get(), m_CommandQueue.Get(), m_FrameIndex);
}

void RenderFrame::EndDraw()
{
    AnimationPreviewWindow::Render();
    MaterialPreview::Render();
    // Stage 2J live probe capture: at most one probe per frame,
    // rendered offscreen here (after the main passes) like the
    // material preview above; the frame state it touches
    // (viewport/scissor, render targets) is restored below.
    ReflectionProbes::ProcessPendingCaptures(m_CommandList.Get());
    AssetThumbnailManager::ProcessGenerationBudget();
    {
        RenderProfiler::ScopedEvent profile("Procedural Color Tile Update", m_CommandList.Get());
        NoiseCacheBindings::FinishFrame(m_CommandList.Get());
    }
    m_CommandList->RSSetViewports(1, &m_Viewport);
    m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
    if (m_CommandList)
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
            m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), m_FrameIndex,
            m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
        m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        ImGuiManager::Draw(m_CommandList.Get());
    }

    if (m_DepthStencilState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_DepthStencilBuffer.Get(), m_DepthStencilState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_CommandList->ResourceBarrier(1, &depthBarrier);
        m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_RenderTargets[m_FrameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    m_CommandList->ResourceBarrier(1, &barrier);

    RenderProfiler::EndFrame(m_CommandList.Get());
    const HRESULT closeHr = m_CommandList->Close();
    if (!GraphicsDevice::CheckDeviceHealth(closeHr, "Command list close"))
    {
        PostMessage(m_Hwnd, WM_CLOSE, 0, 0);
        return;
    }

    ID3D12CommandList* ppCommandLists[] = {m_CommandList.Get()};
    m_CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    const UINT syncInterval = static_cast<UINT>(World::IsVSyncEnabled());
    UINT presentFlags;
    if ((syncInterval == 0 && m_AllowTearing))
    {
        presentFlags = DXGI_PRESENT_ALLOW_TEARING;
    }
    else
    {
        presentFlags = 0u;
    }
    const HRESULT presentHr = m_SwapChain->Present(syncInterval, presentFlags);
    if (!GraphicsDevice::CheckDeviceHealth(presentHr, "Present"))
    {
        PostMessage(m_Hwnd, WM_CLOSE, 0, 0);
        return;
    }

    m_CurrentFenceValue++;
    m_FenceValues[m_FrameIndex] = m_CurrentFenceValue;
    const HRESULT signalHr = m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);
    if (!GraphicsDevice::CheckDeviceHealth(signalHr, "Frame fence signal"))
    {
        PostMessage(m_Hwnd, WM_CLOSE, 0, 0);
        return;
    }

    TextureManager::OnFrameSubmitted(m_CurrentFenceValue);
    GPUDrivenRenderer::EndFrame();
    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

    if (m_Fence->GetCompletedValue() < m_FenceValues[m_FrameIndex])
    {
        m_Fence->SetEventOnCompletion(m_FenceValues[m_FrameIndex], m_FenceEvent);
        WaitForSingleObject(m_FenceEvent, INFINITE);
    }

    World::WaitForFrameLimit();
}

void RenderFrame::SetDescriptorHeap()
{
    ID3D12DescriptorHeap* heaps[] = {m_CbvHeap.Get()};
    m_CommandList->SetDescriptorHeaps(_countof(heaps), heaps);
}
