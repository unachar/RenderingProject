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
	using RendererState::g_kCB_ALIGNED_SIZE;
	using RendererState::g_kMAX_DYNAMIC_VERTICES;
	using RendererState::g_kMAX_SRVS;
	using RendererState::m_DynamicVertexBuffer;
	using RendererState::m_DynamicVertexBufferView;
	using RendererState::m_pDynamicVertexDataBegin;
	using RendererState::m_DynamicVertexOffset;

	static void UpdateLightConstantBuffer(float deferredLightStrength);
	static void UpdateShadowConstantBuffer();
	static void CreateSpriteVertex(const VertexResource& vertexstruct);
	static void CreateObjectVertex(const VertexResource& vertexstruct);
	static void SetMaterial(const EntityID entityID, const MaterialComponent& material);
	static void BeginFrame();

	static ID3D12DescriptorHeap* GetCbvHeap() { return m_CbvHeap.Get(); }
	static UINT8* GetConstantBufferPtr() { return m_pCbvDataBegin; }
	static UINT GetCbvIncrementSize() { return m_CbvIncrementSize; }
	static ID3D12Resource* GetLightCB() { return m_LightConstantBuffer.Get(); }
	static ID3D12Resource* GetPBRCB() { return m_PBRConstantBuffer.Get(); }
};

