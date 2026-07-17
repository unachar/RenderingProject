#include "pch.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "rendererutils.h"
#include "world.h"
#include "ecs.h"
#include "texturemanager.h"
#include "componentmanager.h"
#include "imguimanager.h"
#include "materialsystem.h"
#include "light.h"
#include "camera.h"
#include "atmosphere.h"
#include "renderersettings.h"
#include "localheightfog.h"
#include "lightinstancebuilder.h"
#include <limits>
#include <climits>

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

	void CalculateGeometryBounds(const vector<Vertex>& vertices, XMFLOAT3& center, XMFLOAT3& extents)
	{
		if (vertices.empty())
		{
			center = {};
			extents = {};
			return;
		}
		XMFLOAT3 minimum = vertices.front().Pos;
		XMFLOAT3 maximum = vertices.front().Pos;
		for (const Vertex& vertex : vertices)
		{
			minimum.x = min(minimum.x, vertex.Pos.x);
			minimum.y = min(minimum.y, vertex.Pos.y);
			minimum.z = min(minimum.z, vertex.Pos.z);
			maximum.x = max(maximum.x, vertex.Pos.x);
			maximum.y = max(maximum.y, vertex.Pos.y);
			maximum.z = max(maximum.z, vertex.Pos.z);
		}
		center = { (minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f, (minimum.z + maximum.z) * 0.5f };
		extents = { (maximum.x - minimum.x) * 0.5f, (maximum.y - minimum.y) * 0.5f, (maximum.z - minimum.z) * 0.5f };
	}

	struct RuntimeLightState
	{
		LightComponent Component{};
		XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
		bool HasLight = false;
	};

	RuntimeLightState g_CachedDirectionalLight{};
	RuntimeLightState g_CachedAnyLight{};
	RuntimeLightState g_CachedShadowLight{};
	struct ShadowRenderPass
	{
		XMMATRIX ViewProjection = XMMatrixIdentity();
		XMFLOAT4 Params = {};
		UINT Layer = 0;
		UINT X = 0;
		UINT Y = 0;
		UINT Size = RendererState::g_kSHADOW_MAP_SIZE;
		UINT VirtualLevel = 0;
		bool ClearLayer = true;
		bool VirtualPage = false;
		bool NeedsRender = true;
	};
	XMMATRIX g_ShadowLightViewProjections[RendererState::g_kMAX_SHADOW_LIGHTS]{};
	XMFLOAT4 g_ShadowMapParams[RendererState::g_kMAX_SHADOW_LIGHTS]{};
	EntityID g_ShadowLightEntities[RendererState::g_kMAX_SHADOW_LIGHTS]{};
	UINT g_ShadowLightCount = 0;
	ShadowRenderPass g_ShadowRenderPasses[RendererState::g_kMAX_SHADOW_PASSES]{};
	UINT g_ShadowRenderPassCount = 0;
	EntityID g_VirtualShadowLightEntity = g_kINVALID_ENTITY;
	XMMATRIX g_VirtualShadowViewProjections[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};
	XMFLOAT4 g_VirtualShadowParams[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};
	XMFLOAT4 g_VirtualShadowPageOrigins[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};
	uint32_t g_VirtualShadowResidencyRows[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]
		[RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION]{};
	struct VirtualShadowPhysicalPageCache
	{
		int GlobalPageX = INT_MIN;
		int GlobalPageY = INT_MIN;
		uint64_t ContentKey = 0;
		bool Valid = false;
	};
	VirtualShadowPhysicalPageCache g_VirtualShadowPageCache
		[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]
		[RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION]
		[RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION]{};
	UINT g_VirtualShadowLevelCount = 0;
	float g_DirectionalShadowMode = 0.0f;
	uint64_t g_PreviousVirtualShadowCacheKey = 0;
	bool g_VirtualShadowCacheHit = false;
	UINT g_CurrentShadowPassIndex = 0;
	bool g_LightCacheValid = false;
	uint64_t g_FrameSerial = 0;
	uint64_t g_LightConstantsSerial = 0;
	uint64_t g_ShadowConstantsSerial = 0;
	UINT g_ShadowConstantsPassIndex = UINT_MAX;
	float g_LightConstantsStrength = -1.0f;
	ComPtr<ID3D12Resource> g_LightTileIndexBuffers[RendererState::g_kFRAME_COUNT];
	uint32_t* g_LightTileIndexData[RendererState::g_kFRAME_COUNT]{};
	RendererResource::LightGridStats g_LightGridStats{};

	bool EnsureLightTileIndexBuffers(ID3D12Device* device)
	{
		if (!device)
		{
			return false;
		}
		const UINT64 bufferSize =
			static_cast<UINT64>(RendererState::g_kMAX_LIGHT_TILE_COUNT) *
			RendererState::g_kMAX_LIGHTS_PER_TILE * sizeof(uint32_t);
		for (UINT frame = 0; frame < RendererState::g_kFRAME_COUNT; ++frame)
		{
			if (g_LightTileIndexBuffers[frame] && g_LightTileIndexData[frame])
			{
				continue;
			}
			auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			auto bufferDescription = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
			if (FAILED(device->CreateCommittedResource(
				&heapProperties,
				D3D12_HEAP_FLAG_NONE,
				&bufferDescription,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&g_LightTileIndexBuffers[frame]))))
			{
				return false;
			}
			g_LightTileIndexBuffers[frame]->SetName(L"LightTileIndexBuffer");
			if (FAILED(g_LightTileIndexBuffers[frame]->Map(
				0, nullptr, reinterpret_cast<void**>(&g_LightTileIndexData[frame]))))
			{
				g_LightTileIndexBuffers[frame].Reset();
				return false;
			}
		}
		return true;
	}

	struct LightConstants
	{
		XMFLOAT4 LightDirection = { 0.0f, 1.0f, 0.0f, 0.0f };
		XMFLOAT4 LightColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		XMFLOAT4 LightPositionType = { 0.0f, 0.0f, 0.0f, 0.0f };
		XMFLOAT4 LightExtra = { 0.95f, 0.85f, 0.35f, 0.0f };
		XMFLOAT4 LightCount = { 0.0f, 0.0f, 0.0f, 0.0f };
		XMFLOAT4 LightDirections[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMFLOAT4 LightColors[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMFLOAT4 LightPositionTypes[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMFLOAT4 LightExtras[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMMATRIX LightViewProjections[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMFLOAT4 LightShadowData[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMFLOAT4 LightFlags[RendererState::g_kMAX_SHADER_LIGHTS]{};
		XMMATRIX VirtualShadowViewProjections[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};
		XMFLOAT4 VirtualShadowParams[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};
		XMFLOAT4 VirtualShadowPageOrigins[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};
		XMUINT4 VirtualShadowResidency[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS * 4]{};
		XMFLOAT4 VirtualShadowGlobal = { 0.0f, 0.0f, 1.0f, 0.20f };
		XMFLOAT4 ShadowRuntimeGlobal = { 1.0f, 0.8f, 8.0f, 0.0f };
		XMFLOAT4 ShadowDebugGlobal = { 0.0f, 0.0f, 128.0f, 16.0f };
		XMFLOAT4 DistanceFieldData0[RendererState::g_kMAX_DISTANCE_FIELD_SHADOW_OBJECTS]{};
		XMFLOAT4 DistanceFieldData1[RendererState::g_kMAX_DISTANCE_FIELD_SHADOW_OBJECTS]{};
		XMFLOAT4 DistanceFieldGlobal = { 0.0f, 30.0f, 12.0f, 0.0f };
		XMFLOAT4 LocalFogData0[RendererState::g_kMAX_LOCAL_HEIGHT_FOG_VOLUMES]{};
		XMFLOAT4 LocalFogData1[RendererState::g_kMAX_LOCAL_HEIGHT_FOG_VOLUMES]{};
		XMFLOAT4 LocalFogColors[RendererState::g_kMAX_LOCAL_HEIGHT_FOG_VOLUMES]{};
		XMFLOAT4 LocalFogGlobal = { 0.0f, 0.0f, 0.0f, 0.0f };
		XMFLOAT4 AtmosphereParams0 = { 1.0f, 0.42f, 0.075f, 0.36f };
		XMFLOAT4 AtmosphereParams1 = { 0.18f, 0.22f, 0.76f, 0.030f };
		XMFLOAT4 AtmosphereColor0 = { 0.46f, 0.62f, 1.0f, 0.55f };
		XMFLOAT4 AtmosphereColor1 = { 1.0f, 0.82f, 0.56f, 0.035f };
		XMFLOAT4 AtmosphereCamera = { 0.0f, 0.0f, 0.0f, 0.0f };
	};
	static_assert(
		sizeof(LightConstants) <= RendererState::g_kLIGHT_CB_ALIGNED_SIZE,
		"Light constant buffer size accounting must match the CPU structure");

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

	struct ShadowConstants
	{
		XMMATRIX LightViewProjection{};
		XMFLOAT4 ShadowMapParams = { 1.0f / RendererState::g_kSHADOW_MAP_SIZE, 0.0015f, 0.0025f, 1.0f };
		XMFLOAT4 ShadowFilterParams = { 1.0f, 0.0f, 0.0f, 0.0f };
	};

	UINT GetShadowConstantBufferSlot(UINT shadowIndex)
	{
		const UINT safeShadowIndex = min(shadowIndex, RendererState::g_kMAX_SHADOW_PASSES - 1);
		return RendererCore::GetFrameIndex() * RendererState::g_kMAX_SHADOW_PASSES + safeShadowIndex;
	}

	void BuildShadowViewProjection(const RuntimeLightState& runtimeLight, XMMATRIX& outLightViewProjection, XMFLOAT4& outShadowMapParams);
	void BuildVirtualDirectionalShadowViewProjection(const RuntimeLightState& runtimeLight, UINT level, XMMATRIX& outLightViewProjection, XMFLOAT4& outShadowMapParams);
	bool IsShadowBoundsEntity(EntityID entity);
	bool IsShadowCasterVisible(EntityID entity, const XMMATRIX& transposedViewProjection);

	XMFLOAT3 NormalizeFloat3(const XMFLOAT3& value, const XMFLOAT3& fallback)
	{
		XMVECTOR v = XMLoadFloat3(&value);
		if (XMVectorGetX(XMVector3LengthSq(v)) <= 0.000001f) return fallback;
		XMFLOAT3 result{};
		XMStoreFloat3(&result, XMVector3Normalize(v));
		return result;
	}

	XMFLOAT3 GetActiveCameraPosition()
	{
		const EntityID cameraEntity = Camera::GetCameraEntity();
		if (cameraEntity != g_kINVALID_ENTITY &&
			Registry::IsAlive(cameraEntity) &&
			ComponentManager::HasComponent<TransformComponent>(cameraEntity))
		{
			return ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
		}
		return { 0.0f, 0.0f, -5.0f };
	}

	void WriteAtmosphereConstants(LightConstants& constants)
	{
		const AtmosphereParameters& atmosphere = Atmosphere::GetParameters();
		constants.AtmosphereParams0 = XMFLOAT4(
			atmosphere.Enabled ? 1.0f : 0.0f,
			max(0.0f, atmosphere.RayleighStrength),
			max(0.0f, atmosphere.MieStrength),
			max(0.0f, atmosphere.Density));
		constants.AtmosphereParams1 = XMFLOAT4(
			max(0.0f, atmosphere.HeightFalloff),
			max(0.0f, atmosphere.Extinction),
			clamp(atmosphere.MieG, -0.95f, 0.95f),
			max(0.0001f, atmosphere.DistanceScale));
		constants.AtmosphereColor0 = XMFLOAT4(
			atmosphere.RayleighColor.x,
			atmosphere.RayleighColor.y,
			atmosphere.RayleighColor.z,
			max(0.0f, atmosphere.LightShaftStrength));
		constants.AtmosphereColor1 = XMFLOAT4(
			atmosphere.MieColor.x,
			atmosphere.MieColor.y,
			atmosphere.MieColor.z,
			max(0.0f, atmosphere.AmbientStrength));

		const XMFLOAT3 cameraPosition = GetActiveCameraPosition();
		constants.AtmosphereCamera = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, max(0.0f, min(1.0f, atmosphere.LightShaftBlur)));
	}

	void RebuildLightCache()
	{
		g_CachedDirectionalLight = {};
		g_CachedAnyLight = {};
		g_CachedShadowLight = {};
		g_ShadowLightCount = 0;
		g_ShadowRenderPassCount = 0;
		g_VirtualShadowLightEntity = g_kINVALID_ENTITY;
		g_VirtualShadowLevelCount = 0;
		memset(g_VirtualShadowResidencyRows, 0, sizeof(g_VirtualShadowResidencyRows));
		g_DirectionalShadowMode = 0.0f;
		g_CurrentShadowPassIndex = 0;
		for (UINT i = 0; i < RendererState::g_kMAX_SHADOW_LIGHTS; ++i)
		{
			g_ShadowLightEntities[i] = g_kINVALID_ENTITY;
			g_ShadowLightViewProjections[i] = XMMatrixIdentity();
			g_ShadowMapParams[i] = XMFLOAT4(
				1.0f / static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE),
				0.000008f,
				0.00001f,
				0.0f);
		}
		for (UINT i = 0; i < RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS; ++i)
		{
			g_VirtualShadowViewProjections[i] = XMMatrixIdentity();
			g_VirtualShadowParams[i] = XMFLOAT4(-1.0f, 1.0f / RendererState::g_kSHADOW_MAP_SIZE, 0.0f, 0.0f);
			g_VirtualShadowPageOrigins[i] = XMFLOAT4(0.0f, 0.0f,
				static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION), 0.0f);
		}

		for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
		{
			const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
			const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
			if (!light.IsActive || light.RenderMode != LightRenderMode::Physical)
			{
				continue;
			}

			if (!g_CachedAnyLight.HasLight)
			{
				g_CachedAnyLight.Component = light;
				g_CachedAnyLight.Position = transform.Position;
				g_CachedAnyLight.HasLight = true;
			}
			if (light.Type != LightType::Directional && !g_CachedShadowLight.HasLight)
			{
				g_CachedShadowLight.Component = light;
				g_CachedShadowLight.Position = transform.Position;
				g_CachedShadowLight.HasLight = true;
			}
			const UINT shadowLightBudget = min(
				static_cast<UINT>(RendererSettings::GetShadowLightBudget()),
				RendererState::g_kMAX_SHADOW_LIGHTS);
			if (light.CastShadow && g_ShadowLightCount < shadowLightBudget)
			{
				RuntimeLightState shadowLight{};
				shadowLight.Component = light;
				shadowLight.Position = transform.Position;
				shadowLight.HasLight = true;
				const bool useMultiLevelDirectional =
					light.Type == LightType::Directional &&
					g_VirtualShadowLightEntity == g_kINVALID_ENTITY;
				if (useMultiLevelDirectional)
				{
					g_VirtualShadowLightEntity = entity;
					const bool virtualMode = RendererSettings::GetShadowMapMethod() == ShadowMapMethod::VirtualShadowMap;
					g_DirectionalShadowMode = virtualMode ? 1.0f : 2.0f;
					const UINT requestedLevels = static_cast<UINT>(virtualMode
						? RendererSettings::GetVirtualClipmapLevels()
						: RendererSettings::GetShadowCascadeCount());
					g_VirtualShadowLevelCount = min(requestedLevels, shadowLightBudget - g_ShadowLightCount);
					for (UINT level = 0; level < g_VirtualShadowLevelCount; ++level)
					{
						const UINT viewIndex = g_ShadowLightCount++;
						g_ShadowLightEntities[viewIndex] = entity;
						BuildVirtualDirectionalShadowViewProjection(
							shadowLight, virtualMode ? level : level + 4,
							g_ShadowLightViewProjections[viewIndex],
							g_ShadowMapParams[viewIndex]);
						g_VirtualShadowViewProjections[level] = g_ShadowLightViewProjections[viewIndex];
						g_VirtualShadowParams[level] = XMFLOAT4(
							static_cast<float>(viewIndex),
							g_ShadowMapParams[viewIndex].x,
							g_ShadowMapParams[viewIndex].y,
							g_ShadowMapParams[viewIndex].z);
					}
				}
				else
				{
					g_ShadowLightEntities[g_ShadowLightCount] = entity;
					BuildShadowViewProjection(
						shadowLight,
						g_ShadowLightViewProjections[g_ShadowLightCount],
						g_ShadowMapParams[g_ShadowLightCount]);
					++g_ShadowLightCount;
				}
			}
			if (light.Type == LightType::Directional && !g_CachedDirectionalLight.HasLight)
			{
				g_CachedDirectionalLight.Component = light;
				g_CachedDirectionalLight.Position = transform.Position;
				g_CachedDirectionalLight.HasLight = true;
			}
		}
		if (!g_CachedShadowLight.HasLight)
		{
			g_CachedShadowLight = g_CachedDirectionalLight.HasLight ? g_CachedDirectionalLight : g_CachedAnyLight;
		}

		uint64_t virtualSceneKey = 1469598103934665603ull;
		auto hashVirtualBytes = [&](const void* data, size_t size)
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t byteIndex = 0; byteIndex < size; ++byteIndex)
			{
				virtualSceneKey ^= bytes[byteIndex];
				virtualSceneKey *= 1099511628211ull;
			}
		};
		const uint64_t settingsRevision = RendererSettings::GetRevision();
		hashVirtualBytes(&settingsRevision, sizeof(settingsRevision));
		if (g_CachedDirectionalLight.HasLight)
		{
			hashVirtualBytes(
				&g_CachedDirectionalLight.Component.Direction,
				sizeof(g_CachedDirectionalLight.Component.Direction));
		}

		if (g_DirectionalShadowMode != 1.0f)
		{
			memset(g_VirtualShadowPageCache, 0, sizeof(g_VirtualShadowPageCache));
		}
		g_VirtualShadowCacheHit = g_DirectionalShadowMode == 1.0f;

		for (UINT layer = 0;
			layer < g_ShadowLightCount && g_ShadowRenderPassCount < RendererState::g_kMAX_SHADOW_PASSES;
			++layer)
		{
			const bool virtualLayer =
				g_DirectionalShadowMode == 1.0f &&
				g_ShadowLightEntities[layer] == g_VirtualShadowLightEntity;
			if (!virtualLayer)
			{
				ShadowRenderPass& pass = g_ShadowRenderPasses[g_ShadowRenderPassCount++];
				pass.ViewProjection = g_ShadowLightViewProjections[layer];
				pass.Params = g_ShadowMapParams[layer];
				pass.Layer = layer;
				pass.X = 0;
				pass.Y = 0;
				pass.Size = RendererState::g_kSHADOW_MAP_SIZE;
				pass.VirtualLevel = 0;
				pass.ClearLayer = true;
				pass.VirtualPage = false;
				pass.NeedsRender = true;
				continue;
			}

			UINT virtualLevel = 0;
			for (UINT level = 0; level < g_VirtualShadowLevelCount; ++level)
			{
				if (static_cast<UINT>(max(g_VirtualShadowParams[level].x, 0.0f)) == layer)
				{
					virtualLevel = level;
					break;
				}
			}

			const UINT pageGrid = RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION;
			const UINT residentGrid = min(
				RendererState::g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION,
				pageGrid);
			const UINT firstPage = (pageGrid - residentGrid) / 2;
			const int pageOriginX = static_cast<int>(roundf(g_VirtualShadowPageOrigins[virtualLevel].x));
			const int pageOriginY = static_cast<int>(roundf(g_VirtualShadowPageOrigins[virtualLevel].y));
			const XMMATRIX fullViewProjection = XMMatrixTranspose(g_ShadowLightViewProjections[layer]);
			auto positiveModulo = [](int value, int modulus)
			{
				const int result = value % modulus;
				return result < 0 ? result + modulus : result;
			};

			for (UINT localPageY = 0; localPageY < residentGrid; ++localPageY)
			{
				for (UINT localPageX = 0; localPageX < residentGrid; ++localPageX)
				{
					if (g_ShadowRenderPassCount >= RendererState::g_kMAX_SHADOW_PASSES) break;
					const UINT pageX = firstPage + localPageX;
					const UINT pageY = firstPage + localPageY;
					g_VirtualShadowResidencyRows[virtualLevel][pageY] |= (1u << pageX);

					const int globalPageX = pageOriginX + static_cast<int>(pageX);
					const int globalPageY = pageOriginY + static_cast<int>(pageY);
					const UINT physicalPageX = static_cast<UINT>(positiveModulo(globalPageX, static_cast<int>(pageGrid)));
					const UINT physicalPageY = static_cast<UINT>(positiveModulo(globalPageY, static_cast<int>(pageGrid)));

					const float centerX = -1.0f +
						(2.0f * static_cast<float>(pageX) + 1.0f) / static_cast<float>(pageGrid);
					const float centerY = 1.0f -
						(2.0f * static_cast<float>(pageY) + 1.0f) / static_cast<float>(pageGrid);
					const XMMATRIX crop =
						XMMatrixTranslation(-centerX, -centerY, 0.0f) *
						XMMatrixScaling(static_cast<float>(pageGrid), static_cast<float>(pageGrid), 1.0f);
					const XMMATRIX pageViewProjection = XMMatrixTranspose(fullViewProjection * crop);

					uint64_t pageContentKey = virtualSceneKey;
					auto hashPageValue = [&](const void* data, size_t size)
					{
						const auto* bytes = static_cast<const uint8_t*>(data);
						for (size_t byteIndex = 0; byteIndex < size; ++byteIndex)
						{
							pageContentKey ^= bytes[byteIndex];
							pageContentKey *= 1099511628211ull;
						}
					};
					hashPageValue(&virtualLevel, sizeof(virtualLevel));
					hashPageValue(&globalPageX, sizeof(globalPageX));
					hashPageValue(&globalPageY, sizeof(globalPageY));
					// A moving caster invalidates only pages it can actually touch.  The
					// previous implementation mixed every transform into every page key,
					// turning one animated character into a full 64-page redraw.
					for (EntityID entity : World::GetView<TransformComponent>())
					{
						if (!IsShadowBoundsEntity(entity) ||
							!IsShadowCasterVisible(entity, pageViewProjection))
						{
							continue;
						}
						const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
						hashPageValue(&entity, sizeof(entity));
						hashPageValue(&transform.Position, sizeof(transform.Position));
						hashPageValue(&transform.Rotation, sizeof(transform.Rotation));
						hashPageValue(&transform.Scale, sizeof(transform.Scale));
						if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
						{
							const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
							hashPageValue(&animation.CurrentTime, sizeof(animation.CurrentTime));
							hashPageValue(&animation.BlendRate, sizeof(animation.BlendRate));
						}
					}

					auto& cacheEntry = g_VirtualShadowPageCache[virtualLevel][physicalPageY][physicalPageX];
					const bool cacheEnabled = RendererSettings::GetCacheVirtualShadowPages();
					const bool mappingMatches =
						cacheEntry.Valid &&
						cacheEntry.GlobalPageX == globalPageX &&
						cacheEntry.GlobalPageY == globalPageY;
					const bool contentMatches =
						mappingMatches && cacheEntry.ContentKey == pageContentKey;
					const UINT updatePeriod = 1u << virtualLevel;
					const UINT updatePhase =
						(physicalPageX + physicalPageY * pageGrid) % updatePeriod;
					const bool contentUpdateDue =
						updatePeriod == 1u ||
						((g_FrameSerial + updatePhase) % updatePeriod) == 0u;
					const bool needsRender =
						!cacheEnabled ||
						!mappingMatches ||
						(!contentMatches && contentUpdateDue);

					ShadowRenderPass& pass = g_ShadowRenderPasses[g_ShadowRenderPassCount++];
					pass.ViewProjection = pageViewProjection;
					pass.Params = g_ShadowMapParams[layer];
					pass.Layer = layer;
					pass.X = physicalPageX * RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE;
					pass.Y = physicalPageY * RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE;
					pass.Size = RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE;
					pass.VirtualLevel = virtualLevel;
					pass.ClearLayer = false;
					pass.VirtualPage = true;
					pass.NeedsRender = needsRender;

					if (needsRender)
					{
						g_VirtualShadowCacheHit = false;
						cacheEntry.GlobalPageX = globalPageX;
						cacheEntry.GlobalPageY = globalPageY;
						cacheEntry.ContentKey = pageContentKey;
						cacheEntry.Valid = true;
					}
				}
			}
		}
		g_LightCacheValid = true;
	}

	int FindShadowLightIndex(EntityID entity)
	{
		for (UINT i = 0; i < g_ShadowLightCount; ++i)
		{
			if (g_ShadowLightEntities[i] == entity)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	const RuntimeLightState& GetCachedLightState(bool preferDirectional)
	{
		if (!g_LightCacheValid)
		{
			RebuildLightCache();
		}
		return preferDirectional ? g_CachedDirectionalLight : g_CachedAnyLight;
	}

	const RuntimeLightState& GetCachedShadowLightState()
	{
		if (!g_LightCacheValid)
		{
			RebuildLightCache();
		}
		return g_CachedShadowLight;
	}

	bool IsSkyEntity(EntityID entity)
	{
		return ComponentManager::HasComponent<NameComponent>(entity) &&
			ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky";
	}

	bool IsShadowBoundsEntity(EntityID entity)
	{
		if (!Registry::IsAlive(entity) ||
			!ComponentManager::HasComponent<TransformComponent>(entity) ||
			ComponentManager::HasComponent<LightComponent>(entity) ||
			IsSkyEntity(entity))
		{
			return false;
		}

		bool renderable =
			ComponentManager::HasComponent<MeshComponent>(entity) ||
			ComponentManager::HasComponent<StaticModelComponent>(entity) ||
			ComponentManager::HasComponent<AnimationModelComponent>(entity);

		if (ComponentManager::HasComponent<SpriteComponent>(entity))
		{
			const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
			renderable |= sprite.Is3D;
		}

		if (!renderable)
		{
			return false;
		}

		if (ComponentManager::HasComponent<MaterialComponent>(entity))
		{
			const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
			if (MaterialSystem::IsTransparentMaterial(material) ||
				(material.ShaderClassMode == MaterialMode::Manual && material.ShaderClass == ShaderClass::Shadow))
			{
				return false;
			}
		}

		return true;
	}

	void IncludeBoundsPoint(XMVECTOR point, XMVECTOR& boundsMin, XMVECTOR& boundsMax, bool& hasBounds)
	{
		if (!hasBounds)
		{
			boundsMin = point;
			boundsMax = point;
			hasBounds = true;
			return;
		}

		boundsMin = XMVectorMin(boundsMin, point);
		boundsMax = XMVectorMax(boundsMax, point);
	}

	void IncludeBoundsSphere(XMVECTOR center, float radius, XMVECTOR& boundsMin, XMVECTOR& boundsMax, bool& hasBounds)
	{
		const XMVECTOR extent = XMVectorReplicate(max(radius, 0.01f));
		IncludeBoundsPoint(XMVectorSubtract(center, extent), boundsMin, boundsMax, hasBounds);
		IncludeBoundsPoint(XMVectorAdd(center, extent), boundsMin, boundsMax, hasBounds);
	}

	bool IsShadowCasterVisible(EntityID entity, const XMMATRIX& transposedViewProjection)
	{
		if (!IsShadowBoundsEntity(entity)) return false;

		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		XMFLOAT3 center = transform.Position;
		float radius = max(max(fabsf(transform.Scale.x), fabsf(transform.Scale.y)),
			max(fabsf(transform.Scale.z), 1.0f)) * 1.5f;
		if (ComponentManager::HasComponent<AABBComponent>(entity))
		{
			const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
			center.x += bounds.Center.x * transform.Scale.x;
			center.y += bounds.Center.y * transform.Scale.y;
			center.z += bounds.Center.z * transform.Scale.z;
			const XMFLOAT3 extents = {
				fabsf(bounds.Extents.x * transform.Scale.x),
				fabsf(bounds.Extents.y * transform.Scale.y),
				fabsf(bounds.Extents.z * transform.Scale.z) };
			radius = max(sqrtf(extents.x * extents.x + extents.y * extents.y + extents.z * extents.z), 0.05f);
		}

		const XMMATRIX viewProjection = XMMatrixTranspose(transposedViewProjection);
		const XMVECTOR worldCenter = XMLoadFloat3(&center);
		const XMVECTOR projectedCenter = XMVector3TransformCoord(worldCenter, viewProjection);
		const float centerX = XMVectorGetX(projectedCenter);
		const float centerY = XMVectorGetY(projectedCenter);
		const float centerZ = XMVectorGetZ(projectedCenter);

		// Project three world-space radius vectors and sum their absolute
		// contributions.  This slightly overestimates rotated AABBs, which is the
		// desired failure mode for shadow culling.
		float ndcRadiusX = 0.0f;
		float ndcRadiusY = 0.0f;
		float ndcRadiusZ = 0.0f;
		const XMVECTOR axes[3] = {
			XMVectorSet(radius, 0.0f, 0.0f, 0.0f),
			XMVectorSet(0.0f, radius, 0.0f, 0.0f),
			XMVectorSet(0.0f, 0.0f, radius, 0.0f) };
		for (const XMVECTOR axis : axes)
		{
			const XMVECTOR projected = XMVector3TransformCoord(XMVectorAdd(worldCenter, axis), viewProjection);
			ndcRadiusX += fabsf(XMVectorGetX(projected) - centerX);
			ndcRadiusY += fabsf(XMVectorGetY(projected) - centerY);
			ndcRadiusZ += fabsf(XMVectorGetZ(projected) - centerZ);
		}

		return centerX + ndcRadiusX >= -1.0f && centerX - ndcRadiusX <= 1.0f &&
			centerY + ndcRadiusY >= -1.0f && centerY - ndcRadiusY <= 1.0f &&
			centerZ + ndcRadiusZ >= 0.0f && centerZ - ndcRadiusZ <= 1.0f;
	}

	void BuildShadowFocusBounds(XMVECTOR& outCenter, float& outRadius)
	{
		bool hasBounds = false;
		XMVECTOR boundsMin = XMVectorReplicate(std::numeric_limits<float>::max());
		XMVECTOR boundsMax = XMVectorReplicate(-std::numeric_limits<float>::max());

		for (EntityID entity : World::GetView<TransformComponent>())
		{
			if (!IsShadowBoundsEntity(entity))
			{
				continue;
			}

			const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
			const XMVECTOR position = XMLoadFloat3(&transform.Position);
			const XMFLOAT3 absScale =
			{
				fabsf(transform.Scale.x),
				fabsf(transform.Scale.y),
				fabsf(transform.Scale.z)
			};

			if (ComponentManager::HasComponent<AABBComponent>(entity))
			{
				const auto& aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
				XMFLOAT3 scaledCenter =
				{
					aabb.Center.x * transform.Scale.x,
					aabb.Center.y * transform.Scale.y,
					aabb.Center.z * transform.Scale.z
				};
				XMFLOAT3 scaledExtents =
				{
					max(fabsf(aabb.Extents.x * absScale.x), 0.05f),
					max(fabsf(aabb.Extents.y * absScale.y), 0.05f),
					max(fabsf(aabb.Extents.z * absScale.z), 0.05f)
				};

				const XMVECTOR center = XMVectorAdd(position, XMLoadFloat3(&scaledCenter));
				const XMVECTOR extents = XMLoadFloat3(&scaledExtents);
				IncludeBoundsPoint(XMVectorSubtract(center, extents), boundsMin, boundsMax, hasBounds);
				IncludeBoundsPoint(XMVectorAdd(center, extents), boundsMin, boundsMax, hasBounds);
			}
			else
			{
				const float radius = max(max(absScale.x, absScale.y), max(absScale.z, 1.0f)) * 1.5f;
				IncludeBoundsSphere(position, radius, boundsMin, boundsMax, hasBounds);
			}
		}

		if (!hasBounds)
		{
			outCenter = XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f);
			outRadius = 12.0f;
			return;
		}

		outCenter = XMVectorScale(XMVectorAdd(boundsMin, boundsMax), 0.5f);
		outCenter = XMVectorSetW(outCenter, 1.0f);
		outRadius = XMVectorGetX(XMVector3Length(XMVectorSubtract(boundsMax, boundsMin))) * 0.5f;
		outRadius = clamp(outRadius, 6.0f, 80.0f);
	}

	void BuildShadowViewProjection(const RuntimeLightState& runtimeLight, XMMATRIX& outLightViewProjection, XMFLOAT4& outShadowMapParams)
	{
		XMFLOAT3 lightDirection = runtimeLight.HasLight ? runtimeLight.Component.Direction : XMFLOAT3(0.25f, 1.0f, -0.25f);
		XMVECTOR dir = XMVectorSet(lightDirection.x, lightDirection.y, lightDirection.z, 0.0f);
		if (XMVectorGetX(XMVector3LengthSq(dir)) < 0.000001f)
		{
			dir = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
		}
		dir = XMVector3Normalize(dir);

		XMVECTOR target = XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f);
		float focusRadius = 12.0f;
		BuildShadowFocusBounds(target, focusRadius);

		const LightType lightType = runtimeLight.HasLight ? runtimeLight.Component.Type : LightType::Directional;
		const bool cylinderVolume =
			runtimeLight.HasLight &&
			lightType == LightType::Volume &&
			runtimeLight.Component.VolumeShape == 1;
		XMVECTOR lightPos = XMVectorAdd(target, XMVectorScale(dir, max(45.0f, focusRadius * 2.5f)));
		float nearClip = 0.1f;
		float farClip = max(120.0f, focusRadius * 6.0f + 40.0f);
		float orthoSize = max(20.0f, focusRadius * 2.2f);
		float fovY = XM_PIDIV2;
		bool useOrthographic = true;

		if (runtimeLight.HasLight && lightType != LightType::Directional)
		{
			lightPos = XMLoadFloat3(&runtimeLight.Position);
			if (lightType == LightType::Point)
			{
				useOrthographic = false;
				XMVECTOR toTarget = XMVectorSubtract(target, lightPos);
				const float distanceToTarget = XMVectorGetX(XMVector3Length(toTarget));
				if (distanceToTarget > 0.000001f)
				{
					dir = XMVectorScale(toTarget, 1.0f / distanceToTarget);
				}
				else
				{
					dir = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
					target = XMVectorAdd(lightPos, dir);
				}

				nearClip = 0.03f;
				farClip = max(runtimeLight.Component.Range, distanceToTarget + focusRadius + 4.0f);
				const float requiredFov = 2.0f * atan2f(focusRadius, max(distanceToTarget, 0.1f));
				fovY = clamp(requiredFov, XM_PIDIV2, XMConvertToRadians(155.0f));
			}
			else
			{
				XMVECTOR spotDir = XMVectorSet(runtimeLight.Component.Direction.x, runtimeLight.Component.Direction.y, runtimeLight.Component.Direction.z, 0.0f);
				if (XMVectorGetX(XMVector3LengthSq(spotDir)) > 0.000001f)
				{
					dir = XMVector3Normalize(spotDir);
				}
				target = XMVectorAdd(lightPos, XMVectorScale(dir, max(runtimeLight.Component.Range, 1.0f)));

				nearClip = 0.05f;
				farClip = max(runtimeLight.Component.Range, 1.0f);
				float outerAngle = runtimeLight.Component.OuterAngle * XM_PI / 180.0f;
				if (cylinderVolume)
				{
					const float cylinderRadius = max(0.15f, tanf(outerAngle) * farClip * 0.35f);
					orthoSize = max(cylinderRadius * 2.0f, 0.3f);
					useOrthographic = true;
				}
				else
				{
					fovY = outerAngle * 2.0f;
					if (fovY < XMConvertToRadians(1.0f)) fovY = XMConvertToRadians(1.0f);
					if (fovY > XMConvertToRadians(175.0f)) fovY = XMConvertToRadians(175.0f);
					useOrthographic = false;
				}
			}
		}

		XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		if (fabsf(XMVectorGetX(XMVector3Dot(dir, up))) > 0.96f)
		{
			up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
		}

		const XMMATRIX lightView = XMMatrixLookAtLH(lightPos, target, up);
		const XMMATRIX lightProjection = useOrthographic
			? XMMatrixOrthographicLH(orthoSize, orthoSize, nearClip, farClip)
			: XMMatrixPerspectiveFovLH(fovY, 1.0f, nearClip, farClip);

		UINT shadowSize = (lightType == LightType::Directional)
			? RendererState::g_kSHADOW_MAP_SIZE
			: RendererState::g_kSHADOW_MAP_SIZE_SMALL;
		outLightViewProjection = XMMatrixTranspose(lightView * lightProjection);
		outShadowMapParams = XMFLOAT4(
			1.0f / static_cast<float>(shadowSize),
			RendererSettings::GetShadowDepthBias(),
			RendererSettings::GetShadowNormalBias(),
			1.0f);
	}

	void BuildVirtualDirectionalShadowViewProjection(const RuntimeLightState& runtimeLight, UINT level, XMMATRIX& outLightViewProjection, XMFLOAT4& outShadowMapParams)
	{
		XMFLOAT3 direction = runtimeLight.Component.Direction;
		XMVECTOR lightDirection = XMVectorSet(direction.x, direction.y, direction.z, 0.0f);
		if (XMVectorGetX(XMVector3LengthSq(lightDirection)) < 0.000001f)
		{
			lightDirection = XMVectorSet(0.25f, 1.0f, -0.25f, 0.0f);
		}
		lightDirection = XMVector3Normalize(lightDirection);

		const bool conventionalCascade = level >= 4;
		const UINT actualLevel = conventionalCascade ? level - 4 : level;
		const UINT cascadeCount = static_cast<UINT>(RendererSettings::GetShadowCascadeCount());
		const float residentFraction =
			static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION) /
			static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION);
		// VirtualFirstLevelRadius describes the actually resident world-space
		// radius, not the unallocated 16x16 virtual extent.  Keeping every level
		// in the same exact 2x series prevents the former level 2 -> 3 jump
		// (64 m -> 384 m full-map radius), which visibly changed silhouettes at
		// the last resolution boundary.
		const float residentRadius =
			RendererSettings::GetVirtualFirstLevelRadius() *
			static_cast<float>(1u << actualLevel);
		float radius = conventionalCascade
			? RendererSettings::GetShadowDistance() /
				static_cast<float>(1u << max(0, static_cast<int>(cascadeCount - 1 - actualLevel)))
			: residentRadius / max(residentFraction, 0.01f);

		XMVECTOR viewForward = XMVectorNegate(lightDirection);
		XMVECTOR upHint = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		if (fabsf(XMVectorGetX(XMVector3Dot(viewForward, upHint))) > 0.96f)
		{
			upHint = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
		}
		const XMVECTOR right = XMVector3Normalize(XMVector3Cross(upHint, viewForward));
		const XMVECTOR lightUp = XMVector3Normalize(XMVector3Cross(viewForward, right));
		const XMFLOAT3 cameraPosition = GetActiveCameraPosition();
		const XMVECTOR camera = XMLoadFloat3(&cameraPosition);

		float planeX = XMVectorGetX(XMVector3Dot(camera, right));
		float planeY = XMVectorGetX(XMVector3Dot(camera, lightUp));
		const float planeZ = XMVectorGetX(XMVector3Dot(camera, lightDirection));
		const float pageGrid = static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION);
		const float pageWorldSize = (radius * 2.0f) / pageGrid;

		if (RendererSettings::GetStabilizeVirtualClipmaps())
		{
			const float snapSize = conventionalCascade
				? (radius * 2.0f) / static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE)
				: pageWorldSize;
			planeX = roundf(planeX / snapSize) * snapSize;
			planeY = roundf(planeY / snapSize) * snapSize;
		}

		XMVECTOR target = XMVectorAdd(
			XMVectorAdd(XMVectorScale(right, planeX), XMVectorScale(lightUp, planeY)),
			XMVectorScale(lightDirection, planeZ));
		target = XMVectorSetW(target, 1.0f);

		if (!conventionalCascade && actualLevel < RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS)
		{
			const int centerPageX = static_cast<int>(roundf(planeX / pageWorldSize));
			const int centerPageY = static_cast<int>(roundf(-planeY / pageWorldSize));
			const int halfGrid = static_cast<int>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION / 2);
			g_VirtualShadowPageOrigins[actualLevel] = XMFLOAT4(
				static_cast<float>(centerPageX - halfGrid),
				static_cast<float>(centerPageY - halfGrid),
				pageGrid,
				pageWorldSize);
		}
		else if (conventionalCascade && actualLevel < RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS)
		{
			// Conventional cascades use the same metadata for their light-space
			// overlap and a level-independent world-space PCF width.
			g_VirtualShadowPageOrigins[actualLevel] = XMFLOAT4(
				0.0f,
				0.0f,
				pageGrid,
				pageWorldSize);
		}

		const XMVECTOR lightPosition = XMVectorAdd(
			target,
			XMVectorScale(lightDirection, max(120.0f, radius * 3.0f)));
		const XMMATRIX view = XMMatrixLookAtLH(lightPosition, target, lightUp);
		const float shadowFarClip = max(300.0f, radius * 8.0f);
		const XMMATRIX projection = XMMatrixOrthographicLH(
			radius * 2.0f,
			radius * 2.0f,
			0.1f,
			shadowFarClip);
		outLightViewProjection = XMMatrixTranspose(view * projection);
		// The UI bias is calibrated against the 300 m reference projection.
		// Convert it per level so the receiver offset remains constant in world
		// space for conventional cascades. VSM texels double in world size at
		// every clipmap level, so its receiver offset must grow by the same 2x
		// series or the coarse levels regress into self-shadow bands.
		const float biasScale = (300.0f - 0.1f) / max(shadowFarClip - 0.1f, 0.0001f);
		const float virtualLevelBiasScale = conventionalCascade
			? 1.0f
			: static_cast<float>(1u << actualLevel);
		outShadowMapParams = XMFLOAT4(
			1.0f / static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE),
			RendererSettings::GetShadowDepthBias() * biasScale * virtualLevelBiasScale,
			RendererSettings::GetShadowNormalBias() * biasScale * virtualLevelBiasScale,
			1.0f);
	}

	int GetShapeSideCount(ShapeType shapeType)
	{
		switch (shapeType)
		{
		case ShapeType::TRIANGLE: return 3;
		case ShapeType::QUAD:     return 4;
		case ShapeType::PENTAGON: return 5;
		case ShapeType::HEXAGON:  return 6;
		case ShapeType::HEPTAGON: return 7;
		case ShapeType::OCTAGON:  return 8;
		case ShapeType::CIRCLE:   return 32;
		case ShapeType::NONE:
		default:
			return 3;
		}
	}

	XMFLOAT4 GetVertexColor(Color color)
	{
		switch (color)
		{
		case Color::RED:     return { 1.0f, 0.0f, 0.0f, 1.0f };
		case Color::GREEN:   return { 0.0f, 1.0f, 0.0f, 1.0f };
		case Color::BLUE:    return { 0.0f, 0.0f, 1.0f, 1.0f };
		case Color::YELLOW:  return { 1.0f, 1.0f, 0.0f, 1.0f };
		case Color::CYAN:    return { 0.0f, 1.0f, 1.0f, 1.0f };
		case Color::MAGENTA: return { 1.0f, 0.0f, 1.0f, 1.0f };
		case Color::WHITE:   return { 1.0f, 1.0f, 1.0f, 1.0f };
		case Color::NONE:
		default:
			return { 1.0f, 1.0f, 1.0f, 0.0f };
		}
	}

	template<typename ApplyUvFunc>
	vector<Vertex> CreateShapeVertices(ShapeType shapeType, float radius, Color color, ApplyUvFunc applyUv)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);

		if (shapeType == ShapeType::QUAD)
		{
			float halfW = radius;
			float halfH = radius;

			vertices.push_back({ { -halfW,  halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 0.0f, 0.0f }), vertexColor });
			vertices.push_back({ {  halfW,  halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 1.0f, 0.0f }), vertexColor });
			vertices.push_back({ { -halfW, -halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 0.0f, 1.0f }), vertexColor });

			vertices.push_back({ {  halfW,  halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 1.0f, 0.0f }), vertexColor });
			vertices.push_back({ {  halfW, -halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 1.0f, 1.0f }), vertexColor });
			vertices.push_back({ { -halfW, -halfH, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 0.0f, 1.0f }), vertexColor });
			return vertices;
		}

		const int actualSides = GetShapeSideCount(shapeType);
		const float angleStep = (2.0f * XM_PI) / actualSides;
		const float rotationOffset = 0.0f;

		for (int i = 0; i < actualSides; ++i)
		{
			float angle1 = i * angleStep + rotationOffset;
			float angle2 = (i + 1) * angleStep + rotationOffset;

			vertices.push_back({ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, applyUv({ 0.5f, 0.5f }), vertexColor });

			vertices.push_back({
				{ radius * sinf(angle1), radius * cosf(angle1), 0.0f },
				{ 0.0f, 0.0f, 1.0f },
				applyUv({ 0.5f + 0.5f * sinf(angle1), 0.5f - 0.5f * cosf(angle1) }),
				vertexColor
				});
			vertices.push_back({
				{ radius * sinf(angle2), radius * cosf(angle2), 0.0f },
				{ 0.0f, 0.0f, 1.0f },
				applyUv({ 0.5f + 0.5f * sinf(angle2), 0.5f - 0.5f * cosf(angle2) }),
				vertexColor
				});
		}

		return vertices;
	}

	XMFLOAT3 Subtract(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return { a.x - b.x, a.y - b.y, a.z - b.z };
	}

	XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return {
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x
		};
	}

	float Dot(const XMFLOAT3& a, const XMFLOAT3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	XMFLOAT3 Normalize(const XMFLOAT3& v)
	{
		const float lengthSq = Dot(v, v);
		if (lengthSq <= 0.000001f)
		{
			return { 0.0f, 1.0f, 0.0f };
		}
		const float invLength = 1.0f / sqrtf(lengthSq);
		return { v.x * invLength, v.y * invLength, v.z * invLength };
	}

	void AddTriangle(vector<Vertex>& vertices, Vertex a, Vertex b, Vertex c, const XMFLOAT3& outward)
	{
		const XMFLOAT3 edge1 = Subtract(b.Pos, a.Pos);
		const XMFLOAT3 edge2 = Subtract(c.Pos, a.Pos);
		if (Dot(Cross(edge1, edge2), outward) < 0.0f)
		{
			swap(b, c);
		}

		vertices.push_back(a);
		vertices.push_back(b);
		vertices.push_back(c);
	}

	void AddFace(vector<Vertex>& vertices, const XMFLOAT4& color, XMFLOAT3 normal, XMFLOAT3 v0, XMFLOAT3 v1, XMFLOAT3 v2, XMFLOAT3 v3)
	{
		AddTriangle(vertices, { v0, normal, { 0.0f, 0.0f }, color }, { v1, normal, { 1.0f, 0.0f }, color }, { v2, normal, { 1.0f, 1.0f }, color }, normal);
		AddTriangle(vertices, { v0, normal, { 0.0f, 0.0f }, color }, { v2, normal, { 1.0f, 1.0f }, color }, { v3, normal, { 0.0f, 1.0f }, color }, normal);
	}

	XMFLOAT3 SpherePosition(float radius, float theta, float phi)
	{
		return {
			radius * sinf(phi) * sinf(theta),
			radius * cosf(phi),
			radius * sinf(phi) * cosf(theta)
		};
	}

	void AddSphereBand(vector<Vertex>& vertices, const XMFLOAT4& color, float radius, float centerY, float phi0, float phi1, int slices)
	{
		const float thetaStep = (2.0f * XM_PI) / slices;

		for (int slice = 0; slice < slices; ++slice)
		{
			const float theta0 = slice * thetaStep;
			const float theta1 = (slice + 1) * thetaStep;

			XMFLOAT3 n00 = Normalize(SpherePosition(1.0f, theta0, phi0));
			XMFLOAT3 n01 = Normalize(SpherePosition(1.0f, theta1, phi0));
			XMFLOAT3 n10 = Normalize(SpherePosition(1.0f, theta0, phi1));
			XMFLOAT3 n11 = Normalize(SpherePosition(1.0f, theta1, phi1));

			XMFLOAT3 p00 = { n00.x * radius, centerY + n00.y * radius, n00.z * radius };
			XMFLOAT3 p01 = { n01.x * radius, centerY + n01.y * radius, n01.z * radius };
			XMFLOAT3 p10 = { n10.x * radius, centerY + n10.y * radius, n10.z * radius };
			XMFLOAT3 p11 = { n11.x * radius, centerY + n11.y * radius, n11.z * radius };

			if (phi0 <= 0.0001f)
			{
				AddTriangle(vertices, { p00, n00, { 0.5f, 0.0f }, color }, { p11, n11, { 1.0f, 1.0f }, color }, { p10, n10, { 0.0f, 1.0f }, color }, Normalize(p00));
			}
			else if (phi1 >= XM_PI - 0.0001f)
			{
				AddTriangle(vertices, { p00, n00, { 0.0f, 0.0f }, color }, { p01, n01, { 1.0f, 0.0f }, color }, { p10, n10, { 0.5f, 1.0f }, color }, Normalize(p10));
			}
			else
			{
				XMFLOAT3 outward = Normalize({ p00.x + p01.x + p10.x + p11.x, p00.y + p01.y + p10.y + p11.y - centerY * 4.0f, p00.z + p01.z + p10.z + p11.z });
				AddTriangle(vertices, { p00, n00, { 0.0f, 0.0f }, color }, { p01, n01, { 1.0f, 0.0f }, color }, { p11, n11, { 1.0f, 1.0f }, color }, outward);
				AddTriangle(vertices, { p00, n00, { 0.0f, 0.0f }, color }, { p11, n11, { 1.0f, 1.0f }, color }, { p10, n10, { 0.0f, 1.0f }, color }, outward);
			}
		}
	}

	vector<Vertex> CreateQuadVertices(Color color)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const float h = 0.5f;
		AddFace(vertices, vertexColor, { 0.0f, 0.0f, 1.0f }, { -h,  h, 0.0f }, { h,  h, 0.0f }, { h, -h, 0.0f }, { -h, -h, 0.0f });
		return vertices;
	}

	vector<Vertex> CreatePlaneVertices(Color color)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const float h = 0.5f;
		AddFace(vertices, vertexColor, { 0.0f, 1.0f, 0.0f }, { -h, 0.0f, -h }, { h, 0.0f, -h }, { h, 0.0f, h }, { -h, 0.0f, h });
		return vertices;
	}

	vector<Vertex> CreateCubeVertices(Color color)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const float h = 0.5f;

		AddFace(vertices, vertexColor, { 0.0f, 0.0f, 1.0f }, { -h,  h,  h }, {  h,  h,  h }, {  h, -h,  h }, { -h, -h,  h });
		AddFace(vertices, vertexColor, { 0.0f, 0.0f,-1.0f }, {  h,  h, -h }, { -h,  h, -h }, { -h, -h, -h }, {  h, -h, -h });
		AddFace(vertices, vertexColor, { 1.0f, 0.0f, 0.0f }, {  h,  h,  h }, {  h,  h, -h }, {  h, -h, -h }, {  h, -h,  h });
		AddFace(vertices, vertexColor, {-1.0f, 0.0f, 0.0f }, { -h,  h, -h }, { -h,  h,  h }, { -h, -h,  h }, { -h, -h, -h });
		AddFace(vertices, vertexColor, { 0.0f, 1.0f, 0.0f }, { -h,  h, -h }, {  h,  h, -h }, {  h,  h,  h }, { -h,  h,  h });
		AddFace(vertices, vertexColor, { 0.0f,-1.0f, 0.0f }, { -h, -h,  h }, {  h, -h,  h }, {  h, -h, -h }, { -h, -h, -h });

		return vertices;
	}

	vector<Vertex> CreateSphereVertices(Color color)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const int stacks = 16;
		const int slices = 32;
		const float phiStep = XM_PI / stacks;

		for (int stack = 0; stack < stacks; ++stack)
		{
			AddSphereBand(vertices, vertexColor, 0.5f, 0.0f, stack * phiStep, (stack + 1) * phiStep, slices);
		}

		return vertices;
	}

	vector<Vertex> CreateCylinderVertices(Color color, bool includeCaps = true)
	{
		vector<Vertex> vertices;
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const int slices = 32;
		const float radius = 0.5f;
		const float halfH = 0.5f;
		const float thetaStep = (2.0f * XM_PI) / slices;

		for (int i = 0; i < slices; ++i)
		{
			const float t0 = i * thetaStep;
			const float t1 = (i + 1) * thetaStep;
			const XMFLOAT3 n0 = Normalize({ sinf(t0), 0.0f, cosf(t0) });
			const XMFLOAT3 n1 = Normalize({ sinf(t1), 0.0f, cosf(t1) });
			const XMFLOAT3 top0 = { n0.x * radius, halfH, n0.z * radius };
			const XMFLOAT3 top1 = { n1.x * radius, halfH, n1.z * radius };
			const XMFLOAT3 bottom0 = { n0.x * radius, -halfH, n0.z * radius };
			const XMFLOAT3 bottom1 = { n1.x * radius, -halfH, n1.z * radius };
			const XMFLOAT3 sideOut = Normalize({ n0.x + n1.x, 0.0f, n0.z + n1.z });

			AddTriangle(vertices, { top0, n0, { 0.0f, 0.0f }, vertexColor }, { top1, n1, { 1.0f, 0.0f }, vertexColor }, { bottom1, n1, { 1.0f, 1.0f }, vertexColor }, sideOut);
			AddTriangle(vertices, { top0, n0, { 0.0f, 0.0f }, vertexColor }, { bottom1, n1, { 1.0f, 1.0f }, vertexColor }, { bottom0, n0, { 0.0f, 1.0f }, vertexColor }, sideOut);
			if (includeCaps)
			{
				AddTriangle(vertices, { { 0.0f, halfH, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f }, vertexColor }, { top0, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f }, vertexColor }, { top1, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f }, vertexColor }, { 0.0f, 1.0f, 0.0f });
				AddTriangle(vertices, { { 0.0f, -halfH, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.5f, 0.5f }, vertexColor }, { bottom1, { 0.0f, -1.0f, 0.0f }, { 1.0f, 1.0f }, vertexColor }, { bottom0, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f }, vertexColor }, { 0.0f, -1.0f, 0.0f });
			}
		}

		return vertices;
	}

	vector<Vertex> CreateCapsuleVertices(Color color)
	{
		vector<Vertex> vertices = CreateCylinderVertices(color, false);
		const XMFLOAT4 vertexColor = GetVertexColor(color);
		const int hemiStacks = 8;
		const int slices = 32;
		const float radius = 0.5f;
		const float topCenterY = 0.5f;
		const float bottomCenterY = -0.5f;
		const float phiStep = (XM_PIDIV2) / hemiStacks;

		for (int stack = 0; stack < hemiStacks; ++stack)
		{
			AddSphereBand(vertices, vertexColor, radius, topCenterY, stack * phiStep, (stack + 1) * phiStep, slices);
		}
		for (int stack = 0; stack < hemiStacks; ++stack)
		{
			AddSphereBand(vertices, vertexColor, radius, bottomCenterY, XM_PIDIV2 + stack * phiStep, XM_PIDIV2 + (stack + 1) * phiStep, slices);
		}

		return vertices;
	}

}

