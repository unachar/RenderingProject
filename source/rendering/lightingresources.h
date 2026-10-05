#pragma once
#include "rendererstate.h"

class LightingResources : protected RendererState
{
public:
	struct LightGridStats
	{
		UINT AuthoredLights = 0;
		UINT ActivePhysicalLights = 0;
		UINT ActiveDecalLights = 0;
		UINT OnScreenLights = 0;
		UINT GpuVisibleLights = 0;
		UINT GpuPhysicalLights = 0;
		UINT GpuDecalLights = 0;
		UINT VolumetricLights = 0;
		UINT ShadowedLights = 0;
		UINT TileCountX = 0;
		UINT TileCountY = 0;
		UINT MaxLightsPerTile = 0;
		UINT OverflowedTileAssignments = 0;
	};
	static void UpdateLightConstantBuffer(float deferredLightStrength);
	static void UpdateShadowConstantBuffer();
	static UINT GetShadowLightCount();
	static bool ShouldRenderShadowPass(UINT shadowIndex);
	static bool ShouldDrawEntityInCurrentShadowPass(EntityID entity);
	static bool IsCurrentShadowPassVirtualPage();
	static UINT GetCurrentShadowLodBias();
	static bool IsVirtualShadowCacheHit();
	static bool GetShadowPassInfo(UINT shadowIndex, UINT& layer, D3D12_VIEWPORT& viewport, D3D12_RECT& scissor, bool& clearLayer);
	static void SetCurrentShadowPassIndex(UINT index);
	static XMMATRIX GetCurrentShadowViewProjection();
	static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentShadowConstantBufferAddress();
	static D3D12_GPU_VIRTUAL_ADDRESS GetShadowConstantBufferAddress(UINT shadowIndex);
	static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentLightConstantBufferAddress();
	static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentLightTileIndexBufferAddress();
	static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentVolumetricLightIndexBufferAddress();
	static const LightGridStats& GetLightGridStats();
	static void BeginFrame();
	static ID3D12Resource* GetLightCB() { return m_LightConstantBuffer.Get(); }
	static ID3D12Resource* GetShadowCB() { return m_ShadowConstantBuffer.Get(); }
};
