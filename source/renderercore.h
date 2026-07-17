#pragma once
#include "rendererstate.h"

class RendererCore : protected RendererState
{
public:
	static bool Init(HWND hwnd);
	static void Uninit();
	static void Resize(UINT width, UINT height);
	static void SetRenderMode(RenderMode mode);
	static void ApplyPendingRenderMode();
	static void SetHdr(bool enabled);
	static void ApplyPendingHdr();
	static void SetResolutionScale(float scale);
	static void ApplyPendingResolutionScale();
	static void InvalidateScenePipelineCache();

	static RenderMode GetRenderMode() { return m_RenderMode; }
	static RenderMode GetRequestedRenderMode() { return m_HasPendingRenderMode ? m_PendingRenderMode : m_RenderMode; }
	static UINT GetWidth() { return m_Width; }
	static UINT GetHeight() { return m_Height; }
	static UINT GetSceneWidth() { return m_SceneWidth; }
	static const XMFLOAT4X4& GetPreviousViewMatrix() { return m_PrevViewMatrix; }
	static const XMFLOAT4X4& GetPreviousProjectionMatrix() { return m_PrevProjMatrix; }
	static UINT GetSceneHeight() { return m_SceneHeight; }
	static float GetResolutionScale() { return m_HasPendingResolutionScale ? m_PendingResolutionScale : m_ResolutionScale; }
	static int GetMonitorTextureIndex() { return m_MonitorTextureSrvIndex; }
	static void SetMonitorTextureIndex(int srvIndex) { m_MonitorTextureSrvIndex = srvIndex; }
	static UINT GetFrameIndex() { return m_FrameIndex; }
	static float GetSceneAspectRatio()
	{
		float h = static_cast<float>(GetSceneHeight());
		return h <= 0.0f ? 1.0f : static_cast<float>(GetSceneWidth()) / h;
	}
	static ID3D12Device* GetDevice() { return m_Device.Get(); }
	static ID3D12GraphicsCommandList* GetCommandList() { return m_CommandList.Get(); }
	static bool CheckDeviceHealth(HRESULT operationResult, const char* operation);
	static bool WaitForGpuIdle();
	static ID3D12CommandQueue* GetCommandQueue() { return m_CommandQueue.Get(); }
};