void RendererResource::BeginFrame()
{
	++g_FrameSerial;
	g_LightCacheValid = false;
	m_TransientCbSlot = 0;
}

UINT RendererResource::GetShadowLightCount()
{
	if (!g_LightCacheValid)
	{
		RebuildLightCache();
	}
	return g_ShadowRenderPassCount;
}

void RendererResource::SetCurrentShadowPassIndex(UINT index)
{
	g_CurrentShadowPassIndex = (g_ShadowRenderPassCount > 0)
		? min(index, g_ShadowRenderPassCount - 1)
		: 0;
}

XMMATRIX RendererResource::GetCurrentShadowViewProjection()
{
	if (!g_LightCacheValid)
	{
		RebuildLightCache();
	}
	if (g_ShadowRenderPassCount == 0)
	{
		return XMMatrixIdentity();
	}

	return XMMatrixTranspose(g_ShadowRenderPasses[g_CurrentShadowPassIndex].ViewProjection);
}

D3D12_GPU_VIRTUAL_ADDRESS RendererResource::GetShadowConstantBufferAddress(UINT shadowIndex)
{
	if (!m_ShadowConstantBuffer)
	{
		return 0;
	}
	return m_ShadowConstantBuffer->GetGPUVirtualAddress() +
		GetShadowConstantBufferSlot(shadowIndex) * g_kSHADOW_CB_ALIGNED_SIZE;
}

