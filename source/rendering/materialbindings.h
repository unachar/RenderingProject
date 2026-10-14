#pragma once
#include "rendererstate.h"
#include "gpuscene/gpuscenetypes.h"

class MaterialBindings : protected RendererState
{
public:
	static GPUMaterialData BuildGpuSceneMaterial(const MaterialComponent& material);
	static D3D12_GPU_VIRTUAL_ADDRESS GetPBRConstantBufferAddress(UINT slot = 0);
	static void SetMaterial(const EntityID entityID, const MaterialComponent& material);
    static bool SetPreviewMaterial(const MaterialComponent& material, ID3D12Resource* constants, UINT textureBase,
        UINT partIndex = 0, UINT partsPerFrame = 1);
    static void BindAssetSlot(EntityID entity, const MaterialComponent& material, int modelTexture = -1);
	static uint64_t GetMaterialBatchHash(const MaterialComponent& material);
	static ID3D12Resource* GetPBRCB() { return m_PBRConstantBuffer.Get(); }
	static void UpdateDeferredLightingMaterial();
};
