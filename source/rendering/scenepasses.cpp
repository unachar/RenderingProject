#include "pch.h"
#include "scenepasses.h"
#include "renderframe.h"
#include "lightingresources.h"
#include "materialbindings.h"
#include "psomanager.h"
#include "texturemanager.h"
#include "renderersettings.h"
#include "visibilitybuffer.h"
#include "occlusionculling.h"


void ScenePasses::BeginPass(ID3D12RootSignature* rootSignature, D3D_PRIMITIVE_TOPOLOGY topology)
{
	if (!m_CommandList) return;
	LightingResources::UpdateLightConstantBuffer(1.35f);
	LightingResources::UpdateShadowConstantBuffer();
	m_CommandList->SetGraphicsRootSignature(rootSignature);
	if (m_LightConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(2, LightingResources::GetCurrentLightConstantBufferAddress());
	if (m_PBRConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(3, MaterialBindings::GetPBRConstantBufferAddress());
	if (m_ShadowDepthBuffer)
	{
		CD3DX12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), RendererState::g_kSHADOW_SRV_INDEX, m_CbvIncrementSize);
		m_CommandList->SetGraphicsRootDescriptorTable(4, shadowSrvHandle);
	}
	if (m_ShadowConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(5, LightingResources::GetCurrentShadowConstantBufferAddress());
	int environmentSrvIndex = m_EnvironmentTextureSrvIndex;
	if (environmentSrvIndex < 0)
	{
		environmentSrvIndex = TextureManager::GetDefaultTextureIndex();
	}
	CD3DX12_GPU_DESCRIPTOR_HANDLE environmentSrvHandle(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), environmentSrvIndex, m_CbvIncrementSize);
	m_CommandList->SetGraphicsRootDescriptorTable(7, environmentSrvHandle);
	if (m_TransparentSceneCopy)
	{
		m_CommandList->SetGraphicsRootDescriptorTable(8, m_TransparentSceneSrvHandle);
	}
	const D3D12_GPU_VIRTUAL_ADDRESS lightTileAddress =
		LightingResources::GetCurrentLightTileIndexBufferAddress();
	if (lightTileAddress != 0)
	{
		m_CommandList->SetGraphicsRootShaderResourceView(10, lightTileAddress);
	}
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

bool ScenePasses::BeginShadowPass(UINT shadowIndex)
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
	if (!LightingResources::GetShadowPassInfo(shadowIndex, shadowLayer, shadowViewport, shadowScissor, clearShadowLayer))
	{
		return false;
	}
	const UINT dsvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart(), 1 + shadowLayer, dsvIncrement);
	m_CommandList->RSSetViewports(1, &shadowViewport);
	m_CommandList->RSSetScissorRects(1, &shadowScissor);
	if (clearShadowLayer)
	{
		m_CommandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	}
	else
	{

		m_CommandList->ClearDepthStencilView(
			shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &shadowScissor);
	}
	m_CommandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsvHandle);
	m_CommandList->SetGraphicsRootSignature(m_ModelRootSignature.Get());
	if (m_ShadowConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(5, LightingResources::GetShadowConstantBufferAddress(shadowIndex));
	m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	return true;
}

void ScenePasses::EndShadowPass()
{}

void ScenePasses::EndShadowPassBatch()
{
	if (!m_CommandList || !m_ShadowDepthBuffer) return;
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
		GetRenderTarget(RenderTargetType::EditorScene).Get(),
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
	if (m_UseLowResDepth)
	{
		m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);


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
				m_CommandList->SetGraphicsRootConstantBufferView(2,
					m_PostProcessConstantBuffer->GetGPUVirtualAddress() +
					m_FrameIndex * g_kPP_CB_ALIGNED_SIZE);
			}
			m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			m_CommandList->DrawInstanced(3, 1, 0, 0);
		}
	}
	m_CommandList->OMSetRenderTargets(1, &GetRenderTargetRtvHandle(RenderTargetType::EditorScene), FALSE, &dsvHandle);

	m_CommandList->RSSetViewports(1, &m_FullViewport);
	m_CommandList->RSSetScissorRects(1, &m_FullScissorRect);
}