D3D12_GPU_VIRTUAL_ADDRESS RendererResource::GetCurrentShadowConstantBufferAddress()
{
	return GetShadowConstantBufferAddress(g_CurrentShadowPassIndex);
}

D3D12_GPU_VIRTUAL_ADDRESS RendererResource::GetCurrentLightConstantBufferAddress()
{
	if (!m_LightConstantBuffer)
	{
		return 0;
	}
	return m_LightConstantBuffer->GetGPUVirtualAddress() +
		RendererCore::GetFrameIndex() * g_kLIGHT_CB_ALIGNED_SIZE;
}

D3D12_GPU_VIRTUAL_ADDRESS RendererResource::GetCurrentLightTileIndexBufferAddress()
{
	return LightInstanceBuilder::GetTileIndexAddress(RendererCore::GetFrameIndex());
}

D3D12_GPU_VIRTUAL_ADDRESS RendererResource::GetCurrentVolumetricLightIndexBufferAddress()
{
	return LightInstanceBuilder::GetVolumetricIndexAddress(RendererCore::GetFrameIndex());
}

const RendererResource::LightGridStats& RendererResource::GetLightGridStats()
{
	return g_LightGridStats;
}

D3D12_GPU_VIRTUAL_ADDRESS RendererResource::GetPBRConstantBufferAddress(UINT slot)
{
	if (!m_PBRConstantBuffer)
	{
		return 0;
	}
	const UINT safeSlot = slot < g_kPBR_CB_SLOT_COUNT ? slot : 0;
	const UINT frameSlot = RendererCore::GetFrameIndex() * g_kPBR_CB_SLOT_COUNT + safeSlot;
	return m_PBRConstantBuffer->GetGPUVirtualAddress() +
		frameSlot * g_kPBR_CB_ALIGNED_SIZE;
}

