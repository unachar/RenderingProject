#pragma once
#include "rendererstate.h"

class MaterialBindings : protected RendererState
{
public:
	static D3D12_GPU_VIRTUAL_ADDRESS GetPBRConstantBufferAddress(UINT slot = 0);
	static void SetMaterial(const EntityID entityID, const MaterialComponent& material);
	static uint64_t GetMaterialBatchHash(const MaterialComponent& material);
	static ID3D12Resource* GetPBRCB() { return m_PBRConstantBuffer.Get(); }
	static void UpdateDeferredLightingMaterial();
};