void ScenePasses::PrepareTransparentSceneCopy()
{
	if (!m_CommandList || !GetRenderTarget(RenderTargetType::EditorScene) || !m_TransparentSceneCopy)
	{
		return;
	}

	D3D12_RESOURCE_BARRIER toCopy[2]{};
	toCopy[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		GetRenderTarget(RenderTargetType::EditorScene).Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	toCopy[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		m_TransparentSceneCopy.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COPY_DEST);
	m_CommandList->ResourceBarrier(_countof(toCopy), toCopy);
	m_CommandList->CopyResource(m_TransparentSceneCopy.Get(), GetRenderTarget(RenderTargetType::EditorScene).Get());

	D3D12_RESOURCE_BARRIER toShader[2]{};
	toShader[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		GetRenderTarget(RenderTargetType::EditorScene).Get(),
		D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	toShader[1] = CD3DX12_RESOURCE_BARRIER::Transition(
		m_TransparentSceneCopy.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_CommandList->ResourceBarrier(_countof(toShader), toShader);
}

void ScenePasses::EndEditorSceneOverlayPass()
{
	if (!m_CommandList || !GetRenderTarget(RenderTargetType::EditorScene))
	{
		return;
	}

	D3D12_RESOURCE_BARRIER barriers[2]{};
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		GetRenderTarget(RenderTargetType::EditorScene).Get(),
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

void ScenePasses::BeginScenePass()
{
	if (!GetRenderTarget(RenderTargetType::Scene)) return;
	OcclusionCulling::BeginPhaseOne();
	m_IsSceneColorForwardPass = true;
	m_UseLowResDepth = false;

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
		m_UseLowResDepth = (m_LowResDepthBuffer != nullptr);
		const bool useVisibilityBuffer =
			RendererSettings::GetComputeGBufferEnabled() &&
			VisibilityBuffer::IsAvailable();
		if (useVisibilityBuffer)
		{
			const UINT visibilityIndex = static_cast<UINT>(GBufferType::VISIBILITY);
			if (!m_GBufferTargets[visibilityIndex])
			{
				m_IsDeferredGeometryPass = false;
				return;
			}
			D3D12_RESOURCE_BARRIER visibilityToRt = CD3DX12_RESOURCE_BARRIER::Transition(
				m_GBufferTargets[visibilityIndex].Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
			m_CommandList->ResourceBarrier(1, &visibilityToRt);
			if (m_UseLowResDepth)
			{
				D3D12_RESOURCE_BARRIER depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
					m_LowResDepthBuffer.Get(),
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_DEPTH_WRITE);
				m_CommandList->ResourceBarrier(1, &depthBarrier);
			}
			const float clearVisibility[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			m_CommandList->ClearRenderTargetView(
				m_GBufferRtvHandles[visibilityIndex],
				clearVisibility,
				0,
				nullptr);
			CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle;
			if (m_UseLowResDepth)
			{
				dsvHandle = m_LowResDsvHandle;
			}
			else
			{
				dsvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
			}
			m_CommandList->ClearDepthStencilView(
				dsvHandle,
				D3D12_CLEAR_FLAG_DEPTH,
				1.0f,
				0,
				0,
				nullptr);
			m_CommandList->OMSetRenderTargets(
				1,
				&m_GBufferRtvHandles[visibilityIndex],
				TRUE,
				&dsvHandle);
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
			barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_GBufferTargets[i].Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
		}

		if (m_UseLowResDepth)
		{
			D3D12_RESOURCE_BARRIER depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
				m_LowResDepthBuffer.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
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
		m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

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
		GetRenderTarget(RenderTargetType::Scene).Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_CommandList->ResourceBarrier(1, &sceneBarrier);

	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_CommandList->ClearRenderTargetView(GetRenderTargetRtvHandle(RenderTargetType::Scene), m_kSceneClearColor, 0, nullptr);
	m_CommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	m_CommandList->OMSetRenderTargets(1, &GetRenderTargetRtvHandle(RenderTargetType::Scene), TRUE, &dsvHandle);
}

bool ScenePasses::BuildOcclusionHierarchyAndBeginPhaseTwo()
{
	if (!m_CommandList || OcclusionCulling::GetPhase() != 1u) return false;
	ID3D12Resource* depth;
	if (m_UseLowResDepth)
	{
		depth = m_LowResDepthBuffer.Get();
	}
	else
	{
		depth = m_DepthStencilBuffer.Get();
	}
	if (!depth) return false;
	m_CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
	auto depthToRead = CD3DX12_RESOURCE_BARRIER::Transition(
		depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	m_CommandList->ResourceBarrier(1, &depthToRead);
	if (!m_UseLowResDepth) m_DepthStencilState = D3D12_RESOURCE_STATE_COPY_SOURCE;
	OcclusionCulling::BuildCurrent(m_CommandList.Get(), depth);
	auto depthToWrite = CD3DX12_RESOURCE_BARRIER::Transition(
		depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	m_CommandList->ResourceBarrier(1, &depthToWrite);
	if (!m_UseLowResDepth) m_DepthStencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

	CD3DX12_CPU_DESCRIPTOR_HANDLE dsv;
	if (m_UseLowResDepth)
	{
		dsv = m_LowResDsvHandle;
	}
	else
	{
		dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_DsvHeap->GetCPUDescriptorHandleForHeapStart());
	}
	const bool useVisibilityBuffer =
		RendererSettings::GetComputeGBufferEnabled() && VisibilityBuffer::IsAvailable();
	if (useVisibilityBuffer)
	{
		const UINT visibilityIndex = static_cast<UINT>(GBufferType::VISIBILITY);
		m_CommandList->OMSetRenderTargets(1, &m_GBufferRtvHandles[visibilityIndex], TRUE, &dsv);
	}
	else
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handles[g_kGEOMETRY_GBUFFER_COUNT]{};
		for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i) handles[i] = m_GBufferRtvHandles[i];
		m_CommandList->OMSetRenderTargets(g_kGEOMETRY_GBUFFER_COUNT, handles, TRUE, &dsv);
	}
	OcclusionCulling::BeginPhaseTwo();
	return OcclusionCulling::GetPhase() == 2u;
}

void ScenePasses::EndScenePass()
{
	if (!GetRenderTarget(RenderTargetType::Scene)) return;
	OcclusionCulling::EndFrame();

	if (m_RenderMode == RenderMode::DEFERRED)
	{
		const bool useVisibilityBuffer =
			RendererSettings::GetComputeGBufferEnabled() &&
			VisibilityBuffer::IsAvailable();
		if (useVisibilityBuffer)
		{
			const UINT visibilityIndex = static_cast<UINT>(GBufferType::VISIBILITY);
			D3D12_RESOURCE_BARRIER beforeCompute[g_kGEOMETRY_GBUFFER_COUNT + 2]{};
			UINT beforeCount = 0;
			beforeCompute[beforeCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_GBufferTargets[visibilityIndex].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
			{
				beforeCompute[beforeCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
					m_GBufferTargets[i].Get(),
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			}
			if (m_UseLowResDepth)
			{
				beforeCompute[beforeCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
					m_LowResDepthBuffer.Get(),
					D3D12_RESOURCE_STATE_DEPTH_WRITE,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			else
			{
				beforeCompute[beforeCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
					m_DepthStencilBuffer.Get(),
					m_DepthStencilState,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			}
			m_CommandList->ResourceBarrier(beforeCount, beforeCompute);
			{
				RenderProfiler::ScopedEvent profile("Compute GBuffer", m_CommandList.Get());
				VisibilityBuffer::GenerateGBuffer(
					m_CommandList.Get(),
					m_GBufferSrvHandles[visibilityIndex],
					m_SceneWidth,
					m_SceneHeight);
			}
			D3D12_RESOURCE_BARRIER afterCompute[g_kGEOMETRY_GBUFFER_COUNT + 1]{};
			afterCompute[0] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_GBufferTargets[visibilityIndex].Get(),
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
			{
				afterCompute[i + 1] = CD3DX12_RESOURCE_BARRIER::Transition(
					m_GBufferTargets[i].Get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			m_CommandList->ResourceBarrier(_countof(afterCompute), afterCompute);
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
			barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_GBufferTargets[i].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}

		if (m_UseLowResDepth)
		{
			barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_LowResDepthBuffer.Get(),
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		else
		{
			barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
				m_DepthStencilBuffer.Get(),
				m_DepthStencilState,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			m_DepthStencilState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		}

		m_CommandList->ResourceBarrier(barrierCount, barriers);
		m_IsDeferredGeometryPass = false;
		m_IsSceneColorForwardPass = false;
		return;
	}

	D3D12_RESOURCE_BARRIER barriers[2]{};
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
		GetRenderTarget(RenderTargetType::Scene).Get(),
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

void ScenePasses::RenderVelocityBuffer()
{
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
		m_GBufferTargets[velocityIndex].Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_CommandList->ResourceBarrier(1, &toVelocityRt);

	const float clearVelocity[4] = { 0.5f, 0.5f, 0.0f, 0.0f };
	m_CommandList->ClearRenderTargetView(m_GBufferRtvHandles[velocityIndex], clearVelocity, 0, nullptr);
	m_CommandList->OMSetRenderTargets(1, &m_GBufferRtvHandles[velocityIndex], FALSE, nullptr);
	m_CommandList->SetPipelineState(velocityPso);
	m_CommandList->SetGraphicsRootSignature(m_AaRootSignature.Get());
	RenderFrame::SetDescriptorHeap();
	m_CommandList->RSSetViewports(1, &m_Viewport);
	m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

	if (m_PostProcessConstantBuffer)
	{
		m_CommandList->SetGraphicsRootConstantBufferView(0,
			m_PostProcessConstantBuffer->GetGPUVirtualAddress() + m_FrameIndex * g_kPP_CB_ALIGNED_SIZE);
	}
	m_CommandList->SetGraphicsRootDescriptorTable(1,
		m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
	m_CommandList->SetGraphicsRootDescriptorTable(2,
		m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
	m_CommandList->SetGraphicsRootDescriptorTable(3,
		m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);

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


}

void ScenePasses::EndVelocityBuffer()
{
	const UINT velocityIndex = static_cast<UINT>(GBufferType::VELOCITY);
	if (!m_CommandList || !m_GBufferTargets[velocityIndex]) return;
	D3D12_RESOURCE_BARRIER toVelocitySrv = CD3DX12_RESOURCE_BARRIER::Transition(
		m_GBufferTargets[velocityIndex].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_CommandList->ResourceBarrier(1, &toVelocitySrv);
}
