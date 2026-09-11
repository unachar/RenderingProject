#pragma once
#include "rendererresource.h"
#include <array>
#include <cstddef>

enum class RenderTargetType
{
	Scene,
	PostProcess,
	PreUpscaleAa,
	PreUpscaleAaHistory,
	EditorScene,
	Count
};

constexpr size_t kRenderTargetCount = static_cast<size_t>(RenderTargetType::Count);
using RenderTargetResourceArray = array<ComPtr<ID3D12Resource>, kRenderTargetCount>;
using RenderTargetCpuHandleArray = array<D3D12_CPU_DESCRIPTOR_HANDLE, kRenderTargetCount>;
using RenderTargetGpuHandleArray = array<D3D12_GPU_DESCRIPTOR_HANDLE, kRenderTargetCount>;

class RendererDraw : protected RendererState
{
private:
	inline static RenderTargetResourceArray m_RenderTargetResources{};
	inline static RenderTargetCpuHandleArray m_RenderTargetRtvHandles{};
	inline static RenderTargetGpuHandleArray m_RenderTargetSrvHandles{};

	static constexpr size_t ToIndex(RenderTargetType type)
	{
		return static_cast<size_t>(type);
	}

	static ComPtr<ID3D12Resource>& GetRenderTarget(RenderTargetType type)
	{
		return m_RenderTargetResources[ToIndex(type)];
	}

	static D3D12_CPU_DESCRIPTOR_HANDLE& GetRenderTargetRtvHandle(RenderTargetType type)
	{
		return m_RenderTargetRtvHandles[ToIndex(type)];
	}

	static D3D12_GPU_DESCRIPTOR_HANDLE& GetRenderTargetSrvHandle(RenderTargetType type)
	{
		return m_RenderTargetSrvHandles[ToIndex(type)];
	}

public:
	using RendererState::GetSceneColorFormat;
	using RendererState::GetGBufferFormat;

	static bool CreateDepthBuffer();
	static bool CreateShadowDepthBuffer();
	static bool CreateSceneRenderTarget();
	static void ReleaseGBufferResources();

	static void BeginDraw();
	static void BeginPass(ID3D12RootSignature* rootSignature, D3D_PRIMITIVE_TOPOLOGY topology);
	static void BeginSpritePass();
	static void BeginModelPass();
	static void BeginLinePass();
	static bool BeginShadowPass(UINT shadowIndex);
	static void EndShadowPass();
	static void EndShadowPassBatch();
	static void BeginBackBufferPass();
	static void BeginEditorSceneOverlayPass();
	static void PrepareTransparentSceneCopy();
	static void EndEditorSceneOverlayPass();
	static void SetDescriptorHeap();
	static void EndDraw();
	static void BeginScenePass();
	static void ApplyPostProcess(const PostProcessComponent& config);
	static void ApplyAntiAliasing();
	static void EndScenePass();
	static bool BuildOcclusionHierarchyAndBeginPhaseTwo();
	static void RenderVelocityBuffer();
	static void EndVelocityBuffer();
	static void ResizeScene(UINT width, UINT height);

	static D3D12_GPU_DESCRIPTOR_HANDLE GetSceneSrvHandle() { return GetRenderTargetSrvHandle(RenderTargetType::Scene); }
	static D3D12_GPU_DESCRIPTOR_HANDLE GetEditorSceneSrvHandle() { return GetRenderTargetSrvHandle(RenderTargetType::EditorScene); }
	static D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferSrvHandle(GBufferType type) { return m_GBufferSrvHandles[static_cast<UINT>(type)]; }
	static D3D12_GPU_DESCRIPTOR_HANDLE GetAtmosphereSrvHandle() { return GetGBufferSrvHandle(GBufferType::ATMOSPHERE); }
	static ID3D12Resource* GetShadowDepthResource() { return m_ShadowDepthBuffer.Get(); }
	static D3D12_CPU_DESCRIPTOR_HANDLE GetImGuiCpuHandle();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetImGuiGpuHandle();
};