D3D12_GPU_DESCRIPTOR_HANDLE RendererResource::AllocateTransientConstantBuffer(const ConstantBuffer3D& constants)
{
	UINT8* frameCbvDataBegin = GetConstantBufferPtr();
	if (!frameCbvDataBegin || !m_CbvHeap || m_TransientCbSlot >= g_kTRANSIENT_CB_SLOT_COUNT)
	{
		return {};
	}

	const UINT slot = g_kTRANSIENT_CB_START_INDEX + m_TransientCbSlot++;
	memcpy(frameCbvDataBegin + (slot * g_kCB_ALIGNED_SIZE), &constants, sizeof(constants));
	return GetConstantBufferHandle(slot);
}

void RendererResource::UpdateLightConstantBuffer(float deferredLightStrength)
{
	if (!m_pLightCbvDataBegin)
	{
		return;
	}
	if (g_LightConstantsSerial == g_FrameSerial &&
		fabsf(g_LightConstantsStrength - deferredLightStrength) <= 0.0001f)
	{
		return;
	}

	// The directional light is the stable primary source for the atmosphere and
	// legacy paths.  Local lights still populate the deferred light array below.
	const RuntimeLightState& directionalLight = GetCachedLightState(true);
	const RuntimeLightState& runtimeLight = directionalLight.HasLight
		? directionalLight
		: GetCachedLightState(false);
	LightConstants constants{};
	WriteAtmosphereConstants(constants);
	for (UINT i = 0; i < RendererState::g_kMAX_SHADER_LIGHTS; ++i)
	{
		constants.LightViewProjections[i] = XMMatrixIdentity();
		constants.LightShadowData[i] = XMFLOAT4(
			-1.0f,
			1.0f / static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE),
			0.000008f,
			0.00001f);
	}
	for (UINT i = 0; i < RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS; ++i)
	{
		constants.VirtualShadowViewProjections[i] = g_VirtualShadowViewProjections[i];
		constants.VirtualShadowParams[i] = g_VirtualShadowParams[i];
		constants.VirtualShadowPageOrigins[i] = g_VirtualShadowPageOrigins[i];
	}
	for (UINT level = 0; level < RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS; ++level)
	{
		for (UINT group = 0; group < 4; ++group)
		{
			const UINT row = group * 4;
			constants.VirtualShadowResidency[level * 4 + group] = XMUINT4(
				g_VirtualShadowResidencyRows[level][row + 0],
				g_VirtualShadowResidencyRows[level][row + 1],
				g_VirtualShadowResidencyRows[level][row + 2],
				g_VirtualShadowResidencyRows[level][row + 3]);
		}
	}
	constants.VirtualShadowGlobal = XMFLOAT4(
		g_DirectionalShadowMode,
		static_cast<float>(g_VirtualShadowLevelCount),
		static_cast<float>(RendererSettings::GetShadowFilterRadius()),
		RendererSettings::GetShadowResolutionTransition());
	constants.ShadowRuntimeGlobal = XMFLOAT4(
		RendererSettings::GetContactShadowsEnabled() ? 1.0f : 0.0f,
		RendererSettings::GetContactShadowLength(),
		static_cast<float>(RendererSettings::GetContactShadowSteps()),
		static_cast<float>(RendererSettings::GetShadowMapMethod()));
	constants.ShadowDebugGlobal = XMFLOAT4(
		static_cast<float>(RendererSettings::GetVirtualShadowDebugMode()),
		g_VirtualShadowCacheHit ? 1.0f : 0.0f,
		static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION),
		static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION));
	UINT distanceFieldCount = 0;
	for (EntityID entity : World::GetView<TransformComponent, AABBComponent>())
	{
		if (distanceFieldCount >= RendererState::g_kMAX_DISTANCE_FIELD_SHADOW_OBJECTS) break;
		if (!IsShadowBoundsEntity(entity)) continue;
		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		const auto& aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
		const XMFLOAT3 center = {
			transform.Position.x + aabb.Center.x * transform.Scale.x,
			transform.Position.y + aabb.Center.y * transform.Scale.y,
			transform.Position.z + aabb.Center.z * transform.Scale.z };
		const XMFLOAT3 extents = {
			max(fabsf(aabb.Extents.x * transform.Scale.x), 0.02f),
			max(fabsf(aabb.Extents.y * transform.Scale.y), 0.02f),
			max(fabsf(aabb.Extents.z * transform.Scale.z), 0.02f) };
		constants.DistanceFieldData0[distanceFieldCount] = XMFLOAT4(center.x, center.y, center.z, 1.0f);
		constants.DistanceFieldData1[distanceFieldCount] = XMFLOAT4(extents.x, extents.y, extents.z, 0.0f);
		++distanceFieldCount;
	}
	constants.DistanceFieldGlobal = XMFLOAT4(
		RendererSettings::GetDistanceFieldShadowsEnabled() ? static_cast<float>(distanceFieldCount) : 0.0f,
		RendererSettings::GetDistanceFieldShadowDistance(),
		static_cast<float>(RendererSettings::GetDistanceFieldShadowSteps()),
		0.0f);
	const auto& localFogVolumes = LocalHeightFog::GetVolumes();
	const UINT localFogCount = min(static_cast<UINT>(localFogVolumes.size()), RendererState::g_kMAX_LOCAL_HEIGHT_FOG_VOLUMES);
	for (UINT i = 0; i < localFogCount; ++i)
	{
		const auto& volume = localFogVolumes[i];
		constants.LocalFogData0[i] = XMFLOAT4(volume.Position.x, volume.Position.y, volume.Position.z, max(volume.Radius, 0.01f));
		constants.LocalFogData1[i] = XMFLOAT4(max(volume.HeightFalloff, 0.0f), max(volume.Density, 0.0f), static_cast<float>(volume.Shape), volume.Enabled ? 1.0f : 0.0f);
		constants.LocalFogColors[i] = XMFLOAT4(volume.Color.x, volume.Color.y, volume.Color.z, 1.0f);
	}
	constants.LocalFogGlobal = XMFLOAT4(static_cast<float>(localFogCount), 0.0f, 0.0f, 0.0f);
	if (runtimeLight.HasLight)
	{
		const LightComponent& lightComponent = runtimeLight.Component;
		XMFLOAT3 lightDirection = NormalizeFloat3(lightComponent.Direction, { 0.0f, 1.0f, -1.0f });
		XMFLOAT4 lightColor = lightComponent.Color;
		lightColor.w = deferredLightStrength * lightComponent.Intensity;

		constants.LightDirection = XMFLOAT4(lightDirection.x, lightDirection.y, lightDirection.z, lightComponent.Range);
		constants.LightColor = lightColor;
		constants.LightPositionType = XMFLOAT4(runtimeLight.Position.x, runtimeLight.Position.y, runtimeLight.Position.z, static_cast<float>(lightComponent.Type));
		const float innerRad = lightComponent.InnerAngle * XM_PI / 180.0f;
		const float outerRad = lightComponent.OuterAngle * XM_PI / 180.0f;
		constants.LightExtra = XMFLOAT4(cosf(innerRad), cosf(outerRad), lightComponent.VolumeDensity, static_cast<float>(lightComponent.VolumeShape));
	}
	else
	{
		constants.LightDirection = XMFLOAT4(0.0f, 1.0f, -1.0f, 12.0f);
		constants.LightColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.0f);
		constants.LightPositionType = XMFLOAT4(0.0f, 0.0f, 0.0f, static_cast<float>(LightType::Directional));
		constants.LightExtra = XMFLOAT4(cosf(18.0f * XM_PI / 180.0f), cosf(32.0f * XM_PI / 180.0f), 0.35f, 0.0f);
	}

	struct VisibleLightCandidate
	{
		EntityID Entity = g_kINVALID_ENTITY;
		LightComponent Light{};
		XMFLOAT3 Position{};
		UINT MinTileX = 0;
		UINT MinTileY = 0;
		UINT MaxTileX = 0;
		UINT MaxTileY = 0;
		float Score = 0.0f;
		bool Directional = false;
	};

	g_LightGridStats = {};
	const UINT sceneWidth = max(m_SceneWidth, 1u);
	const UINT sceneHeight = max(m_SceneHeight, 1u);
	const UINT tileCountX =
		(sceneWidth + g_kLIGHT_TILE_SIZE - 1) / g_kLIGHT_TILE_SIZE;
	const UINT tileCountY =
		(sceneHeight + g_kLIGHT_TILE_SIZE - 1) / g_kLIGHT_TILE_SIZE;
	g_LightGridStats.TileCountX = tileCountX;
	g_LightGridStats.TileCountY = tileCountY;

	XMMATRIX cameraView = XMMatrixIdentity();
	XMMATRIX cameraProjection = XMMatrixIdentity();
	Camera::GetCameraMatrices(Camera::GetCameraEntity(), cameraView, cameraProjection);
	const float projectionX = fabsf(XMVectorGetX(cameraProjection.r[0]));
	const float projectionY = fabsf(XMVectorGetY(cameraProjection.r[1]));

	auto calculateTileBounds = [&](const LightComponent& light, const XMFLOAT3& position,
		UINT& minTileX, UINT& minTileY, UINT& maxTileX, UINT& maxTileY) -> bool
	{
		if (light.Type == LightType::Directional)
		{
			minTileX = 0;
			minTileY = 0;
			maxTileX = tileCountX - 1;
			maxTileY = tileCountY - 1;
			return true;
		}

		XMFLOAT3 viewPosition{};
		XMStoreFloat3(&viewPosition, XMVector3TransformCoord(XMLoadFloat3(&position), cameraView));
		const float radius = max(light.Range, 0.01f);
		if (viewPosition.z + radius <= 0.1f)
		{
			return false;
		}

		if (viewPosition.z <= radius + 0.1f)
		{
			minTileX = 0;
			minTileY = 0;
			maxTileX = tileCountX - 1;
			maxTileY = tileCountY - 1;
			return true;
		}

		const float reciprocalDepth = 1.0f / max(viewPosition.z, 0.1f);
		const float radiusDepth = 1.0f / max(viewPosition.z - radius, 0.1f);
		const float centerNdcX = viewPosition.x * projectionX * reciprocalDepth;
		const float centerNdcY = viewPosition.y * projectionY * reciprocalDepth;
		const float radiusNdcX = radius * projectionX * radiusDepth;
		const float radiusNdcY = radius * projectionY * radiusDepth;
		const float centerPixelX = (centerNdcX * 0.5f + 0.5f) * sceneWidth;
		const float centerPixelY = (-centerNdcY * 0.5f + 0.5f) * sceneHeight;
		const float radiusPixelX = radiusNdcX * 0.5f * sceneWidth;
		const float radiusPixelY = radiusNdcY * 0.5f * sceneHeight;
		const int minPixelX = static_cast<int>(floorf(centerPixelX - radiusPixelX));
		const int minPixelY = static_cast<int>(floorf(centerPixelY - radiusPixelY));
		const int maxPixelX = static_cast<int>(ceilf(centerPixelX + radiusPixelX));
		const int maxPixelY = static_cast<int>(ceilf(centerPixelY + radiusPixelY));
		if (maxPixelX < 0 || maxPixelY < 0 ||
			minPixelX >= static_cast<int>(sceneWidth) ||
			minPixelY >= static_cast<int>(sceneHeight))
		{
			return false;
		}

		const UINT clampedMinX = static_cast<UINT>(clamp(minPixelX, 0, static_cast<int>(sceneWidth) - 1));
		const UINT clampedMinY = static_cast<UINT>(clamp(minPixelY, 0, static_cast<int>(sceneHeight) - 1));
		const UINT clampedMaxX = static_cast<UINT>(clamp(maxPixelX, 0, static_cast<int>(sceneWidth) - 1));
		const UINT clampedMaxY = static_cast<UINT>(clamp(maxPixelY, 0, static_cast<int>(sceneHeight) - 1));
		minTileX = clampedMinX / g_kLIGHT_TILE_SIZE;
		minTileY = clampedMinY / g_kLIGHT_TILE_SIZE;
		maxTileX = clampedMaxX / g_kLIGHT_TILE_SIZE;
		maxTileY = clampedMaxY / g_kLIGHT_TILE_SIZE;
		return true;
	};

	vector<VisibleLightCandidate> candidates;
	candidates.reserve(64);
	for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
	{
		++g_LightGridStats.AuthoredLights;
		const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
		if (!light.IsActive || light.RenderMode == LightRenderMode::EmissionOnly)
		{
			continue;
		}
		if (light.RenderMode == LightRenderMode::Physical)
		{
			++g_LightGridStats.ActivePhysicalLights;
		}
		else
		{
			++g_LightGridStats.ActiveDecalLights;
		}

		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		VisibleLightCandidate candidate{};
		candidate.Entity = entity;
		candidate.Light = light;
		candidate.Position = transform.Position;
		candidate.Directional = light.Type == LightType::Directional;
		if (!calculateTileBounds(light, transform.Position,
			candidate.MinTileX, candidate.MinTileY,
			candidate.MaxTileX, candidate.MaxTileY))
		{
			continue;
		}
		++g_LightGridStats.OnScreenLights;
		if (light.AffectsVolumetrics && light.VolumeDensity > 0.0001f)
		{
			++g_LightGridStats.VolumetricLights;
		}
		if (light.CastShadow)
		{
			++g_LightGridStats.ShadowedLights;
		}
		const float tileCoverage = static_cast<float>(
			(candidate.MaxTileX - candidate.MinTileX + 1) *
			(candidate.MaxTileY - candidate.MinTileY + 1)) /
			max(static_cast<float>(tileCountX * tileCountY), 1.0f);
		const float luminance = max(
			light.Color.x * 0.299f + light.Color.y * 0.587f + light.Color.z * 0.114f,
			0.0f);
		candidate.Score = light.Priority * 1000.0f +
			max(light.Intensity, 0.0f) * luminance * (0.25f + tileCoverage);
		candidates.push_back(candidate);
	}

	stable_sort(candidates.begin(), candidates.end(),
		[](const VisibleLightCandidate& lhs, const VisibleLightCandidate& rhs)
		{
			if (lhs.Light.RenderMode != rhs.Light.RenderMode)
			{
				return lhs.Light.RenderMode == LightRenderMode::Physical;
			}
			if (lhs.Directional != rhs.Directional)
			{
				return lhs.Directional;
			}
			if (lhs.Light.AffectsVolumetrics != rhs.Light.AffectsVolumetrics)
			{
				return lhs.Light.AffectsVolumetrics;
			}
			if (fabsf(lhs.Score - rhs.Score) > 0.000001f)
			{
				return lhs.Score > rhs.Score;
			}
			return lhs.Entity < rhs.Entity;
		});

	const UINT screenLightBudget = min(
		static_cast<UINT>(RendererSettings::GetScreenLightBudget()),
		g_kMAX_SHADER_LIGHTS);
	const UINT physicalTileLightBudget = min(
		static_cast<UINT>(RendererSettings::GetTileLightBudget()),
		g_kMAX_LIGHTS_PER_TILE);
	const UINT decalLightBudget = min(
		static_cast<UINT>(RendererSettings::GetDecalLightBudget()),
		g_kMAX_SHADER_LIGHTS - screenLightBudget);
	const UINT decalTileLightBudget = min(
		static_cast<UINT>(RendererSettings::GetDecalTileLightBudget()),
		g_kMAX_LIGHTS_PER_TILE - physicalTileLightBudget);
	const UINT volumetricLightBudget = min(
		static_cast<UINT>(RendererSettings::GetVolumetricLightBudget()),
		5u);
	vector<VisibleLightCandidate> selectedCandidates;
	selectedCandidates.reserve(screenLightBudget + decalLightBudget);
	UINT selectedPhysicalLights = 0;
	UINT selectedDecalLights = 0;
	for (const VisibleLightCandidate& candidate : candidates)
	{
		if (candidate.Light.RenderMode == LightRenderMode::Physical)
		{
			if (selectedPhysicalLights >= screenLightBudget) continue;
			++selectedPhysicalLights;
		}
		else
		{
			if (selectedDecalLights >= decalLightBudget) continue;
			++selectedDecalLights;
		}
		selectedCandidates.push_back(candidate);
	}
	const UINT lightCount = static_cast<UINT>(selectedCandidates.size());
	g_LightGridStats.GpuVisibleLights = lightCount;
	g_LightGridStats.GpuPhysicalLights = selectedPhysicalLights;
	g_LightGridStats.GpuDecalLights = selectedDecalLights;
	UINT enabledVolumetricLights = 0;
	vector<LightInstanceBuilder::Input> lightInstances;
	lightInstances.reserve(lightCount);
	for (UINT lightIndex = 0; lightIndex < lightCount; ++lightIndex)
	{
		const VisibleLightCandidate& candidate = selectedCandidates[lightIndex];
		const LightComponent& light = candidate.Light;
		const XMFLOAT3 direction = NormalizeFloat3(light.Direction, { 0.0f, 1.0f, -1.0f });
		XMFLOAT4 color = light.Color;
		color.w = deferredLightStrength * light.Intensity;
		const float innerRad = light.InnerAngle * XM_PI / 180.0f;
		const float outerRad = light.OuterAngle * XM_PI / 180.0f;

		constants.LightDirections[lightIndex] = XMFLOAT4(direction.x, direction.y, direction.z, light.Range);
		constants.LightColors[lightIndex] = color;
		constants.LightPositionTypes[lightIndex] = XMFLOAT4(
			candidate.Position.x, candidate.Position.y, candidate.Position.z,
			static_cast<float>(light.Type));
		constants.LightExtras[lightIndex] = XMFLOAT4(
			cosf(innerRad), cosf(outerRad), light.VolumeDensity,
			static_cast<float>(light.VolumeShape));
		const bool enableVolumetrics = light.RenderMode == LightRenderMode::Physical &&
			light.AffectsVolumetrics &&
			light.VolumeDensity > 0.0001f &&
			enabledVolumetricLights < volumetricLightBudget;
		if (enableVolumetrics)
		{
			++enabledVolumetricLights;
		}
		const bool decal = light.RenderMode == LightRenderMode::Decal;
		const UINT slotBegin = decal ? physicalTileLightBudget : 0u;
		const UINT slotEnd = decal
			? physicalTileLightBudget + decalTileLightBudget
			: physicalTileLightBudget;
		if (light.AffectsOpaque || light.AffectsForward || enableVolumetrics)
		{
			LightInstanceBuilder::Input instance{};
			instance.TileBounds = XMUINT4(
				candidate.MinTileX, candidate.MinTileY,
				candidate.MaxTileX, candidate.MaxTileY);
			instance.Metadata = XMUINT4(
				lightIndex, slotBegin, slotEnd, enableVolumetrics ? 1u : 0u);
			lightInstances.push_back(instance);
		}
		constants.LightFlags[lightIndex] = XMFLOAT4(
			light.AffectsOpaque ? 1.0f : 0.0f,
			light.AffectsForward ? 1.0f : 0.0f,
			enableVolumetrics ? 1.0f : 0.0f,
			static_cast<float>(light.RenderMode));
		// An explicit -1 is required for non-shadowed lights.  Zero denotes
		// shadow array layer zero and previously made every light sample it.
		constants.LightShadowData[lightIndex] = XMFLOAT4(-1.0f, 0.0f, 0.0f, 0.0f);
		const int shadowIndex = FindShadowLightIndex(candidate.Entity);
		if (shadowIndex >= 0)
		{
			constants.LightViewProjections[lightIndex] = g_ShadowLightViewProjections[shadowIndex];
			constants.LightShadowData[lightIndex] = XMFLOAT4(
				static_cast<float>(shadowIndex),
				g_ShadowMapParams[shadowIndex].x,
				g_ShadowMapParams[shadowIndex].y,
				(candidate.Entity == g_VirtualShadowLightEntity)
					? -max(g_ShadowMapParams[shadowIndex].z, 0.0000001f)
					: g_ShadowMapParams[shadowIndex].z);
		}
	}

	const UINT slotsPerTile = physicalTileLightBudget + decalTileLightBudget;
	bool lightGridAvailable =
		tileCountX * tileCountY <= g_kMAX_LIGHT_TILE_COUNT &&
		slotsPerTile > 0 &&
		LightInstanceBuilder::Build(
			RendererCore::GetCommandList(), RendererCore::GetFrameIndex(),
			lightInstances, tileCountX, tileCountY, slotsPerTile);
	if (lightGridAvailable)
	{
		g_LightGridStats.MaxLightsPerTile = slotsPerTile;
	}
	constants.LocalFogGlobal.y = static_cast<float>(enabledVolumetricLights);

	constants.LightCount = XMFLOAT4(
		static_cast<float>(lightCount),
		lightGridAvailable ? static_cast<float>(tileCountX) : 0.0f,
		lightGridAvailable ? static_cast<float>(tileCountY) : 0.0f,
		lightGridAvailable
			? static_cast<float>(slotsPerTile)
			: 0.0f);
	auto* lightDst = static_cast<UINT8*>(m_pLightCbvDataBegin) +
		RendererCore::GetFrameIndex() * g_kLIGHT_CB_ALIGNED_SIZE;
	memcpy(lightDst, &constants, sizeof(constants));

	// Update PBR constants
	if (m_pPBRCbvDataBegin)
	{
		PBRConstants pbrConstants = BuildPBRConstantsFromMaterial(GetDeferredLightingMaterial());
		auto* pbrDst = static_cast<UINT8*>(m_pPBRCbvDataBegin) +
			RendererCore::GetFrameIndex() * g_kPBR_CB_SLOT_COUNT * g_kPBR_CB_ALIGNED_SIZE;
		memcpy(pbrDst, &pbrConstants, sizeof(pbrConstants));
	}

	g_LightConstantsSerial = g_FrameSerial;
	g_LightConstantsStrength = deferredLightStrength;
}

