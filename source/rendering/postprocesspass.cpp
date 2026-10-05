#include "pch.h"
#include "postprocesspass.h"
#include "scenepasses.h"
#include "renderframe.h"
#include "frameconstants.h"
#include "lightingresources.h"
#include "graphicsdevice.h"
#include "shaderpaths.h"
#include "world.h"
#include "ecs.h"
#include "texturemanager.h"
#include "componentmanager.h"
#include "imguimanager.h"
#include "psomanager.h"
#include "camera.h"
#include "renderprofiler.h"
#include "renderersettings.h"
#include "spatialupscaler.h"
#include "visibilitybuffer.h"
#include "screenspaceeffects.h"
#include "occlusionculling.h"
#include "materialbindings.h"


namespace
{
struct PostProcessConstants
{
	XMFLOAT4 Flags{};
	XMFLOAT4 PPCameraPos{};
	XMFLOAT4 HdrFlags{};
	XMFLOAT4 BloomParams{};
	XMFLOAT4X4 PPInvViewProjection{};
	XMFLOAT4X4 PPViewProjection{};
};

static_assert(sizeof(PostProcessConstants) <= RendererState::g_kPP_CB_ALIGNED_SIZE);

UpscaleMode ResolveUpscaleMode(UINT inputWidth, UINT inputHeight, UINT outputWidth, UINT outputHeight)
{
	UpscaleMode mode = RendererSettings::GetUpscaleMode();
	if (!SpatialUpscaler::IsAvailable(mode) ||
		!SpatialUpscaler::IsScaleSupported(
			mode,
			inputWidth,
			inputHeight,
			outputWidth,
			outputHeight))
	{
		return UpscaleMode::Bilateral;
	}
	return mode;
}


}

