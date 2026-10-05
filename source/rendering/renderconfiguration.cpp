#include "pch.h"
#include "renderconfiguration.h"
#include "graphicslog.h"
#include "graphicsdevice.h"
#include "rendertargets.h"
#include "componentmanager.h"
#include "world.h"
#include "psomanager.h"



void RenderConfiguration::Resize(UINT width, UINT height)
{
	if (!m_SwapChain || !m_Device)
	{
		return;
	}
	if (width == 0 || height == 0)
	{
		return;
	}
	if (width == m_Width && height == m_Height)
	{
		return;
	}
	if (!GraphicsDevice::CheckDeviceHealth(S_OK, "Resize begin"))
	{
		PostMessage(m_Hwnd, WM_CLOSE, 0, 0);
		return;
	}

	if (m_CommandQueue && m_Fence && m_FenceEvent)
	{
		m_CurrentFenceValue++;
		const HRESULT signalHr = m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);
		if (!GraphicsDevice::CheckDeviceHealth(signalHr, "Resize fence signal"))
		{
			PostMessage(m_Hwnd, WM_CLOSE, 0, 0);
			return;
		}
		if (m_Fence->GetCompletedValue() < m_CurrentFenceValue)
		{
			m_Fence->SetEventOnCompletion(m_CurrentFenceValue, m_FenceEvent);
			WaitForSingleObject(m_FenceEvent, INFINITE);
		}

		for (UINT i = 0; i < g_kFRAME_COUNT; ++i)
		{
			m_FenceValues[i] = m_CurrentFenceValue;
		}
	}

	for (UINT n = 0; n < g_kFRAME_COUNT; n++)
	{
		m_RenderTargets[n].Reset();
	}

	m_DepthStencilBuffer.Reset();

	UINT tearingFlag;
	if (m_AllowTearing)
	{
		tearingFlag = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	}
	else
	{
		tearingFlag = 0;
	}
	HRESULT hr = m_SwapChain->ResizeBuffers(
		g_kFRAME_COUNT, width, height,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
		(tearingFlag));
	if (FAILED(hr))
	{
		WriteGraphicsLog("ERROR: ResizeBuffers failed\n");
		GraphicsDevice::CheckDeviceHealth(hr, "ResizeBuffers");
		return;
	}

	m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

	UINT rtvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT n = 0; n < g_kFRAME_COUNT; n++)
	{
		m_SwapChain->GetBuffer(n, IID_PPV_ARGS(&m_RenderTargets[n]));
		m_Device->CreateRenderTargetView(m_RenderTargets[n].Get(), nullptr, rtvHandle);
		rtvHandle.Offset(1, rtvSize);
	}

	UINT sceneWidth = max((UINT)roundf(width * m_ResolutionScale), 1u);
	UINT sceneHeight = max((UINT)roundf(height * m_ResolutionScale), 1u);
	D3D12_RESOURCE_DESC depthDesc {};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = width;
	depthDesc.Height = height;
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clearValue {};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil.Depth = 1.0f;
	clearValue.DepthStencil.Stencil = 0;

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	hr = m_Device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&depthDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(&m_DepthStencilBuffer));
	if (FAILED(hr))
	{
		WriteGraphicsLog("ERROR: Resize CreateCommittedResource(Depth) failed\n");
		return;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	m_Device->CreateDepthStencilView(m_DepthStencilBuffer.Get(), &dsvDesc,
		m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	m_Width = width;
	m_Height = height;
	m_SceneWidth = sceneWidth;
	m_SceneHeight = sceneHeight;

	m_Viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)m_SceneWidth, (float)m_SceneHeight);
	m_ScissorRect = CD3DX12_RECT(0, 0, m_SceneWidth, m_SceneHeight);
	m_FullViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)m_Width, (float)m_Height);
	m_FullScissorRect = CD3DX12_RECT(0, 0, m_Width, m_Height);

	RenderTargets::CreateSceneRenderTarget();
}

void RenderConfiguration::ApplyPendingRenderMode()
{
	m_PendingRenderMode = RenderMode::DEFERRED;
	if (!m_HasPendingRenderMode || m_RenderMode == m_PendingRenderMode)
	{
		m_HasPendingRenderMode = false;
		m_RenderMode = RenderMode::DEFERRED;
		return;
	}

	if (m_CommandQueue && m_Fence && m_FenceEvent)
	{
		m_CurrentFenceValue++;
		m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);
		if (m_Fence->GetCompletedValue() < m_CurrentFenceValue)
		{
			m_Fence->SetEventOnCompletion(m_CurrentFenceValue, m_FenceEvent);
			WaitForSingleObject(m_FenceEvent, INFINITE);
		}

		for (UINT i = 0; i < g_kFRAME_COUNT; ++i)
		{
			m_FenceValues[i] = m_CurrentFenceValue;
		}
	}

	m_RenderMode = m_PendingRenderMode;
	m_HasPendingRenderMode = false;

	for (EntityID entity : World::GetView<ShaderComponent>())
	{
		ComponentManager::GetComponent<ShaderComponent>(entity).Pso.Reset();
	}
	m_PsoCache.clear();

	if (m_Device && m_SceneWidth > 0 && m_SceneHeight > 0)
	{
		RenderTargets::CreateSceneRenderTarget();
	}
}