void RendererResource::UpdateShadowConstantBuffer()
{
	if (!m_pShadowCbvDataBegin)
	{
		return;
	}
	if (!g_LightCacheValid)
	{
		RebuildLightCache();
	}
	if (g_CurrentShadowPassIndex >= g_ShadowRenderPassCount)
	{
		return;
	}
	if (g_ShadowConstantsSerial == g_FrameSerial &&
		g_ShadowConstantsPassIndex == g_CurrentShadowPassIndex)
	{
		return;
	}

	ShadowConstants constants{};
	constants.LightViewProjection = g_ShadowRenderPasses[g_CurrentShadowPassIndex].ViewProjection;
	constants.ShadowMapParams = g_ShadowRenderPasses[g_CurrentShadowPassIndex].Params;
	constants.ShadowFilterParams.x = static_cast<float>(RendererSettings::GetShadowFilterRadius());
	auto* dst = static_cast<UINT8*>(m_pShadowCbvDataBegin) +
		GetShadowConstantBufferSlot(g_CurrentShadowPassIndex) * g_kSHADOW_CB_ALIGNED_SIZE;
	memcpy(dst, &constants, sizeof(constants));
	g_ShadowConstantsSerial = g_FrameSerial;
	g_ShadowConstantsPassIndex = g_CurrentShadowPassIndex;
}

