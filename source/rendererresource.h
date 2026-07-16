#pragma once
#include "rendererstate.h"

struct rendererResource
{
	// 描画関連
	const char* csoPath{};
	const char* vsPath{};
	const char* psPath{};
	const char* psMrtPath{};
	bool isModel{};
	ID3DBlob** ppBlob{};
	ComPtr<ID3DBlob> vsBlob;
	ComPtr<ID3DBlob> psBlob;
	ComPtr<ID3DBlob> csBlob;
	ID3D12RootSignature* rootSignature{};
	D3D_PRIMITIVE_TOPOLOGY topology{};
	bool frontCounterClockwise{};
	bool enableAlphaBlend{};

	// リソース関連
	HWND hwnd{};
	UINT width{};
	UINT height{};
	RenderMode renderMode{};
};


class RendererResource : protected RendererState
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
	using RendererState::g_kCB_ALIGNED_SIZE;
	using RendererState::g_kCBV_PER_FRAME_COUNT;
	using RendererState::g_kCBV_COUNT;
	using RendererState::g_kMAX_DYNAMIC_VERTICES;
	using RendererState::g_kMAX_SRVS;
	using RendererState::g_kTEXTURE_SRV_START_INDEX;
	using RendererState::g_kTRANSIENT_CB_SLOT_COUNT;
	using RendererState::g_kTRANSIENT_CB_START_INDEX;
	using RendererState::m_DynamicVertexBuffer;
	using RendererState::m_DynamicVertexBufferView;
	using RendererState::m_pDynamicVertexDataBegin;
	using RendererState::m_DynamicVertexOffset;

	static void UpdateLightConstantBuffer(float deferredLightStrength);
	static void UpdateShadowConstantBuffer();
	static UINT GetShadowLightCount();
	static bool ShouldRenderShadowPass(UINT shadowIndex);
	// Reject shadow casters that cannot overlap the active virtual page.  This is
	// Reject shadow casters that cannot overlap the active light/page frustum.
	static bool ShouldDrawEntityInCurrentShadowPass(EntityID entity);
	static bool IsCurrentShadowPassVirtualPage();
	// Coarser clipmap levels cannot represent the detail of LOD0 geometry.  Use
	// this as the minimum mesh LOD while drawing the active virtual page.
	static UINT GetCurrentShadowLodBias();
	static bool IsVirtualShadowCacheHit();
	static bool GetShadowPassInfo(UINT shadowIndex, UINT& layer, D3D12_VIEWPORT& viewport, D3D12_RECT& scissor, bool& clearLayer);
	static void SetCurrentShadowPassIndex(UINT index);
	static XMMATRIX GetCurrentShadowViewProjection();
	static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentShadowConstantBufferAddress();
	static D3D12_GPU_VIRTUAL_ADDRESS GetShadowConstantBufferAddress(UINT shadowIndex);
	static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentLightConstantBufferAddress();
	static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentLightTileIndexBufferAddress();
	static const LightGridStats& GetLightGridStats();
	static D3D12_GPU_VIRTUAL_ADDRESS GetPBRConstantBufferAddress(UINT slot = 0);
	static void CreateSpriteVertex(const VertexResource& vertexstruct);
	static void CreateObjectVertex(const VertexResource& vertexstruct);
	static void SetMaterial(const EntityID entityID, const MaterialComponent& material);
	static uint64_t GetMaterialBatchHash(const MaterialComponent& material);
	static D3D12_GPU_DESCRIPTOR_HANDLE AllocateTransientConstantBuffer(const ConstantBuffer3D& constants);
	static void BeginFrame();

	static ID3D12DescriptorHeap* GetCbvHeap() { return m_CbvHeap.Get(); }
	static UINT GetCurrentFrameCbvBaseIndex() { return m_FrameIndex * g_kCBV_PER_FRAME_COUNT; }
	static UINT GetCurrentFrameCbvIndex(UINT slot)
	{
		const UINT safeSlot = slot < g_kCBV_PER_FRAME_COUNT ? slot : g_kCBV_PER_FRAME_COUNT - 1;
		return GetCurrentFrameCbvBaseIndex() + safeSlot;
	}
	static D3D12_GPU_DESCRIPTOR_HANDLE GetConstantBufferHandle(UINT slot)
	{
		if (!m_CbvHeap)
		{
			return {};
		}
		return CD3DX12_GPU_DESCRIPTOR_HANDLE(
			m_CbvHeap->GetGPUDescriptorHandleForHeapStart(),
			GetCurrentFrameCbvIndex(slot),
			m_CbvIncrementSize);
	}
	static UINT8* GetConstantBufferPtr()
	{
		if (!m_pCbvDataBegin)
		{
			return nullptr;
		}
		return m_pCbvDataBegin + GetCurrentFrameCbvBaseIndex() * g_kCB_ALIGNED_SIZE;
	}
	static UINT GetCbvIncrementSize() { return m_CbvIncrementSize; }
	static ID3D12Resource* GetLightCB() { return m_LightConstantBuffer.Get(); }
	static ID3D12Resource* GetPBRCB() { return m_PBRConstantBuffer.Get(); }
	static ID3D12Resource* GetShadowCB() { return m_ShadowConstantBuffer.Get(); }
};
