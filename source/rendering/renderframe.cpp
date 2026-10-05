#include "pch.h"
#include "renderframe.h"
#include "renderconfiguration.h"
#include "graphicsdevice.h"
#include "lightingresources.h"
#include "texturemanager.h"
#include "imguimanager.h"
#include "world.h"


void RenderFrame::BeginDraw()
{
	if (m_FrameLatencyWaitableObject)
	{
		WaitForSingleObjectEx(m_FrameLatencyWaitableObject, 1000, TRUE);
	}
	RenderConfiguration::ApplyPendingRenderMode();
	RenderConfiguration::ApplyPendingHdr();
	RenderConfiguration::ApplyPendingResolutionScale();
	LightingResources::BeginFrame();

	m_CommandAllocator[m_FrameIndex]->Reset();
	m_CommandList->Reset(m_CommandAllocator[m_FrameIndex].Get(), nullptr);
	TextureManager::UpdateStreaming(m_CommandList.Get());

	m_CommandList->RSSetViewports(1, &m_Viewport);
	m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = m_RenderTargets[m_FrameIndex].Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	m_CommandList->ResourceBarrier(1, &barrier);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), m_FrameIndex, m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());

	m_CommandList->ClearRenderTargetView(rtvHandle, m_kSceneClearColor, 0, nullptr);
	m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	m_DynamicVertexOffset = 0;

	SetDescriptorHeap();
	ImGuiManager::Update();
}

void RenderFrame::EndDraw()
{
	if (m_CommandList)
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), m_FrameIndex,
			m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
		CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
		m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
		ImGuiManager::Draw(m_CommandList.Get());
	}

	if (m_DepthStencilState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
	{
		D3D12_RESOURCE_BARRIER depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_DepthStencilBuffer.Get(),
			m_DepthStencilState,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &depthBarrier);
		m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	}

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = m_RenderTargets[m_FrameIndex].Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

	m_CommandList->ResourceBarrier(1, &barrier);

	const HRESULT closeHr = m_CommandList->Close();
	if (FAILED(closeHr))
	{
		PostMessage(m_Hwnd, WM_CLOSE, 0, 0);
		return;
	}

	ID3D12CommandList* ppCommandLists[] = { m_CommandList.Get() };
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

	m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

	if (m_Fence->GetCompletedValue() < m_FenceValues[m_FrameIndex])
	{
		m_Fence->SetEventOnCompletion(m_FenceValues[m_FrameIndex], m_FenceEvent);
		WaitForSingleObject(m_FenceEvent, INFINITE);
	}

	World::WaitForFrameLimit();
}

void RenderFrame::BeginBackBufferPass()
{
	if (!m_CommandList || !m_RtvHeap || !m_DsvHeap || !m_Device) return;
	m_IsSceneColorForwardPass = false;
	if (m_DepthStencilState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
	{
		D3D12_RESOURCE_BARRIER depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_DepthStencilBuffer.Get(),
			m_DepthStencilState,
			D3D12_RESOURCE_STATE_DEPTH_WRITE);
		m_CommandList->ResourceBarrier(1, &depthBarrier);
		m_DepthStencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	}
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), m_FrameIndex, m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
	m_CommandList->RSSetViewports(1, &m_FullViewport);
	m_CommandList->RSSetScissorRects(1, &m_FullScissorRect);
}

void RenderFrame::SetDescriptorHeap()
{
	ID3D12DescriptorHeap* heaps[] = { m_CbvHeap.Get() };
	m_CommandList->SetDescriptorHeaps(_countof(heaps), heaps);
}
