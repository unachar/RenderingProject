#pragma once
#include "rendererresource.h"

class RendererDraw : protected RendererState
{
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
	static void BeginBackBufferPass();
	static void BeginEditorSceneOverlayPass();
	static void EndEditorSceneOverlayPass();
	static void SetDescriptorHeap();
	static void EndDraw();
	static void BeginScenePass();
	static void ApplyPostProcess(const PostProcessComponent& config);
	static void ApplyAntiAliasing();
	static void EndScenePass();
	static void ResizeScene(UINT width, UINT height);

	static D3D12_GPU_DESCRIPTOR_HANDLE GetSceneSrvHandle() { return m_SceneSrvHandle; }
	static D3D12_GPU_DESCRIPTOR_HANDLE GetEditorSceneSrvHandle() { return m_EditorSceneSrvHandle; }
	static D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferSrvHandle(GBufferType type) { return m_GBufferSrvHandles[static_cast<UINT>(type)]; }
	static D3D12_GPU_DESCRIPTOR_HANDLE GetAtmosphereSrvHandle() { return m_AtmosphereSrvHandle; }
	static D3D12_CPU_DESCRIPTOR_HANDLE GetImGuiCpuHandle();
	static D3D12_GPU_DESCRIPTOR_HANDLE GetImGuiGpuHandle();
};

