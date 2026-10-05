#include "pch.h"
#include "materialbindings.h"
#include "graphicsdevice.h"
#include "componentmanager.h"
#include "world.h"

namespace
{
	uint64_t HashGeometry(const void* data, size_t size)
	{
		uint64_t hash = 1469598103934665603ull;
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	struct MaterialPartShaderConstants
	{
		XMFLOAT4 Basic{};
		XMFLOAT4 Base{};
		XMFLOAT4 Shadow0{};
		XMFLOAT4 Shadow1{};
		XMFLOAT4 Highlight{};
		XMFLOAT4 RimStyle{};
		XMFLOAT4 RimLight{};
		XMFLOAT4 Skin0{};
		XMFLOAT4 Skin1{};
	};

	struct PBRConstants
	{
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		float Fresnel = 0.04f;
		float NormalBlend = 0.0f;
		float NormalBias = 0.0f;
		float BaseSaturation = 1.2f;
		float BaseBrightness = 1.0f;
		float ShadowThreshold = 0.44f;
		float ShadowSoftness = 0.045f;
		float ShadowStrength = 1.0f;
		float MidStrength = 1.0f;
		float LitStrength = 1.0f;
		float RimStrength = 0.45f;
		float RimThreshold = 0.70f;
		float RimSoftness = 0.055f;
		float RimPower = 1.0f;
		XMFLOAT3 RimColor = { 0.38f, 0.48f, 0.80f };
		float RimAlbedoBlend = 0.20f;
		float RimLightBlend = 0.35f;
		float SpecularStrength = 0.35f;
		float SpecularThreshold = 0.35f;
		float KawaiiBlend = 1.0f;
		float SkinScatterStrength = 1.0f;
		float SkinScatterWrap = 0.42f;
		float SkinBacklightStrength = 1.0f;
		float SkinRimScatterStrength = 1.0f;
		float SkinOilSpecularStrength = 1.0f;
		float SkinShadowScatter = 0.54f;
		float CastShadowThreshold = 0.28f;
		float CastShadowSoftness = 0.10f;
		float Padding[3]{};
		XMFLOAT4 Transparent0 = { 1.50f, 0.98f, 0.02f, 0.035f };
		XMFLOAT4 Transparent1 = { 0.10f, 0.02f, 0.01f, 0.005f };
		MaterialPartShaderConstants PartParams[kMaterialPartParamCount]{};
	};

	void WriteMaterialPartConstants(PBRConstants& constants, int index, const MaterialPartParams& params)
	{
		if (index < 0 || index >= kMaterialPartParamCount)
		{
			return;
		}

		constants.PartParams[index].Basic = XMFLOAT4(params.Metallic, params.Roughness, params.Fresnel, params.NormalBlend);
		constants.PartParams[index].Base = XMFLOAT4(params.NormalBias, params.BaseSaturation, params.BaseBrightness, params.KawaiiBlend);
		constants.PartParams[index].Shadow0 = XMFLOAT4(params.ShadowThreshold, params.ShadowSoftness, params.ShadowStrength, params.MidStrength);
		constants.PartParams[index].Shadow1 = XMFLOAT4(params.LitStrength, params.CastShadowThreshold, params.CastShadowSoftness, 0.0f);
		constants.PartParams[index].Highlight = XMFLOAT4(params.RimStrength, params.RimThreshold, params.SpecularStrength, params.SpecularThreshold);
		constants.PartParams[index].RimStyle = XMFLOAT4(params.RimSoftness, params.RimPower, params.RimAlbedoBlend, params.RimLightBlend);
		constants.PartParams[index].RimLight = XMFLOAT4(params.RimColor.x, params.RimColor.y, params.RimColor.z, 1.0f);
		constants.PartParams[index].Skin0 = XMFLOAT4(params.SkinScatterStrength, params.SkinScatterWrap, params.SkinBacklightStrength, params.SkinRimScatterStrength);
		constants.PartParams[index].Skin1 = XMFLOAT4(params.SkinOilSpecularStrength, params.SkinShadowScatter, 0.0f, 0.0f);
	}

	PBRConstants BuildPBRConstantsFromMaterial(const MaterialComponent& material)
	{
		PBRConstants constants{};
		constants.Metallic = material.Metallic;
		constants.Roughness = material.Roughness;
		constants.Fresnel = material.Fresnel;
		constants.NormalBlend = material.NormalBlend;
		constants.NormalBias = material.NormalBias;
		constants.BaseSaturation = material.BaseSaturation;
		constants.BaseBrightness = material.BaseBrightness;
		constants.ShadowThreshold = material.ShadowThreshold;
		constants.ShadowSoftness = material.ShadowSoftness;
		constants.ShadowStrength = material.ShadowStrength;
		constants.MidStrength = material.MidStrength;
		constants.LitStrength = material.LitStrength;
		constants.RimStrength = material.RimStrength;
		constants.RimThreshold = material.RimThreshold;
		constants.RimSoftness = material.RimSoftness;
		constants.RimPower = material.RimPower;
		constants.RimColor = material.RimColor;
		constants.RimAlbedoBlend = material.RimAlbedoBlend;
		constants.RimLightBlend = material.RimLightBlend;
		constants.SpecularStrength = material.SpecularStrength;
		constants.SpecularThreshold = material.SpecularThreshold;
		constants.KawaiiBlend = material.KawaiiBlend;
		constants.SkinScatterStrength = material.SkinScatterStrength;
		constants.SkinScatterWrap = material.SkinScatterWrap;
		constants.SkinBacklightStrength = material.SkinBacklightStrength;
		constants.SkinRimScatterStrength = material.SkinRimScatterStrength;
		constants.SkinOilSpecularStrength = material.SkinOilSpecularStrength;
		constants.SkinShadowScatter = material.SkinShadowScatter;
		constants.CastShadowThreshold = material.CastShadowThreshold;
		constants.CastShadowSoftness = material.CastShadowSoftness;
		constants.Transparent0 = XMFLOAT4(
			max(material.IOR, 1.0001f),
			clamp(material.Transmission, 0.0f, 1.0f),
			clamp(material.TransmissionRoughness, 0.0f, 1.0f),
			max(material.RefractionStrength, 0.0f));
		constants.Transparent1 = XMFLOAT4(
			max(material.Thickness, 0.0f),
			max(material.AbsorptionCoefficient.x, 0.0f),
			max(material.AbsorptionCoefficient.y, 0.0f),
			max(material.AbsorptionCoefficient.z, 0.0f));
		for (int i = 0; i < kMaterialPartParamCount; ++i)
		{
			WriteMaterialPartConstants(constants, i, material.PartParams[i]);
		}
		return constants;
	}

	const MaterialComponent& GetDeferredLightingMaterial()
	{
		static MaterialComponent defaultMaterial{};
		EntityID fallbackEntity = g_kINVALID_ENTITY;
		EntityID litEntity = g_kINVALID_ENTITY;
		EntityID selfShadowEntity = g_kINVALID_ENTITY;
		EntityID toonEntity = g_kINVALID_ENTITY;

		for (EntityID entity : World::GetView<MaterialComponent>())
		{
			const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
			if (material.ShaderClassMode != MaterialMode::Manual)
			{
				continue;
			}

			if (fallbackEntity == g_kINVALID_ENTITY)
			{
				fallbackEntity = entity;
			}
			if (material.ShaderClass == ShaderClass::Lit && litEntity == g_kINVALID_ENTITY)
			{
				litEntity = entity;
			}
			if (material.ShaderClass == ShaderClass::SelfShadow && selfShadowEntity == g_kINVALID_ENTITY)
			{
				selfShadowEntity = entity;
			}
			if (material.ShaderClass == ShaderClass::Toon && toonEntity == g_kINVALID_ENTITY)
			{
				toonEntity = entity;
			}
		}

		if (litEntity != g_kINVALID_ENTITY)
		{
			return ComponentManager::GetComponentUnchecked<MaterialComponent>(litEntity);
		}

		if (selfShadowEntity != g_kINVALID_ENTITY)
		{
			return ComponentManager::GetComponentUnchecked<MaterialComponent>(selfShadowEntity);
		}

		if (toonEntity != g_kINVALID_ENTITY)
		{
			return ComponentManager::GetComponentUnchecked<MaterialComponent>(toonEntity);
		}

		if (fallbackEntity != g_kINVALID_ENTITY)
		{
			return ComponentManager::GetComponentUnchecked<MaterialComponent>(fallbackEntity);
		}

		return defaultMaterial;
	}

}

D3D12_GPU_VIRTUAL_ADDRESS MaterialBindings::GetPBRConstantBufferAddress(UINT slot)
{
	if (!m_PBRConstantBuffer)
	{
		return 0;
	}
	UINT safeSlot;
	if (slot < g_kPBR_CB_SLOT_COUNT)
	{
		safeSlot = slot;
	}
	else
	{
		safeSlot = 0;
	}
	const UINT frameSlot = GraphicsDevice::GetFrameIndex() * g_kPBR_CB_SLOT_COUNT + safeSlot;
	return m_PBRConstantBuffer->GetGPUVirtualAddress() +
		frameSlot * g_kPBR_CB_ALIGNED_SIZE;
}

void MaterialBindings::SetMaterial(const EntityID entityID, const MaterialComponent& material)
{
	if (!m_pPBRCbvDataBegin || !m_PBRConstantBuffer || !m_CommandList)
	{
		return;
	}

	UINT slot;
	if (entityID < g_kMAX_ENTITIES)
	{
		slot = entityID + 1;
	}
	else
	{
		slot = 0;
	}
	PBRConstants constants = BuildPBRConstantsFromMaterial(material);

	const UINT frameSlot = GraphicsDevice::GetFrameIndex() * g_kPBR_CB_SLOT_COUNT + slot;
	auto* dst = static_cast<UINT8*>(m_pPBRCbvDataBegin) + frameSlot * g_kPBR_CB_ALIGNED_SIZE;
	memcpy(dst, &constants, sizeof(constants));
	m_CommandList->SetGraphicsRootConstantBufferView(3, GetPBRConstantBufferAddress(slot));
}

uint64_t MaterialBindings::GetMaterialBatchHash(const MaterialComponent& material)
{
	const PBRConstants constants = BuildPBRConstantsFromMaterial(material);
	uint64_t hash = HashGeometry(&constants, sizeof(constants));
	struct BatchMaterialState
	{
		int ShaderMode = 0;
		int ShaderClassValue = 0;
		float Alpha = 1.0f;
		UINT IsTransparent = 0;
		UINT UseTexture = 0;
		UINT UseNormalMap = 0;
	} state{};
	state.ShaderMode = static_cast<int>(material.ShaderClassMode);
	state.ShaderClassValue = static_cast<int>(material.ShaderClass);
	state.Alpha = material.Alpha;
	state.IsTransparent = static_cast<UINT>(material.IsTransparent);
	state.UseTexture = static_cast<UINT>(material.UseTexture);
	state.UseNormalMap = static_cast<UINT>(material.NormalMapID >= 0);
	const uint64_t stateHash = HashGeometry(&state, sizeof(state));

	hash ^= stateHash + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
	return hash;
}

void MaterialBindings::UpdateDeferredLightingMaterial()
{
	if (m_pPBRCbvDataBegin)
	{
		PBRConstants pbrConstants = BuildPBRConstantsFromMaterial(GetDeferredLightingMaterial());
		auto* pbrDst = static_cast<UINT8*>(m_pPBRCbvDataBegin) +
			GraphicsDevice::GetFrameIndex() * g_kPBR_CB_SLOT_COUNT * g_kPBR_CB_ALIGNED_SIZE;
		memcpy(pbrDst, &pbrConstants, sizeof(pbrConstants));
	}
}
