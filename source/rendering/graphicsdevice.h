#pragma once
#include "rendererstate.h"

class GraphicsDevice : protected RendererState
{
public:
	static bool Init(HWND hwnd);
	static void Uninit();

	static UINT GetWidth() { return m_Width; }
	static UINT GetHeight() { return m_Height; }
	static UINT GetSceneWidth() { return m_SceneWidth; }
	static const XMFLOAT4X4& GetPreviousViewMatrix() { return m_PrevViewMatrix; }
	static const XMFLOAT4X4& GetPreviousProjectionMatrix() { return m_PrevProjMatrix; }
	static UINT GetSceneHeight() { return m_SceneHeight; }
	static UINT GetFrameIndex() { return m_FrameIndex; }
	static float GetSceneAspectRatio()
	{
		float h = static_cast<float>(GetSceneHeight());
		if (h <= 0.0f)
		{
			return 1.0f;
		}
		return static_cast<float>(GetSceneWidth()) / h;
	}
	static ID3D12Device* GetDevice() { return m_Device.Get(); }
	static ID3D12GraphicsCommandList* GetCommandList() { return m_CommandList.Get(); }
	static bool CheckDeviceHealth(HRESULT operationResult, const char* operation);
	static bool WaitForGpuIdle();
	static ID3D12CommandQueue* GetCommandQueue() { return m_CommandQueue.Get(); }
};