void RenderConfiguration::SetRenderMode(RenderMode mode)
{
	(void)mode;
	if (GetRequestedRenderMode() == RenderMode::DEFERRED)
	{
		return;
	}

	m_PendingRenderMode = RenderMode::DEFERRED;
	m_HasPendingRenderMode = true;
}

void RenderConfiguration::InvalidateScenePipelineCache()
{
	m_PsoCache.clear();
	for (EntityID entity : World::GetView<ShaderComponent>())
	{
		ComponentManager::GetComponent<ShaderComponent>(entity).Pso.Reset();
	}
}

void RenderConfiguration::SetHdr(bool enabled)
{
	DXGI_FORMAT targetFormat;
	if (enabled)
	{
		targetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
	else
	{
		targetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	}
	if (m_SceneColorFormat == targetFormat)
	{
		return;
	}
	m_PendingHdr = enabled;
	m_HasPendingHdr = true;
}

void RenderConfiguration::SetResolutionScale(float scale)
{
	const float clampedScale = clamp(scale, 0.25f, 1.0f);
	if (fabsf(GetResolutionScale() - clampedScale) < 0.0001f)
	{
		return;
	}
	m_PendingResolutionScale = clampedScale;
	m_HasPendingResolutionScale = true;
}

void RenderConfiguration::ApplyPendingResolutionScale()
{
	if (!m_HasPendingResolutionScale)
	{
		return;
	}

	const UINT sceneWidth = max((UINT)roundf(m_Width * m_PendingResolutionScale), 1u);
	const UINT sceneHeight = max((UINT)roundf(m_Height * m_PendingResolutionScale), 1u);
	if (sceneWidth == m_SceneWidth && sceneHeight == m_SceneHeight)
	{
		m_ResolutionScale = m_PendingResolutionScale;
		m_HasPendingResolutionScale = false;
		return;
	}

	if (m_CommandQueue && m_Fence && m_FenceEvent)
	{
		m_CurrentFenceValue++;
		m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);
		if (m_Fence->GetCompletedValue() < m_CurrentFenceValue)
		{
			m_Fence->SetEventOnCompletion(m_CurrentFenceValue, m_FenceEvent);
			WaitForSingleObject(m_FenceEvent, INFINITE);
		}
		for (UINT i = 0; i < g_kFRAME_COUNT; ++i)
		{
			m_FenceValues[i] = m_CurrentFenceValue;
		}
	}

	m_ResolutionScale = m_PendingResolutionScale;
	m_HasPendingResolutionScale = false;
	m_SceneWidth = sceneWidth;
	m_SceneHeight = sceneHeight;
	m_Viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)m_SceneWidth, (float)m_SceneHeight);
	m_ScissorRect = CD3DX12_RECT(0, 0, m_SceneWidth, m_SceneHeight);
	RenderTargets::CreateSceneRenderTarget();
}

void RenderConfiguration::ApplyPendingHdr()
{
	DXGI_FORMAT requestedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (m_PendingHdr)
	{
		requestedFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
	if (!m_HasPendingHdr || m_SceneColorFormat == requestedFormat)
	{
		m_HasPendingHdr = false;
		return;
	}

	if (m_CommandQueue && m_Fence && m_FenceEvent)
	{
		m_CurrentFenceValue++;
		m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);
		if (m_Fence->GetCompletedValue() < m_CurrentFenceValue)
		{
			m_Fence->SetEventOnCompletion(m_CurrentFenceValue, m_FenceEvent);
			WaitForSingleObject(m_FenceEvent, INFINITE);
		}

		for (UINT i = 0; i < g_kFRAME_COUNT; ++i)
		{
			m_FenceValues[i] = m_CurrentFenceValue;
		}
	}

	if (m_PendingHdr)
	{
		m_SceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
	else
	{
		m_SceneColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	}
	m_HasPendingHdr = false;

	m_PsoCache.clear();
	m_PostProcessPsoMap.clear();
	m_DeferredLightingPso.Reset();
	m_AtmospherePso.Reset();

	for (EntityID entity : World::GetView<ShaderComponent>())
	{
		ComponentManager::GetComponent<ShaderComponent>(entity).Pso.Reset();
	}

	if (m_Device && m_SceneWidth > 0 && m_SceneHeight > 0)
	{
		RenderTargets::CreateSceneRenderTarget();
	}

	PsoManager::CreatePostProcessPipelines();
	PsoManager::CreateAtmospherePso();

	m_UpscaleBilateralPso.Reset();
	PsoManager::CreateUpscalePso();
	m_UpscaleDepthPso.Reset();
	PsoManager::CreateUpscaleDepthPso();
	m_VelocityPso.Reset();
	PsoManager::CreateVelocityPso();
	m_VelocityGeometryPso.Reset();
	PsoManager::CreateVelocityGeometryPso();

	m_FxaaPso.Reset();
	m_TaaBlendPso.Reset();
	PsoManager::CreateAaPsos();
}