void RendererResource::CreateSpriteVertex(const VertexResource& vertexstruct)
{
	if (vertexstruct.entityid >= g_kMAX_ENTITIES) return;

	if (!ComponentManager::HasComponent(vertexstruct.entityid, ComponentType::SPRITE))
	{
		ComponentManager::ReportMissingComponentError(vertexstruct.entityid, "SpriteComponent");
		return;
	}

	auto& spriteComponent = ComponentManager::GetComponent<SpriteComponent>(vertexstruct.entityid);
	auto ApplyUv = [&](const XMFLOAT2& uv)
		{
			if (!spriteComponent.UseUvTransform)
			{
				return uv;
			}
			return XMFLOAT2(
				uv.x * spriteComponent.UvScale.x + spriteComponent.UvOffset.x,
				uv.y * spriteComponent.UvScale.y + spriteComponent.UvOffset.y
			);
		};

	vector<Vertex> vertices = CreateShapeVertices(vertexstruct.shapetype, vertexstruct.radius, vertexstruct.color, ApplyUv);

	const UINT vertexBufferSize = static_cast<UINT>(sizeof(Vertex) * vertices.size());

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

	ID3D12Device* device = RendererCore::GetDevice();
	if (!device)
	{
		Debug::Log("ERROR: Renderer device is null in CreateSpriteVertex\n");
		return;
	}

	HRESULT hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&ComponentManager::GetComponent<SpriteComponent>(vertexstruct.entityid).VertexBuffer)
	);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: CreateCommittedResource failed\n");
		return;
	}

	UINT8* pVertexDataBegin{};
	CD3DX12_RANGE readRange(0, 0);

	hr = spriteComponent.VertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin));
	if (FAILED(hr))
	{
		Debug::Log("ERROR: VertexBuffer Map failed in CreateSpriteVertex\n");
		return;
	}
	memcpy(pVertexDataBegin, vertices.data(), vertexBufferSize);
	spriteComponent.VertexBuffer->Unmap(0, nullptr);

	spriteComponent.VertexBufferView.BufferLocation = spriteComponent.VertexBuffer->GetGPUVirtualAddress();
	spriteComponent.VertexBufferView.StrideInBytes = sizeof(Vertex);
	spriteComponent.VertexBufferView.SizeInBytes = vertexBufferSize;
	spriteComponent.VertexCount = static_cast<UINT>(vertices.size());
	spriteComponent.GeometryHash = HashGeometry(vertices.data(), vertexBufferSize);
	CalculateGeometryBounds(vertices, spriteComponent.LocalBoundsCenter, spriteComponent.LocalBoundsExtents);
	spriteComponent.HasLocalBounds = !vertices.empty();
}