void PostProcessPass::ApplyPostProcess(const PostProcessComponent& config)
{
	auto DrawFullscreenPass = [&](ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE targetRtv,
		D3D12_GPU_DESCRIPTOR_HANDLE sourceHandle, float intensity, float renderModeFlag, float deferredLightStrength,
		D3D12_GPU_DESCRIPTOR_HANDLE atmosphereHandle = D3D12_GPU_DESCRIPTOR_HANDLE{},
		D3D12_GPU_DESCRIPTOR_HANDLE rimStyleHandle = D3D12_GPU_DESCRIPTOR_HANDLE{},
		D3D12_GPU_DESCRIPTOR_HANDLE rimLightHandle = D3D12_GPU_DESCRIPTOR_HANDLE{},
		D3D12_GPU_DESCRIPTOR_HANDLE bloomHandle = D3D12_GPU_DESCRIPTOR_HANDLE{},
		float bloomPass = 0.0f)
		{
			if (!pso || !m_PostProcessRootSignature)
			{
				return;
			}


			const bool needsLightingConstants = deferredLightStrength > 0.0f;
			if (needsLightingConstants)
			{
				LightingResources::UpdateLightConstantBuffer(deferredLightStrength);
				LightingResources::UpdateShadowConstantBuffer();
			}

			m_CommandList->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
			m_CommandList->SetPipelineState(pso);
			m_CommandList->SetGraphicsRootSignature(m_PostProcessRootSignature.Get());

			RenderFrame::SetDescriptorHeap();

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
				params.Flags = XMFLOAT4(ImGuiManager::GetExposure(), intensity, renderModeFlag, m_ResolutionScale);
				params.PPCameraPos = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
				params.HdrFlags = XMFLOAT4(
					static_cast<float>(ImGuiManager::IsHdrEnabled()),
					static_cast<float>(ImGuiManager::IsToneMapEnabled()),
					static_cast<float>(RendererSettings::GetSsaoEnabled()),
					static_cast<float>(RendererSettings::GetSsgiEnabled()));
				params.BloomParams = XMFLOAT4(
					bloomPass,
					config.BloomThreshold,
					config.BloomSoftKnee,
					config.BloomRadius);
				XMStoreFloat4x4(&params.PPInvViewProjection, XMMatrixTranspose(invViewProjection));
				XMStoreFloat4x4(&params.PPViewProjection, XMMatrixTranspose(view * projection));
				auto* ppDst = static_cast<UINT8*>(m_pPostProcessCbvDataBegin) +
					m_FrameIndex * g_kPP_CB_ALIGNED_SIZE;
				memcpy(ppDst, &params, sizeof(params));
			}
			m_CommandList->SetGraphicsRootDescriptorTable(0, sourceHandle);
			if (m_PostProcessConstantBuffer)
			{
				m_CommandList->SetGraphicsRootConstantBufferView(
					1,
					m_PostProcessConstantBuffer->GetGPUVirtualAddress() + m_FrameIndex * g_kPP_CB_ALIGNED_SIZE);
			}
			if (m_LightConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(2, LightingResources::GetCurrentLightConstantBufferAddress());
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
			if (m_ShadowConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(5, LightingResources::GetCurrentShadowConstantBufferAddress());
			if (m_PBRConstantBuffer) m_CommandList->SetGraphicsRootConstantBufferView(6, MaterialBindings::GetPBRConstantBufferAddress());
			if (atmosphereHandle.ptr != 0) m_CommandList->SetGraphicsRootDescriptorTable(7, atmosphereHandle);
			if (rimStyleHandle.ptr != 0) m_CommandList->SetGraphicsRootDescriptorTable(8, rimStyleHandle);
			if (rimLightHandle.ptr != 0) m_CommandList->SetGraphicsRootDescriptorTable(9, rimLightHandle);
			const D3D12_GPU_VIRTUAL_ADDRESS lightTileAddress =
				LightingResources::GetCurrentLightTileIndexBufferAddress();
			if (lightTileAddress != 0)
			{
				m_CommandList->SetGraphicsRootShaderResourceView(10, lightTileAddress);
			}
			int monitorSrvIndex = m_MonitorTextureSrvIndex;
			if (monitorSrvIndex < 0)
			{
				monitorSrvIndex = TextureManager::GetDefaultTextureIndex();
			}
			CD3DX12_GPU_DESCRIPTOR_HANDLE monitorSrvHandle(
				m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
				monitorSrvIndex,
				m_CbvIncrementSize);
			m_CommandList->SetGraphicsRootDescriptorTable(11, monitorSrvHandle);
			m_CommandList->SetGraphicsRootDescriptorTable(12, ScreenSpaceEffects::GetAoSrv());
			m_CommandList->SetGraphicsRootDescriptorTable(13, ScreenSpaceEffects::GetGiSrv());
			const D3D12_GPU_VIRTUAL_ADDRESS volumetricLightAddress =
				LightingResources::GetCurrentVolumetricLightIndexBufferAddress();
			if (volumetricLightAddress != 0)
			{
				m_CommandList->SetGraphicsRootShaderResourceView(14, volumetricLightAddress);
			}
			D3D12_GPU_DESCRIPTOR_HANDLE bloomSourceHandle;
			if (bloomHandle.ptr != 0)
			{
				bloomSourceHandle = bloomHandle;
			}
			else
			{
				bloomSourceHandle = sourceHandle;
			}
			m_CommandList->SetGraphicsRootDescriptorTable(
				15,
				bloomSourceHandle);
			m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			m_CommandList->DrawInstanced(3, 1, 0, 0);
		};

	if (!GetRenderTarget(RenderTargetType::EditorScene))
	{
		return;
	}

	ID3D12PipelineState* postProcessPso = PsoManager::GetPostProcessPso(config.Type);

	if (m_RenderMode == RenderMode::DEFERRED)
	{
		ID3D12PipelineState* deferredLightingPso = PsoManager::GetDeferredLightingPso();
		ID3D12PipelineState* atmospherePso = PsoManager::GetAtmospherePso();
		const UINT atmosphereIndex = static_cast<UINT>(GBufferType::ATMOSPHERE);
		if (!deferredLightingPso || !atmospherePso || !GetRenderTarget(RenderTargetType::Scene) || !m_GBufferTargets[atmosphereIndex])
		{
			return;
		}

		D3D12_RESOURCE_BARRIER atmosphereToRt = CD3DX12_RESOURCE_BARRIER::Transition(
			m_GBufferTargets[atmosphereIndex].Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_CommandList->ResourceBarrier(1, &atmosphereToRt);
		const float atmosphereClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		m_CommandList->ClearRenderTargetView(m_GBufferRtvHandles[atmosphereIndex], atmosphereClear, 0, nullptr);
		const D3D12_RESOURCE_DESC atmosphereDesc = m_GBufferTargets[atmosphereIndex]->GetDesc();
		const CD3DX12_VIEWPORT atmosphereViewport(
			0.0f,
			0.0f,
			static_cast<float>(atmosphereDesc.Width),
			static_cast<float>(atmosphereDesc.Height));
		const CD3DX12_RECT atmosphereScissor(
			0,
			0,
			static_cast<LONG>(atmosphereDesc.Width),
			static_cast<LONG>(atmosphereDesc.Height));
		m_CommandList->RSSetViewports(1, &atmosphereViewport);
		m_CommandList->RSSetScissorRects(1, &atmosphereScissor);
		{
			RenderProfiler::ScopedEvent profile("Atmosphere", m_CommandList.Get());
			DrawFullscreenPass(
				atmospherePso,
				m_GBufferRtvHandles[atmosphereIndex],
				m_GBufferSrvHandles[static_cast<UINT>(GBufferType::BASE_COLOR)],
				1.0f,
				1.0f,
				1.35f);
		}
		m_CommandList->RSSetViewports(1, &m_Viewport);
		m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
		D3D12_RESOURCE_BARRIER atmosphereToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			m_GBufferTargets[atmosphereIndex].Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &atmosphereToSrv);

		D3D12_RESOURCE_BARRIER toSceneRt = CD3DX12_RESOURCE_BARRIER::Transition(
			GetRenderTarget(RenderTargetType::Scene).Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
		ScreenSpaceEffects::Execute(
			m_CommandList.Get(),
			m_GBufferTargets[static_cast<UINT>(GBufferType::DEPTH)].Get(),
			m_GBufferTargets[static_cast<UINT>(GBufferType::NORMAL)].Get(),
			m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)],
			m_GBufferSrvHandles[static_cast<UINT>(GBufferType::NORMAL)],
			m_FrameIndex);
		m_CommandList->ResourceBarrier(1, &toSceneRt);
		m_CommandList->ClearRenderTargetView(GetRenderTargetRtvHandle(RenderTargetType::Scene), m_kSceneClearColor, 0, nullptr);

		{
			RenderProfiler::ScopedEvent profile("Deferred Lighting", m_CommandList.Get());
			DrawFullscreenPass(
				deferredLightingPso,
				GetRenderTargetRtvHandle(RenderTargetType::Scene),
				m_GBufferSrvHandles[static_cast<UINT>(GBufferType::BASE_COLOR)],
				1.0f,
				1.0f,
				1.35f,
				m_GBufferSrvHandles[atmosphereIndex]);
		}

		D3D12_RESOURCE_BARRIER toSceneSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			GetRenderTarget(RenderTargetType::Scene).Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &toSceneSrv);
		ScreenSpaceEffects::CaptureHistory(m_CommandList.Get(), GetRenderTarget(RenderTargetType::Scene).Get());
	}

	if (!postProcessPso || !GetRenderTarget(RenderTargetType::PostProcess))
	{
		return;
	}

	const UINT bloomIndex = static_cast<UINT>(GBufferType::BLOOM);
	const bool useBloomBuffer =
		config.Type == PostProcessType::BLOOM &&
		m_RenderMode == RenderMode::DEFERRED &&
		m_GBufferTargets[bloomIndex];
	if (useBloomBuffer)
	{
		D3D12_RESOURCE_BARRIER bloomToRt = CD3DX12_RESOURCE_BARRIER::Transition(
			m_GBufferTargets[bloomIndex].Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_CommandList->ResourceBarrier(1, &bloomToRt);
		const float bloomClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		m_CommandList->ClearRenderTargetView(m_GBufferRtvHandles[bloomIndex], bloomClear, 0, nullptr);
		{
			const D3D12_RESOURCE_DESC bloomDesc = m_GBufferTargets[bloomIndex]->GetDesc();
			const CD3DX12_VIEWPORT bloomViewport(
				0.0f, 0.0f,
				static_cast<float>(bloomDesc.Width),
				static_cast<float>(bloomDesc.Height));
			const CD3DX12_RECT bloomScissor(
				0, 0,
				static_cast<LONG>(bloomDesc.Width),
				static_cast<LONG>(bloomDesc.Height));
			m_CommandList->RSSetViewports(1, &bloomViewport);
			m_CommandList->RSSetScissorRects(1, &bloomScissor);

			RenderProfiler::ScopedEvent profile("Bloom Extract", m_CommandList.Get());
			DrawFullscreenPass(
				m_BloomExtractPso.Get(),
				m_GBufferRtvHandles[bloomIndex],
				GetRenderTargetSrvHandle(RenderTargetType::Scene),
				1.0f,
				0.0f,
				0.0f,
				D3D12_GPU_DESCRIPTOR_HANDLE{},
				D3D12_GPU_DESCRIPTOR_HANDLE{},
				D3D12_GPU_DESCRIPTOR_HANDLE{},
				GetRenderTargetSrvHandle(RenderTargetType::Scene),
				1.0f);

			m_CommandList->RSSetViewports(1, &m_Viewport);
			m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
		}
		D3D12_RESOURCE_BARRIER bloomToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			m_GBufferTargets[bloomIndex].Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &bloomToSrv);
	}


	D3D12_RESOURCE_BARRIER postToRt = CD3DX12_RESOURCE_BARRIER::Transition(
		GetRenderTarget(RenderTargetType::PostProcess).Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_CommandList->ResourceBarrier(1, &postToRt);
	m_CommandList->ClearRenderTargetView(GetRenderTargetRtvHandle(RenderTargetType::PostProcess), m_kSceneClearColor, 0, nullptr);
	m_CommandList->RSSetViewports(1, &m_Viewport);
	m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
	{
		RenderProfiler::ScopedEvent profile("Final PostProcess", m_CommandList.Get());
		D3D12_GPU_DESCRIPTOR_HANDLE bloomSourceHandleValue;
		if (useBloomBuffer)
		{
			bloomSourceHandleValue = m_GBufferSrvHandles[bloomIndex];
		}
		else
		{
			bloomSourceHandleValue = D3D12_GPU_DESCRIPTOR_HANDLE{};
		}
		float shaderValue;
		if (useBloomBuffer)
		{
			shaderValue = 2.0f;
		}
		else
		{
			shaderValue = 0.0f;
		}
		DrawFullscreenPass(
			postProcessPso,
			GetRenderTargetRtvHandle(RenderTargetType::PostProcess),
			GetRenderTargetSrvHandle(RenderTargetType::Scene),
			config.Intensity,
			0.0f,
			0.0f,
			D3D12_GPU_DESCRIPTOR_HANDLE{},
			D3D12_GPU_DESCRIPTOR_HANDLE{},
			D3D12_GPU_DESCRIPTOR_HANDLE{},
			bloomSourceHandleValue,
			shaderValue);
	}
	D3D12_RESOURCE_BARRIER postToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
		GetRenderTarget(RenderTargetType::PostProcess).Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_CommandList->ResourceBarrier(1, &postToSrv);

	const bool needsUpscale = m_SceneWidth != m_Width || m_SceneHeight != m_Height;
	const UpscaleMode upscaleMode =
		ResolveUpscaleMode(m_SceneWidth, m_SceneHeight, m_Width, m_Height);

	D3D12_GPU_DESCRIPTOR_HANDLE upscaleSourceSrv = GetRenderTargetSrvHandle(RenderTargetType::PostProcess);
	ID3D12Resource* upscaleSource = GetRenderTarget(RenderTargetType::PostProcess).Get();

	if (needsUpscale && upscaleMode != UpscaleMode::Bilateral && GetRenderTarget(RenderTargetType::PreUpscaleAa))
	{
		const bool useTaa =
			m_AntiAliasingMode == AntiAliasingMode::TAA &&
			GetRenderTarget(RenderTargetType::PreUpscaleAaHistory);
		ID3D12PipelineState* aaPso;
		if (useTaa)
		{
			aaPso = PsoManager::GetTaaBlendPso();
		}
		else
		{
			aaPso = PsoManager::GetFxaaPso();
		}
		if (aaPso && m_AaRootSignature)
		{
			const char* antiAliasingLabel;
			if (useTaa)
			{
				antiAliasingLabel = "Pre-upscale TAA";
			}
			else
			{
				antiAliasingLabel = "Pre-upscale FXAA";
			}
			RenderProfiler::ScopedEvent profile(
				antiAliasingLabel,
				m_CommandList.Get());
			D3D12_RESOURCE_BARRIER preAaToRt = CD3DX12_RESOURCE_BARRIER::Transition(
				GetRenderTarget(RenderTargetType::PreUpscaleAa).Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
			m_CommandList->ResourceBarrier(1, &preAaToRt);
			m_CommandList->OMSetRenderTargets(1, &GetRenderTargetRtvHandle(RenderTargetType::PreUpscaleAa), FALSE, nullptr);
			m_CommandList->SetPipelineState(aaPso);
			m_CommandList->SetGraphicsRootSignature(m_AaRootSignature.Get());
			RenderFrame::SetDescriptorHeap();
			m_CommandList->SetGraphicsRootConstantBufferView(
				0,
				m_PostProcessConstantBuffer->GetGPUVirtualAddress() + m_FrameIndex * g_kPP_CB_ALIGNED_SIZE);
			m_CommandList->SetGraphicsRootDescriptorTable(1, GetRenderTargetSrvHandle(RenderTargetType::PostProcess));
			if (useTaa)
			{
				m_CommandList->SetGraphicsRootDescriptorTable(
					2,
					GetRenderTargetSrvHandle(RenderTargetType::PreUpscaleAaHistory));
				m_CommandList->SetGraphicsRootDescriptorTable(
					3,
					m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
				m_CommandList->SetGraphicsRootDescriptorTable(
					5,
					CD3DX12_GPU_DESCRIPTOR_HANDLE(
						m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
						RendererState::g_kVELOCITY_CALCULATION_SRV_INDEX,
						m_CbvIncrementSize));

				XMMATRIX prevView = XMLoadFloat4x4(&m_PrevViewMatrix);
				XMMATRIX prevProj = XMLoadFloat4x4(&m_PrevProjMatrix);
				XMMATRIX prevViewProj = XMMatrixTranspose(prevView * prevProj);
				float constants[20] = {};
				memcpy(constants, &prevViewProj, sizeof(XMFLOAT4X4));
				constants[16] = 0.90f;
				constants[17] = 1.0f / max(static_cast<float>(m_SceneWidth), 1.0f);
				constants[18] = 1.0f / max(static_cast<float>(m_SceneHeight), 1.0f);
				constants[19] = static_cast<float>(m_TaaFrameIndex > 0);
				m_CommandList->SetGraphicsRoot32BitConstants(4, 20, constants, 0);
			}
			else
			{
				m_CommandList->SetGraphicsRootDescriptorTable(2, GetRenderTargetSrvHandle(RenderTargetType::PostProcess));
				m_CommandList->SetGraphicsRootDescriptorTable(3, GetRenderTargetSrvHandle(RenderTargetType::PostProcess));
				m_CommandList->SetGraphicsRootDescriptorTable(5, GetRenderTargetSrvHandle(RenderTargetType::PostProcess));
				const float reciprocalExtent[2] =
				{
					1.0f / max(static_cast<float>(m_SceneWidth), 1.0f),
					1.0f / max(static_cast<float>(m_SceneHeight), 1.0f)
				};
				m_CommandList->SetGraphicsRoot32BitConstants(4, 2, reciprocalExtent, 0);
			}
			m_CommandList->RSSetViewports(1, &m_Viewport);
			m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
			m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			m_CommandList->DrawInstanced(3, 1, 0, 0);

			if (useTaa)
			{
				D3D12_RESOURCE_BARRIER toCopy[] =
				{
					CD3DX12_RESOURCE_BARRIER::Transition(
						GetRenderTarget(RenderTargetType::PreUpscaleAa).Get(),
						D3D12_RESOURCE_STATE_RENDER_TARGET,
						D3D12_RESOURCE_STATE_COPY_SOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(
						GetRenderTarget(RenderTargetType::PreUpscaleAaHistory).Get(),
						D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
						D3D12_RESOURCE_STATE_COPY_DEST)
				};
				m_CommandList->ResourceBarrier(_countof(toCopy), toCopy);
				m_CommandList->CopyResource(
					GetRenderTarget(RenderTargetType::PreUpscaleAaHistory).Get(),
					GetRenderTarget(RenderTargetType::PreUpscaleAa).Get());
				D3D12_RESOURCE_BARRIER toShaderRead[] =
				{
					CD3DX12_RESOURCE_BARRIER::Transition(
						GetRenderTarget(RenderTargetType::PreUpscaleAa).Get(),
						D3D12_RESOURCE_STATE_COPY_SOURCE,
						D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(
						GetRenderTarget(RenderTargetType::PreUpscaleAaHistory).Get(),
						D3D12_RESOURCE_STATE_COPY_DEST,
						D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
				};
				m_CommandList->ResourceBarrier(_countof(toShaderRead), toShaderRead);
				++m_TaaFrameIndex;
			}
			else
			{
				D3D12_RESOURCE_BARRIER preAaToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
					GetRenderTarget(RenderTargetType::PreUpscaleAa).Get(),
					D3D12_RESOURCE_STATE_RENDER_TARGET,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				m_CommandList->ResourceBarrier(1, &preAaToSrv);
			}
			upscaleSourceSrv = GetRenderTargetSrvHandle(RenderTargetType::PreUpscaleAa);
			upscaleSource = GetRenderTarget(RenderTargetType::PreUpscaleAa).Get();
		}
	}

	if (!needsUpscale)
	{
		D3D12_RESOURCE_BARRIER barriers[] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(
				upscaleSource,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(
				GetRenderTarget(RenderTargetType::EditorScene).Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_DEST)
		};
		m_CommandList->ResourceBarrier(_countof(barriers), barriers);
		m_CommandList->CopyResource(GetRenderTarget(RenderTargetType::EditorScene).Get(), upscaleSource);
		barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
			upscaleSource,
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
			GetRenderTarget(RenderTargetType::EditorScene).Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(_countof(barriers), barriers);
	}
	else if (upscaleMode == UpscaleMode::Bilateral)
	{
		D3D12_RESOURCE_BARRIER editorToRt = CD3DX12_RESOURCE_BARRIER::Transition(
			GetRenderTarget(RenderTargetType::EditorScene).Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_CommandList->ResourceBarrier(1, &editorToRt);
		m_CommandList->ClearRenderTargetView(GetRenderTargetRtvHandle(RenderTargetType::EditorScene), m_kSceneClearColor, 0, nullptr);
		m_CommandList->RSSetViewports(1, &m_FullViewport);
		m_CommandList->RSSetScissorRects(1, &m_FullScissorRect);

		ID3D12PipelineState* upscalePso = PsoManager::GetUpscaleBilateralPso();
		if (upscalePso && m_UpscaleRootSignature)
		{
			RenderProfiler::ScopedEvent profile("Bilateral Upscale", m_CommandList.Get());
			m_CommandList->OMSetRenderTargets(1, &GetRenderTargetRtvHandle(RenderTargetType::EditorScene), FALSE, nullptr);
			m_CommandList->SetPipelineState(upscalePso);
			m_CommandList->SetGraphicsRootSignature(m_UpscaleRootSignature.Get());
			RenderFrame::SetDescriptorHeap();
			m_CommandList->SetGraphicsRootDescriptorTable(0, upscaleSourceSrv);
			m_CommandList->SetGraphicsRootDescriptorTable(
				1,
				m_GBufferSrvHandles[static_cast<UINT>(GBufferType::DEPTH)]);
			m_CommandList->SetGraphicsRootConstantBufferView(
				2,
				m_PostProcessConstantBuffer->GetGPUVirtualAddress() + m_FrameIndex * g_kPP_CB_ALIGNED_SIZE);
			m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			m_CommandList->DrawInstanced(3, 1, 0, 0);
		}
		D3D12_RESOURCE_BARRIER editorToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			GetRenderTarget(RenderTargetType::EditorScene).Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &editorToSrv);
	}
	else
	{
		D3D12_RESOURCE_BARRIER barriers[] =
		{
			CD3DX12_RESOURCE_BARRIER::Transition(
				upscaleSource,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(
				GetRenderTarget(RenderTargetType::EditorScene).Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		};
		m_CommandList->ResourceBarrier(_countof(barriers), barriers);
		{
			const char* upscalerLabel;
			if (upscaleMode == UpscaleMode::Fsr1)
			{
				upscalerLabel = "FSR 1 EASU + RCAS";
			}
			else
			{
				upscalerLabel = "NVIDIA Image Scaling";
			}
			RenderProfiler::ScopedEvent profile(
				upscalerLabel,
				m_CommandList.Get());
			SpatialUpscaler::Execute(
				m_CommandList.Get(),
				upscaleSourceSrv,
				GetRenderTarget(RenderTargetType::EditorScene).Get(),
				m_EditorSceneUavHandle,
				m_SceneWidth,
				m_SceneHeight,
				m_Width,
				m_Height,
				upscaleMode);
		}
		barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
			upscaleSource,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
			GetRenderTarget(RenderTargetType::EditorScene).Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(_countof(barriers), barriers);
	}

	m_CommandList->RSSetViewports(1, &m_FullViewport);
	m_CommandList->RSSetScissorRects(1, &m_FullScissorRect);
}

void PostProcessPass::ApplyAntiAliasing()
{
	if (!m_AaRenderTarget || !m_AaRootSignature)
		return;

	AntiAliasingMode mode = m_AntiAliasingMode;
	if (mode == AntiAliasingMode::NONE)
		return;
	const bool usesPreUpscaleAa =
		(m_SceneWidth != m_Width || m_SceneHeight != m_Height) &&
		ResolveUpscaleMode(m_SceneWidth, m_SceneHeight, m_Width, m_Height) !=
		UpscaleMode::Bilateral;
	if (usesPreUpscaleAa)
	{


		return;
	}

	ID3D12PipelineState* pso = nullptr;
	if (mode == AntiAliasingMode::FXAA)
		pso = PsoManager::GetFxaaPso();
	else if (mode == AntiAliasingMode::TAA)
		pso = PsoManager::GetTaaBlendPso();
	else
		return;

	if (!pso)
		return;

	RenderFrame::SetDescriptorHeap();

	D3D12_RESOURCE_BARRIER toAaCopy = CD3DX12_RESOURCE_BARRIER::Transition(
		m_AaRenderTarget.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_CommandList->ResourceBarrier(1, &toAaCopy);

	m_CommandList->OMSetRenderTargets(1, &m_AaRtvHandle, FALSE, nullptr);
	m_CommandList->SetPipelineState(pso);
	m_CommandList->SetGraphicsRootSignature(m_AaRootSignature.Get());
	m_CommandList->RSSetViewports(1, &m_FullViewport);
	m_CommandList->RSSetScissorRects(1, &m_FullScissorRect);

	m_CommandList->SetGraphicsRootConstantBufferView(0, m_PostProcessConstantBuffer->GetGPUVirtualAddress() + m_FrameIndex * g_kPP_CB_ALIGNED_SIZE);

	m_CommandList->SetGraphicsRootDescriptorTable(1, GetRenderTargetSrvHandle(RenderTargetType::EditorScene));

	if (mode == AntiAliasingMode::FXAA)
	{
		m_CommandList->SetGraphicsRootDescriptorTable(2, GetRenderTargetSrvHandle(RenderTargetType::EditorScene));
		m_CommandList->SetGraphicsRootDescriptorTable(3, GetRenderTargetSrvHandle(RenderTargetType::EditorScene));
		m_CommandList->SetGraphicsRootDescriptorTable(5, GetRenderTargetSrvHandle(RenderTargetType::EditorScene));

		struct FxaaCb { float rcpWidth; float rcpHeight; } cb;
		cb.rcpWidth = 1.0f / (float)m_Width;
		cb.rcpHeight = 1.0f / (float)m_Height;
		m_CommandList->SetGraphicsRoot32BitConstants(4, 2, &cb, 0);
	}
	else if (mode == AntiAliasingMode::TAA)
	{
		CD3DX12_GPU_DESCRIPTOR_HANDLE historySrv(
			m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
			RendererState::g_kAA_HISTORY_SRV_INDEX, m_CbvIncrementSize);
		m_CommandList->SetGraphicsRootDescriptorTable(2, historySrv);

		CD3DX12_GPU_DESCRIPTOR_HANDLE depthSrv(
			m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
			RendererState::g_kDEPTH_SRV_INDEX, m_CbvIncrementSize);
		m_CommandList->SetGraphicsRootDescriptorTable(3, depthSrv);
		CD3DX12_GPU_DESCRIPTOR_HANDLE velocityCalculationSrv(
			m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
			RendererState::g_kVELOCITY_CALCULATION_SRV_INDEX, m_CbvIncrementSize);
		m_CommandList->SetGraphicsRootDescriptorTable(5, velocityCalculationSrv);

		XMMATRIX prevView = XMLoadFloat4x4(&m_PrevViewMatrix);
		XMMATRIX prevProj = XMLoadFloat4x4(&m_PrevProjMatrix);
		XMMATRIX prevViewProj = XMMatrixTranspose(prevView * prevProj);

		float cbData[20] = {};
		memcpy(cbData, &prevViewProj, 64);
		cbData[16] = 0.90f;
		cbData[17] = 1.0f / (float)m_Width;
		cbData[18] = 1.0f / (float)m_Height;
		cbData[19] = static_cast<float>(m_TaaFrameIndex > 0);
		m_CommandList->SetGraphicsRoot32BitConstants(4, 20, cbData, 0);
	}

	m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_CommandList->DrawInstanced(3, 1, 0, 0);

	D3D12_RESOURCE_BARRIER toAaSrv = CD3DX12_RESOURCE_BARRIER::Transition(
		m_AaRenderTarget.Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	m_CommandList->ResourceBarrier(1, &toAaSrv);

	D3D12_RESOURCE_BARRIER toEditorCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
		GetRenderTarget(RenderTargetType::EditorScene).Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COPY_DEST);
	m_CommandList->ResourceBarrier(1, &toEditorCopyDest);

	m_CommandList->CopyResource(GetRenderTarget(RenderTargetType::EditorScene).Get(), m_AaRenderTarget.Get());

	if (mode == AntiAliasingMode::TAA)
	{
		m_TaaFrameIndex++;

		D3D12_RESOURCE_BARRIER toHistoryCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
			m_AaRenderTarget.Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_COPY_DEST);
		m_CommandList->ResourceBarrier(1, &toHistoryCopyDest);

		D3D12_RESOURCE_BARRIER toEditorCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
			GetRenderTarget(RenderTargetType::EditorScene).Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		m_CommandList->ResourceBarrier(1, &toEditorCopySource);

		m_CommandList->CopyResource(m_AaRenderTarget.Get(), GetRenderTarget(RenderTargetType::EditorScene).Get());

		D3D12_RESOURCE_BARRIER toHistorySrv = CD3DX12_RESOURCE_BARRIER::Transition(
			m_AaRenderTarget.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &toHistorySrv);

		D3D12_RESOURCE_BARRIER toEditorSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			GetRenderTarget(RenderTargetType::EditorScene).Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &toEditorSrv);
	}
	else
	{
		D3D12_RESOURCE_BARRIER toAaFinalSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			m_AaRenderTarget.Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &toAaFinalSrv);

		D3D12_RESOURCE_BARRIER toEditorFinalSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			GetRenderTarget(RenderTargetType::EditorScene).Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		m_CommandList->ResourceBarrier(1, &toEditorFinalSrv);
	}
}
