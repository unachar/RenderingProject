#include "pch.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "rendererutils.h"
#include "world.h"
#include "ecs.h"
#include "texturemanager.h"
#include "componentmanager.h"
#include "imguimanager.h"
#include "psomanager.h"
#include "camera.h"

namespace
{
	struct PostProcessConstants
	{
		XMFLOAT4 Flags{};
		XMFLOAT4 PPCameraPos{};
		XMFLOAT4 HdrFlags{};
		XMFLOAT4X4 PPInvViewProjection{};
	};

	static_assert(sizeof(PostProcessConstants) <= RendererState::g_kPP_CB_ALIGNED_SIZE);
}

void RendererDraw::ReleaseGBufferResources()
{
	for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
	{
		m_GBufferTargets[i].Reset();
		m_GBufferRtvHandles[i] = {};
		m_GBufferSrvHandles[i] = {};
	}
}

bool RendererDraw::CreateDepthBuffer()
{
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc {};
	dsvHeapDesc.NumDescriptors = 2;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	HRESULT hr = m_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_DsvHeap));
	if (FAILED(hr))
	{
		Debug::Log("ERROR: CreateDescriptorHeap(DSV) failed\n");
		return false;
	}

	D3D12_RESOURCE_DESC depthDesc {};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = m_Width;
	depthDesc.Height = m_Height;
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
		Debug::Log("ERROR: CreateCommittedResource(Depth) failed\n");
		return false;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	m_Device->CreateDepthStencilView(m_DepthStencilBuffer.Get(), &dsvDesc,
		m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	return true;
}

bool RendererDraw::CreateShadowDepthBuffer()
{
	D3D12_CLEAR_VALUE clearValue {};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil.Depth = 1.0f;
	clearValue.DepthStencil.Stencil = 0;
	auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, RendererState::g_kSHADOW_MAP_SIZE, RendererState::g_kSHADOW_MAP_SIZE, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	HRESULT hr = m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&m_ShadowDepthBuffer));
	if (FAILED(hr)) { Debug::Log("ERROR: CreateCommittedResource(ShadowDepth) failed\n"); return false; }
	if (!m_DsvHeap)
	{
		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc {};
		dsvHeapDesc.NumDescriptors = 2;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		hr = m_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_DsvHeap));
		if (FAILED(hr)) { Debug::Log("ERROR: CreateDescriptorHeap(DSV) failed\n"); return false; }
	}
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	const UINT dsvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), 1, dsvIncrement);
	m_Device->CreateDepthStencilView(m_ShadowDepthBuffer.Get(), &dsvDesc, shadowDsvHandle);
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), RendererState::g_kSHADOW_SRV_INDEX, m_CbvIncrementSize);
	m_Device->CreateShaderResourceView(m_ShadowDepthBuffer.Get(), &srvDesc, srvCpuHandle);
	auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(RendererState::g_kSHADOW_CB_ALIGNED_SIZE);
	auto cbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	hr = m_Device->CreateCommittedResource(&cbHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ShadowConstantBuffer));
	if (FAILED(hr)) { Debug::Log("ERROR: CreateCommittedResource(Shadow CB) failed\n"); return false; }
	CD3DX12_RANGE readRange(0, 0);
	m_ShadowConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pShadowCbvDataBegin));
	m_ShadowViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE), static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE));
	m_ShadowScissorRect = CD3DX12_RECT(0, 0, RendererState::g_kSHADOW_MAP_SIZE, RendererState::g_kSHADOW_MAP_SIZE);
	return true;
}
void RendererDraw::BeginDraw()
{
	RendererCore::ApplyPendingRenderMode();
	RendererCore::ApplyPendingHdr();
	RendererResource::BeginFrame();

	m_CommandAllocator[m_FrameIndex]->Reset();
	m_CommandList->Reset(m_CommandAllocator[m_FrameIndex].Get(), nullptr);

	m_CommandList->RSSetViewports(1, &m_Viewport);
	m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

	D3D12_RESOURCE_BARRIER barrier {};
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

void RendererDraw::BeginPass(ID3D12RootSignature* rootSignature, D3D_PRIMITIVE_TOPOLOGY topology)
{
	if (!m_CommandList) return;
	SetDescriptorHeap();
	RendererResource::UpdateLightConstantBuffer(1.35f);
	RendererResource::UpdateShadowConstantBuffer();
	m_CommandList->SetGraphicsRootSignature(rootSignature);
	if (m_LightConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(2, m_LightConstantBuffer->GetGPUVirtualAddress());
	if (m_PBRConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(3, m_PBRConstantBuffer->GetGPUVirtualAddress());
	if (m_ShadowDepthBuffer)
	{
		CD3DX12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), RendererState::g_kSHADOW_SRV_INDEX, m_CbvIncrementSize);
		m_CommandList->SetGraphicsRootDescriptorTable(4, shadowSrvHandle);
	}
	if (m_ShadowConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(5, m_ShadowConstantBuffer->GetGPUVirtualAddress());
	int environmentSrvIndex = m_EnvironmentTextureSrvIndex;
	if (environmentSrvIndex < 0)
	{
		environmentSrvIndex = TextureManager::GetDefaultTextureIndex();
	}
	CD3DX12_GPU_DESCRIPTOR_HANDLE environmentSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), environmentSrvIndex, m_CbvIncrementSize);
	m_CommandList->SetGraphicsRootDescriptorTable(7, environmentSrvHandle);
	m_CommandList->IASetPrimitiveTopology(topology);
}