void RendererResource::CreateObjectVertex(const VertexResource& vertexstruct)
{
	if (vertexstruct.entityid >= g_kMAX_ENTITIES) return;

	if (!ComponentManager::HasComponent(vertexstruct.entityid, ComponentType::MESH))
	{
		ComponentManager::ReportMissingComponentError(vertexstruct.entityid, "MeshComponent");
		return;
	}

	auto& meshComponent = ComponentManager::GetComponent<MeshComponent>(vertexstruct.entityid);

	vector<Vertex> vertices;
	switch (vertexstruct.objectType)
	{
	case ObjectType::CUBE:
		vertices = CreateCubeVertices(vertexstruct.color);
		break;
	case ObjectType::SPHERE:
		vertices = CreateSphereVertices(vertexstruct.color);
		break;
	case ObjectType::CAPSULE:
		vertices = CreateCapsuleVertices(vertexstruct.color);
		break;
	case ObjectType::CYLINDER:
		vertices = CreateCylinderVertices(vertexstruct.color);
		break;
	case ObjectType::PLANE:
		vertices = CreatePlaneVertices(vertexstruct.color);
		break;
	case ObjectType::QUAD:
		vertices = CreateQuadVertices(vertexstruct.color);
		break;
	case ObjectType::NONE:
	default:
		Debug::Log("ERROR: Unsupported ObjectType in CreateObjectVertex\n");
		return;
	}

	const UINT vertexBufferSize = static_cast<UINT>(sizeof(Vertex) * vertices.size());

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

	ID3D12Device* device = RendererCore::GetDevice();
	if (!device)
	{
		Debug::Log("ERROR: Renderer device is null in CreateObjectVertex\n");
		return;
	}

	HRESULT hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&meshComponent.VertexBuffer)
	);
	if (FAILED(hr))
	{
		Debug::Log("ERROR: CreateCommittedResource failed\n");
		return;
	}

	UINT8* pVertexDataBegin{};
	CD3DX12_RANGE readRange(0, 0);

	hr = meshComponent.VertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin));
	if (FAILED(hr))
	{
		Debug::Log("ERROR: VertexBuffer Map failed in CreateObjectVertex\n");
		return;
	}
	memcpy(pVertexDataBegin, vertices.data(), vertexBufferSize);
	meshComponent.VertexBuffer->Unmap(0, nullptr);

	meshComponent.VertexBufferView.BufferLocation = meshComponent.VertexBuffer->GetGPUVirtualAddress();
	meshComponent.VertexBufferView.StrideInBytes = sizeof(Vertex);
	meshComponent.VertexBufferView.SizeInBytes = vertexBufferSize;
	meshComponent.VertexCount = static_cast<UINT>(vertices.size());
	meshComponent.GeometryHash = HashGeometry(vertices.data(), vertexBufferSize);
	CalculateGeometryBounds(vertices, meshComponent.LocalBoundsCenter, meshComponent.LocalBoundsExtents);
	meshComponent.HasLocalBounds = !vertices.empty();
}

