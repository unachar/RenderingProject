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
#include <limits>

namespace
{
	struct RuntimeLightState
	{
		LightComponent Component{};
		XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
		bool HasLight = false;
	};

	RuntimeLightState g_CachedDirectionalLight{};
	RuntimeLightState g_CachedAnyLight{};
	RuntimeLightState g_CachedShadowLight{};
	XMMATRIX g_ShadowLightViewProjections[RendererState::g_kMAX_SHADOW_LIGHTS]{};
	XMFLOAT4 g_ShadowMapParams[RendererState::g_kMAX_SHADOW_LIGHTS]{};
	EntityID g_ShadowLightEntities[RendererState::g_kMAX_SHADOW_LIGHTS]{};
	UINT g_ShadowLightCount = 0;
	UINT g_CurrentShadowPassIndex = 0;
	bool g_LightCacheValid = false;
	uint64_t g_FrameSerial = 0;
	uint64_t g_LightConstantsSerial = 0;
	uint64_t g_ShadowConstantsSerial = 0;
	UINT g_ShadowConstantsPassIndex = UINT_MAX;
	float g_LightConstantsStrength = -1.0f;

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
		XMFLOAT4 AtmosphereParams0 = { 1.0f, 0.42f, 0.075f, 0.36f };
		XMFLOAT4 AtmosphereParams1 = { 0.18f, 0.22f, 0.76f, 0.030f };
		XMFLOAT4 AtmosphereColor0 = { 0.46f, 0.62f, 1.0f, 0.55f };
		XMFLOAT4 AtmosphereColor1 = { 1.0f, 0.82f, 0.56f, 0.035f };
		XMFLOAT4 AtmosphereCamera = { 0.0f, 0.0f, 0.0f, 0.0f };
	};

	struct MaterialPartShaderConstants
	{
		XMFLOAT4 Basic{};
		XMFLOAT4 Base{};
		XMFLOAT4 Shadow0{};
		XMFLOAT4 Shadow1{};
		XMFLOAT4 Highlight{};
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
	};

	UINT GetShadowConstantBufferSlot(UINT shadowIndex)
	{
		const UINT safeShadowIndex = min(shadowIndex, RendererState::g_kMAX_SHADOW_LIGHTS - 1);
		return RendererCore::GetFrameIndex() * RendererState::g_kMAX_SHADOW_LIGHTS + safeShadowIndex;
	}

	void BuildShadowViewProjection(const RuntimeLightState& runtimeLight, XMMATRIX& outLightViewProjection, XMFLOAT4& outShadowMapParams);

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
		constants.AtmosphereCamera = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f);
	}

	void RebuildLightCache()
	{
		g_CachedDirectionalLight = {};
		g_CachedAnyLight = {};
		g_CachedShadowLight = {};
		g_ShadowLightCount = 0;
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

		for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
		{
			const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
			const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
			if (!light.IsActive)
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
			if (light.CastShadow && g_ShadowLightCount < RendererState::g_kMAX_SHADOW_LIGHTS)
			{
				RuntimeLightState shadowLight{};
				shadowLight.Component = light;
				shadowLight.Position = transform.Position;
				shadowLight.HasLight = true;
				g_ShadowLightEntities[g_ShadowLightCount] = entity;
				BuildShadowViewProjection(
					shadowLight,
					g_ShadowLightViewProjections[g_ShadowLightCount],
					g_ShadowMapParams[g_ShadowLightCount]);
				++g_ShadowLightCount;
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

		outLightViewProjection = XMMatrixTranspose(lightView * lightProjection);
		outShadowMapParams = XMFLOAT4(
			1.0f / static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE),
			0.000008f,
			0.00001f,
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
	return g_ShadowLightCount;
}

void RendererResource::SetCurrentShadowPassIndex(UINT index)
{
	g_CurrentShadowPassIndex = (g_ShadowLightCount > 0)
		? min(index, g_ShadowLightCount - 1)
		: 0;
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

	const RuntimeLightState& runtimeLight = GetCachedLightState(false);
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

	UINT lightCount = 0;
	for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
	{
		if (lightCount >= g_kMAX_SHADER_LIGHTS)
		{
			break;
		}

		const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
		if (!light.IsActive)
		{
			continue;
		}

		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		const XMFLOAT3 direction = NormalizeFloat3(light.Direction, { 0.0f, 1.0f, -1.0f });
		XMFLOAT4 color = light.Color;
		color.w = deferredLightStrength * light.Intensity;
		const float innerRad = light.InnerAngle * XM_PI / 180.0f;
		const float outerRad = light.OuterAngle * XM_PI / 180.0f;

		constants.LightDirections[lightCount] = XMFLOAT4(direction.x, direction.y, direction.z, light.Range);
		constants.LightColors[lightCount] = color;
		constants.LightPositionTypes[lightCount] = XMFLOAT4(transform.Position.x, transform.Position.y, transform.Position.z, static_cast<float>(light.Type));
		constants.LightExtras[lightCount] = XMFLOAT4(cosf(innerRad), cosf(outerRad), light.VolumeDensity, static_cast<float>(light.VolumeShape));
		const int shadowIndex = FindShadowLightIndex(entity);
		if (shadowIndex >= 0)
		{
			constants.LightViewProjections[lightCount] = g_ShadowLightViewProjections[shadowIndex];
			constants.LightShadowData[lightCount] = XMFLOAT4(
				static_cast<float>(shadowIndex),
				g_ShadowMapParams[shadowIndex].x,
				g_ShadowMapParams[shadowIndex].y,
				g_ShadowMapParams[shadowIndex].z);
		}
		++lightCount;
	}
	constants.LightCount = XMFLOAT4(static_cast<float>(lightCount), 0.0f, 0.0f, 0.0f);
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
	if (g_CurrentShadowPassIndex >= g_ShadowLightCount)
	{
		return;
	}
	if (g_ShadowConstantsSerial == g_FrameSerial &&
		g_ShadowConstantsPassIndex == g_CurrentShadowPassIndex)
	{
		return;
	}

	ShadowConstants constants{};
	constants.LightViewProjection = g_ShadowLightViewProjections[g_CurrentShadowPassIndex];
	constants.ShadowMapParams = g_ShadowMapParams[g_CurrentShadowPassIndex];
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