void RendererDraw::BeginSpritePass()
{
	BeginPass(m_RootSignature.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void RendererDraw::BeginModelPass()
{
	BeginPass(m_ModelRootSignature.Get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void RendererDraw::BeginLinePass()
{
	BeginPass(m_ModelRootSignature.Get(), D3D_PRIMITIVE_TOPOLOGY_LINELIST);
}
void RendererDraw::BeginShadowPass()
{
	if (!m_CommandList || !m_ShadowDepthBuffer || !m_DsvHeap) return;
	RendererResource::UpdateShadowConstantBuffer();
	D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_ShadowDepthBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	m_CommandList->ResourceBarrier(1, &barrier);
	const UINT dsvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), 1, dsvIncrement);
	m_CommandList->RSSetViewports(1, &m_ShadowViewport);
	m_CommandList->RSSetScissorRects(1, &m_ShadowScissorRect);
	m_CommandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	m_CommandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsvHandle);
	m_CommandList->SetGraphicsRootSignature(m_ModelRootSignature.Get());
	if (m_ShadowConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(5, m_ShadowConstantBuffer->GetGPUVirtualAddress());
	m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
void RendererDraw::EndShadowPass()
{
	if (!m_CommandList || !m_ShadowDepthBuffer) return;
	D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_ShadowDepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_CommandList->ResourceBarrier(1, &barrier);
	m_CommandList->RSSetViewports(1, &m_Viewport);
	m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
}
void RendererDraw::BeginBackBufferPass()
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
}

void RendererDraw::BeginEditorSceneOverlayPass()
{
	if (!m_CommandList || !m_EditorSceneRenderTarget || !m_DsvHeap)
	{
		return;
	}

	D3D12_RESOURCE_BARRIER barriers[2]{};
	UINT barrierCount = 0;
	barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
		m_EditorSceneRenderTarget.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);

	if (m_DepthStencilState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
	{
		barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
			m_DepthStencilBuffer.Get(),
			m_DepthStencilState,
			D3D12_RESOURCE_STATE_DEPTH_WRITE);
		m_DepthStencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	}

	m_CommandList->ResourceBarrier(barrierCount, barriers);
	m_IsDeferredGeometryPass = false;
	m_IsSceneColorForwardPass = true;

	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_CommandList->OMSetRenderTargets(1, &m_EditorSceneRtvHandle, FALSE, &dsvHandle);
}

void RendererDraw::EndEditorSceneOverlayPass()
{
	if (!m_CommandList || !m_EditorSceneRenderTarget)
	{
		return;
	}

	D3D12_RESOURCE_BARRIER barriers[2]{};
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		m_EditorSceneRenderTarget.Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		m_DepthStencilBuffer.Get(),
		m_DepthStencilState,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_CommandList->ResourceBarrier(2, barriers);
	m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	m_IsSceneColorForwardPass = false;
}

void RendererDraw::BeginScenePass()
{
	if (!m_SceneRenderTarget) return;
	m_IsSceneColorForwardPass = true;

	if (m_DepthStencilState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
	{
		D3D12_RESOURCE_BARRIER depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_DepthStencilBuffer.Get(),
			m_DepthStencilState,
			D3D12_RESOURCE_STATE_DEPTH_WRITE);
		m_CommandList->ResourceBarrier(1, &depthBarrier);
		m_DepthStencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	}

	if (m_RenderMode == RenderMode::DEFERRED)
	{
		m_IsDeferredGeometryPass = true;

		for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
		{
			if (!m_GBufferTargets[i])
			{
				m_IsDeferredGeometryPass = false;
				return;
			}
		}

		D3D12_RESOURCE_BARRIER barriers[g_kGBUFFER_COUNT] {};
		for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
		{
			barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_GBufferTargets[i].Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
		m_CommandList->ResourceBarrier(g_kGBUFFER_COUNT, barriers);

		CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
		for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
		{
			const float normalClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			const float positionClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			const float depthClear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			const float blackClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			const float zeroClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			const float materialClear[4] = { 0.0f, 0.0f, 0.0f, -1.0f };
			const float* clearColor = zeroClear;
			if (i == static_cast<UINT>(GBufferType::BASE_COLOR)) clearColor = m_kSceneClearColor;
			else if (i == static_cast<UINT>(GBufferType::NORMAL)) clearColor = normalClear;
			else if (i == static_cast<UINT>(GBufferType::POSITION)) clearColor = positionClear;
			else if (i == static_cast<UINT>(GBufferType::DEPTH)) clearColor = depthClear;
			else if (i == static_cast<UINT>(GBufferType::MATERIAL)) clearColor = materialClear;
			m_CommandList->ClearRenderTargetView(m_GBufferRtvHandles[i], clearColor, 0, nullptr);
		}
		m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		D3D12_CPU_DESCRIPTOR_HANDLE handles[g_kGBUFFER_COUNT]{};
		for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
		{
			handles[i] = m_GBufferRtvHandles[i];
		}
		m_CommandList->OMSetRenderTargets(g_kGBUFFER_COUNT, handles, TRUE, &dsvHandle);
		return;
	}

	m_IsDeferredGeometryPass = false;

	D3D12_RESOURCE_BARRIER sceneBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
		m_SceneRenderTarget.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_CommandList->ResourceBarrier(1, &sceneBarrier);

	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_CommandList->ClearRenderTargetView(m_SceneRtvHandle, m_kSceneClearColor, 0, nullptr);
	m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	m_CommandList->OMSetRenderTargets(1, &m_SceneRtvHandle, TRUE, &dsvHandle);
}
void RendererDraw::EndScenePass()
{
	if (!m_SceneRenderTarget) return;

	if (m_RenderMode == RenderMode::DEFERRED)
	{
		for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
		{
			if (!m_GBufferTargets[i])
			{
				m_IsDeferredGeometryPass = false;
				return;
			}
		}

		D3D12_RESOURCE_BARRIER barriers[g_kGBUFFER_COUNT + 1] {};
		for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
		{
			barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_GBufferTargets[i].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		barriers[g_kGBUFFER_COUNT] = CD3DX12_RESOURCE_BARRIER::Transition(
			m_DepthStencilBuffer.Get(),
			m_DepthStencilState,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(g_kGBUFFER_COUNT + 1, barriers);
		m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		m_IsDeferredGeometryPass = false;
		m_IsSceneColorForwardPass = false;
		return;
	}

	D3D12_RESOURCE_BARRIER barriers[2] {};
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		m_SceneRenderTarget.Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		m_DepthStencilBuffer.Get(),
		m_DepthStencilState,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_CommandList->ResourceBarrier(2, barriers);
	m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	m_IsDeferredGeometryPass = false;
	m_IsSceneColorForwardPass = false;
}
void RendererDraw::ApplyPostProcess(const PostProcessComponent& config)
{
	auto DrawFullscreenPass = [&](ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE targetRtv,
		D3D12_GPU_DESCRIPTOR_HANDLE sourceHandle, float intensity, float renderModeFlag, float deferredLightStrength)
		{
			if (!pso || !m_PostProcessRootSignature)
			{
				return;
			}

			m_CommandList->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
			m_CommandList->SetPipelineState(pso);
			m_CommandList->SetGraphicsRootSignature(m_PostProcessRootSignature.Get());

			SetDescriptorHeap();

			if (m_pPostProcessCbvDataBegin)
			{
				XMFLOAT3 cameraPosition = { 0.0f, 0.0f, 5.0f };
				EntityID cameraEntity = Camera::GetCameraEntity();
				if (cameraEntity != g_kINVALID_ENTITY && ComponentManager::HasComponent<TransformComponent>(cameraEntity))
				{
					cameraPosition = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
				}

				XMMATRIX view = XMMatrixIdentity();
				XMMATRIX projection = XMMatrixIdentity();
				Camera::GetCameraMatrices(cameraEntity, view, projection);
				const XMMATRIX invViewProjection = XMMatrixInverse(nullptr, view * projection);

				PostProcessConstants params{};
				params.Flags = XMFLOAT4(ImGuiManager::GetExposure(), intensity, renderModeFlag, 0.0f);
				params.PPCameraPos = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
				params.HdrFlags = XMFLOAT4(ImGuiManager::IsHdrEnabled() ? 1.0f : 0.0f, ImGuiManager::IsToneMapEnabled() ? 1.0f : 0.0f, 0.0f, 0.0f);
				XMStoreFloat4x4(&params.PPInvViewProjection, XMMatrixTranspose(invViewProjection));
				memcpy(m_pPostProcessCbvDataBegin, &params, sizeof(params));
			}
			RendererResource::UpdateLightConstantBuffer(deferredLightStrength);
			RendererResource::UpdateShadowConstantBuffer();

			m_CommandList->SetGraphicsRootDescriptorTable(0, sourceHandle);
			m_CommandList->SetGraphicsRootConstantBufferView(1, m_PostProcessConstantBuffer->GetGPUVirtualAddress());
			if (m_LightConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(2, m_LightConstantBuffer->GetGPUVirtualAddress());
			int environmentSrvIndex = m_EnvironmentTextureSrvIndex;
			if (environmentSrvIndex < 0)
			{
				environmentSrvIndex = TextureManager::GetDefaultTextureIndex();
			}
			CD3DX12_GPU_DESCRIPTOR_HANDLE environmentSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), environmentSrvIndex, m_CbvIncrementSize);
			m_CommandList->SetGraphicsRootDescriptorTable(3, environmentSrvHandle);
			if (m_ShadowDepthBuffer)
			{
				CD3DX12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), RendererState::g_kSHADOW_SRV_INDEX, m_CbvIncrementSize);
				m_CommandList->SetGraphicsRootDescriptorTable(4, shadowSrvHandle);
			}
			if (m_ShadowConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(5, m_ShadowConstantBuffer->GetGPUVirtualAddress());
			if (m_PBRConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(6, m_PBRConstantBuffer->GetGPUVirtualAddress());
			m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			m_CommandList->DrawInstanced(3, 1, 0, 0);
		};

	if (!m_EditorSceneRenderTarget)
	{
		return;
	}

	if (m_RenderMode == RenderMode::DEFERRED)
	{
		ID3D12PipelineState* deferredLightingPso = PsoManager::GetDeferredLightingPso();
		if (!deferredLightingPso || !m_SceneRenderTarget)
		{
			return;
		}

		D3D12_RESOURCE_BARRIER toSceneRt = CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneRenderTarget.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_CommandList->ResourceBarrier(1, &toSceneRt);
		m_CommandList->ClearRenderTargetView(m_SceneRtvHandle, m_kSceneClearColor, 0, nullptr);

		DrawFullscreenPass(
			deferredLightingPso,
			m_SceneRtvHandle,
			m_GBufferSrvHandles[static_cast<UINT>(GBufferType::BASE_COLOR)],
			1.0f,
			1.0f,
			1.35f);

		D3D12_RESOURCE_BARRIER toSceneSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			m_SceneRenderTarget.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &toSceneSrv);
	}

	ID3D12PipelineState* postProcessPso = PsoManager::GetPostProcessPso(config.Type);
	if (!postProcessPso)
	{
		return;
	}

	D3D12_RESOURCE_BARRIER toEditorRt = CD3DX12_RESOURCE_BARRIER::Transition(
		m_EditorSceneRenderTarget.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_CommandList->ResourceBarrier(1, &toEditorRt);
	m_CommandList->ClearRenderTargetView(m_EditorSceneRtvHandle, m_kSceneClearColor, 0, nullptr);

	DrawFullscreenPass(
		postProcessPso,
		m_EditorSceneRtvHandle,
		m_SceneSrvHandle,
		config.Intensity,
		0.0f,
		0.0f);

	D3D12_RESOURCE_BARRIER toEditorSrv = CD3DX12_RESOURCE_BARRIER::Transition(
		m_EditorSceneRenderTarget.Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_CommandList->ResourceBarrier(1, &toEditorSrv);
}
void RendererDraw::SetDescriptorHeap()

{
	ID3D12DescriptorHeap* heaps[] = { m_CbvHeap.Get() };
	m_CommandList->SetDescriptorHeaps(_countof(heaps), heaps);
}

void RendererDraw::EndDraw()
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

	D3D12_RESOURCE_BARRIER barrier {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = m_RenderTargets[m_FrameIndex].Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

	m_CommandList->ResourceBarrier(1, &barrier);

	m_CommandList->Close();

	ID3D12CommandList* ppCommandLists[] = { m_CommandList.Get() };
	m_CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	const UINT syncInterval = World::IsVSyncEnabled() ? 1u : 0u;
	m_SwapChain->Present(syncInterval, 0);

	m_CurrentFenceValue++;
	m_FenceValues[m_FrameIndex] = m_CurrentFenceValue;
	m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFenceValue);

	m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

	if (m_Fence->GetCompletedValue() < m_FenceValues[m_FrameIndex])
	{
		m_Fence->SetEventOnCompletion(m_FenceValues[m_FrameIndex], m_FenceEvent);
		WaitForSingleObject(m_FenceEvent, INFINITE);
	}

	World::WaitForFrameLimit();
}

bool RendererDraw::CreateSceneRenderTarget()
{
	if (m_SceneWidth == 0 || m_SceneHeight == 0) return true;

    ReleaseGBufferResources();
	m_SceneRenderTarget.Reset();
	m_EditorSceneRenderTarget.Reset();
	m_SceneRtvHeap.Reset();

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		m_SceneColorFormat, m_SceneWidth, m_SceneHeight, 1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

	D3D12_CLEAR_VALUE clearValue {};
	clearValue.Format = m_SceneColorFormat;
	clearValue.Color[0] = m_kSceneClearColor[0];
	clearValue.Color[1] = m_kSceneClearColor[1];
	clearValue.Color[2] = m_kSceneClearColor[2];
	clearValue.Color[3] = m_kSceneClearColor[3];

	HRESULT hr = m_Device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&m_SceneRenderTarget));
	if (FAILED(hr)) return false;

	hr = m_Device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&m_EditorSceneRenderTarget));
	if (FAILED(hr)) return false;

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc {};
	rtvHeapDesc.NumDescriptors = (m_RenderMode == RenderMode::DEFERRED) ? (2 + g_kGBUFFER_COUNT) : 2;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hr = m_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_SceneRtvHeap));
	if (FAILED(hr)) return false;
	m_SceneRtvHandle = m_SceneRtvHeap->GetCPUDescriptorHandleForHeapStart();
	m_Device->CreateRenderTargetView(m_SceneRenderTarget.Get(), nullptr, m_SceneRtvHandle);

	UINT cbvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	UINT rtvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_EditorSceneRtvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_SceneRtvHandle, 1, rtvIncrement);
	m_Device->CreateRenderTargetView(m_EditorSceneRenderTarget.Get(), nullptr, m_EditorSceneRtvHandle);

	m_SceneSrvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), RendererState::g_kSCENE_SRV_INDEX, cbvIncrement);
	CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), RendererState::g_kSCENE_SRV_INDEX, cbvIncrement);
	m_EditorSceneSrvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), RendererState::g_kEDITOR_SCENE_SRV_INDEX, cbvIncrement);
	CD3DX12_CPU_DESCRIPTOR_HANDLE editorSceneSrvCpuHandle(m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), RendererState::g_kEDITOR_SCENE_SRV_INDEX, cbvIncrement);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = m_SceneColorFormat;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	m_Device->CreateShaderResourceView(m_SceneRenderTarget.Get(), &srvDesc, srvCpuHandle);
	m_Device->CreateShaderResourceView(m_EditorSceneRenderTarget.Get(), &srvDesc, editorSceneSrvCpuHandle);

	if (m_EnvironmentTextureSrvIndex < 0)
	{
		m_EnvironmentTextureSrvIndex = TextureManager::LoadTexture("asset\\model\\sky\\charolettenbrunn_park_2k.DDS");
	}

	if (m_RenderMode == RenderMode::DEFERRED)
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE gbufferRtvHandle(m_SceneRtvHandle, 2, rtvIncrement);
		for (UINT i = 0; i < g_kGBUFFER_COUNT; ++i)
		{
			auto gbufferDesc = CD3DX12_RESOURCE_DESC::Tex2D(
				m_kDeferredRtvFormats[i], m_SceneWidth, m_SceneHeight, 1, 1, 1, 0,
				D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

			D3D12_CLEAR_VALUE gbufferClear {};
			gbufferClear.Format = m_kDeferredRtvFormats[i];
			if (i == static_cast<UINT>(GBufferType::BASE_COLOR))
			{
				gbufferClear.Color[0] = m_kSceneClearColor[0];
				gbufferClear.Color[1] = m_kSceneClearColor[1];
				gbufferClear.Color[2] = m_kSceneClearColor[2];
				gbufferClear.Color[3] = m_kSceneClearColor[3];
			}
			else if (i == static_cast<UINT>(GBufferType::NORMAL))
			{
				gbufferClear.Color[0] = 0.0f;
				gbufferClear.Color[1] = 0.0f;
				gbufferClear.Color[2] = 0.0f;
				gbufferClear.Color[3] = 1.0f;
			}
			else if (i == static_cast<UINT>(GBufferType::POSITION))
			{
				gbufferClear.Color[0] = 0.0f;
				gbufferClear.Color[1] = 0.0f;
				gbufferClear.Color[2] = 0.0f;
				gbufferClear.Color[3] = 0.0f;
			}
			else if (i == static_cast<UINT>(GBufferType::DEPTH))
			{
				gbufferClear.Color[0] = 1.0f;
				gbufferClear.Color[1] = 1.0f;
				gbufferClear.Color[2] = 1.0f;
				gbufferClear.Color[3] = 1.0f;
			}
			else if (i == static_cast<UINT>(GBufferType::MATERIAL))
			{
				gbufferClear.Color[0] = 0.0f;
				gbufferClear.Color[1] = 0.0f;
				gbufferClear.Color[2] = 0.0f;
				gbufferClear.Color[3] = -1.0f;
			}
			else if (i == static_cast<UINT>(GBufferType::SHADOW))
			{
				gbufferClear.Color[0] = 0.0f;
				gbufferClear.Color[1] = 0.0f;
				gbufferClear.Color[2] = 0.0f;
				gbufferClear.Color[3] = 0.0f;
			}
			else if (i == static_cast<UINT>(GBufferType::RIM_STYLE))
			{
				gbufferClear.Color[0] = 0.0f;
				gbufferClear.Color[1] = 0.0f;
				gbufferClear.Color[2] = 0.0f;
				gbufferClear.Color[3] = 0.0f;
			}
			else if (i == static_cast<UINT>(GBufferType::RIM_LIGHT))
			{
				gbufferClear.Color[0] = 0.0f;
				gbufferClear.Color[1] = 0.0f;
				gbufferClear.Color[2] = 0.0f;
				gbufferClear.Color[3] = 0.0f;
			}

			hr = m_Device->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&gbufferDesc,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				&gbufferClear,
				IID_PPV_ARGS(&m_GBufferTargets[i]));
			if (FAILED(hr)) return false;

			m_GBufferTargets[i]->SetName(g_GBufferTargetNames[i]);

			D3D12_RENDER_TARGET_VIEW_DESC gbufferRtvDesc{};
			gbufferRtvDesc.Format = m_kDeferredRtvFormats[i];
			gbufferRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			m_Device->CreateRenderTargetView(m_GBufferTargets[i].Get(), &gbufferRtvDesc, gbufferRtvHandle);
			m_GBufferRtvHandles[i] = gbufferRtvHandle;

			const UINT gbufferSrvIndex = RendererState::g_kGBUFFER_SRV_START_INDEX + i;
			CD3DX12_CPU_DESCRIPTOR_HANDLE gbufferSrvCpuHandle(m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), gbufferSrvIndex, cbvIncrement);
			m_GBufferSrvHandles[i] = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), gbufferSrvIndex, cbvIncrement);

			D3D12_SHADER_RESOURCE_VIEW_DESC gbufferSrvDesc {};
			gbufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			gbufferSrvDesc.Format = m_kDeferredRtvFormats[i];
			gbufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			gbufferSrvDesc.Texture2D.MipLevels = 1;
			m_Device->CreateShaderResourceView(m_GBufferTargets[i].Get(), &gbufferSrvDesc, gbufferSrvCpuHandle);

			gbufferRtvHandle.Offset(1, rtvIncrement);
		}
	}

	return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE RendererDraw::GetImGuiCpuHandle()
{
	UINT cbvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), RendererState::g_kIMGUI_SRV_INDEX, cbvIncrement);
}

D3D12_GPU_DESCRIPTOR_HANDLE RendererDraw::GetImGuiGpuHandle()
{
	UINT cbvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), RendererState::g_kIMGUI_SRV_INDEX, cbvIncrement);
}

void RendererDraw::ResizeScene(UINT width, UINT height)
{
	m_SceneWidth = width;
	m_SceneHeight = height;
	CreateSceneRenderTarget();
}