bool RendererResource::ShouldRenderShadowPass(UINT shadowIndex)
{
	if (!g_LightCacheValid) RebuildLightCache();
	if (shadowIndex >= g_ShadowRenderPassCount) return false;
	const ShadowRenderPass& pass = g_ShadowRenderPasses[shadowIndex];
	return !pass.VirtualPage || pass.NeedsRender;
}

bool RendererResource::ShouldDrawEntityInCurrentShadowPass(EntityID entity)
{
	if (!g_LightCacheValid) RebuildLightCache();
	if (g_CurrentShadowPassIndex >= g_ShadowRenderPassCount) return true;
	const ShadowRenderPass& pass = g_ShadowRenderPasses[g_CurrentShadowPassIndex];
	return IsShadowCasterVisible(entity, pass.ViewProjection);
}

bool RendererResource::IsCurrentShadowPassVirtualPage()
{
	if (!g_LightCacheValid) RebuildLightCache();
	return g_CurrentShadowPassIndex < g_ShadowRenderPassCount &&
		g_ShadowRenderPasses[g_CurrentShadowPassIndex].VirtualPage;
}

UINT RendererResource::GetCurrentShadowLodBias()
{
	// Keep the caster mesh identical in every shadow pass.  Selecting a coarser
	// mesh for outer virtual levels changes the silhouette exactly where the
	// shadow-map resolution changes and produces visible LOD bands.
	return 0;
}

bool RendererResource::IsVirtualShadowCacheHit()
{
	if (!g_LightCacheValid) RebuildLightCache();
	return g_VirtualShadowCacheHit;
}

bool RendererResource::GetShadowPassInfo(UINT shadowIndex, UINT& layer, D3D12_VIEWPORT& viewport, D3D12_RECT& scissor, bool& clearLayer)
{
	if (!g_LightCacheValid) RebuildLightCache();
	if (shadowIndex >= g_ShadowRenderPassCount) return false;
	const ShadowRenderPass& pass = g_ShadowRenderPasses[shadowIndex];
	layer = pass.Layer;
	viewport = {};
	viewport.TopLeftX = static_cast<float>(pass.X);
	viewport.TopLeftY = static_cast<float>(pass.Y);
	viewport.Width = static_cast<float>(pass.Size);
	viewport.Height = static_cast<float>(pass.Size);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	scissor = {
		static_cast<LONG>(pass.X),
		static_cast<LONG>(pass.Y),
		static_cast<LONG>(pass.X + pass.Size),
		static_cast<LONG>(pass.Y + pass.Size) };
	clearLayer = pass.ClearLayer;
	return true;
}

void RendererResource::SetMaterial(const EntityID entityID, const MaterialComponent& material)
{
	if (!m_pPBRCbvDataBegin || !m_PBRConstantBuffer || !m_CommandList)
	{
		return;
	}

	const UINT slot = entityID < g_kMAX_ENTITIES ? entityID + 1 : 0;
	PBRConstants constants = BuildPBRConstantsFromMaterial(material);

	const UINT frameSlot = RendererCore::GetFrameIndex() * g_kPBR_CB_SLOT_COUNT + slot;
	auto* dst = static_cast<UINT8*>(m_pPBRCbvDataBegin) + frameSlot * g_kPBR_CB_ALIGNED_SIZE;
	memcpy(dst, &constants, sizeof(constants));
	m_CommandList->SetGraphicsRootConstantBufferView(3, GetPBRConstantBufferAddress(slot));
}

uint64_t RendererResource::GetMaterialBatchHash(const MaterialComponent& material)
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
	state.IsTransparent = material.IsTransparent ? 1u : 0u;
	state.UseTexture = material.UseTexture ? 1u : 0u;
	state.UseNormalMap = material.NormalMapID >= 0 ? 1u : 0u;
	const uint64_t stateHash = HashGeometry(&state, sizeof(state));
	// Combine the two stable hashes without hashing object padding or STL members.
	hash ^= stateHash + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
	return hash;
}

