#include "pch.h"
#include "lightingresources.h"
#include "graphicsdevice.h"
#include "shaderpaths.h"
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
#include "frameconstants.h"
#include "materialbindings.h"

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
		uint64_t ProjectionKey = 0;
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
	LightingResources::LightGridStats g_LightGridStats{};

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

	struct ShadowConstants
	{
		XMMATRIX LightViewProjection{};
		XMFLOAT4 ShadowMapParams = { 1.0f / RendererState::g_kSHADOW_MAP_SIZE, 0.0015f, 0.0025f, 1.0f };
		XMFLOAT4 ShadowFilterParams = { 1.0f, 0.0f, 0.0f, 0.0f };
	};

	UINT GetShadowConstantBufferSlot(UINT shadowIndex)
	{
		const UINT safeShadowIndex = min(shadowIndex, RendererState::g_kMAX_SHADOW_PASSES - 1);
		return GraphicsDevice::GetFrameIndex() * RendererState::g_kMAX_SHADOW_PASSES + safeShadowIndex;
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
			static_cast<float>(atmosphere.Enabled),
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
					if (virtualMode)
					{
						g_DirectionalShadowMode = 1.0f;
					}
					else
					{
						g_DirectionalShadowMode = 2.0f;
					}
					UINT shadowLevelCount;
					if (virtualMode)
					{
						shadowLevelCount = RendererSettings::GetVirtualClipmapLevels();
					}
					else
					{
						shadowLevelCount = RendererSettings::GetShadowCascadeCount();
					}
					const UINT requestedLevels = static_cast<UINT>(shadowLevelCount);
					g_VirtualShadowLevelCount = min(requestedLevels, shadowLightBudget - g_ShadowLightCount);
					for (UINT level = 0; level < g_VirtualShadowLevelCount; ++level)
					{
						const UINT viewIndex = g_ShadowLightCount++;
						g_ShadowLightEntities[viewIndex] = entity;
						UINT shadowLevel;
						if (virtualMode)
						{
							shadowLevel = level;
						}
						else
						{
							shadowLevel = level + 4;
						}
						BuildVirtualDirectionalShadowViewProjection(
							shadowLight, shadowLevel,
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
			if (g_CachedDirectionalLight.HasLight)
			{
				g_CachedShadowLight = g_CachedDirectionalLight;
			}
			else
			{
				g_CachedShadowLight = g_CachedAnyLight;
			}
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
				if (result < 0)
				{
					return result + modulus;
				}
				return result;
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

					uint64_t pageProjectionKey = 1469598103934665603ull;
					const auto* projectionBytes =
						reinterpret_cast<const uint8_t*>(&pageViewProjection);
					for (size_t byteIndex = 0;
						byteIndex < sizeof(pageViewProjection);
						++byteIndex)
					{
						pageProjectionKey ^= projectionBytes[byteIndex];
						pageProjectionKey *= 1099511628211ull;
					}

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


					hashPageValue(&pageViewProjection, sizeof(pageViewProjection));


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
						cacheEntry.GlobalPageY == globalPageY &&
						cacheEntry.ProjectionKey == pageProjectionKey;
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
						cacheEntry.ProjectionKey = pageProjectionKey;
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
		if (preferDirectional)
		{
			return g_CachedDirectionalLight;
		}
		return g_CachedAnyLight;
	}

	const RuntimeLightState& GetCachedShadowLightState()
	{
		if (!g_LightCacheValid)
		{
			RebuildLightCache();
		}
		return g_CachedShadowLight;
	}

	static bool IsSkyEntity(EntityID entity)
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
		XMVECTOR boundsMin = XMVectorReplicate(numeric_limits<float>::max());
		XMVECTOR boundsMax = XMVectorReplicate(-numeric_limits<float>::max());

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
		XMFLOAT3 lightDirection;
		if (runtimeLight.HasLight)
		{
			lightDirection = runtimeLight.Component.Direction;
		}
		else
		{
			lightDirection = XMFLOAT3(0.25f, 1.0f, -0.25f);
		}
		XMVECTOR dir = XMVectorSet(lightDirection.x, lightDirection.y, lightDirection.z, 0.0f);
		if (XMVectorGetX(XMVector3LengthSq(dir)) < 0.000001f)
		{
			dir = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
		}
		dir = XMVector3Normalize(dir);

		XMVECTOR target = XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f);
		float focusRadius = 12.0f;
		BuildShadowFocusBounds(target, focusRadius);

		LightType lightType;
		if (runtimeLight.HasLight)
		{
			lightType = runtimeLight.Component.Type;
		}
		else
		{
			lightType = LightType::Directional;
		}
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
		XMMATRIX lightProjection;
		if (useOrthographic)
		{
			lightProjection = XMMatrixOrthographicLH(orthoSize, orthoSize, nearClip, farClip);
		}
		else
		{
			lightProjection = XMMatrixPerspectiveFovLH(fovY, 1.0f, nearClip, farClip);
		}

		UINT shadowSize;
		if ((lightType == LightType::Directional))
		{
			shadowSize = RendererState::g_kSHADOW_MAP_SIZE;
		}
		else
		{
			shadowSize = RendererState::g_kSHADOW_MAP_SIZE_SMALL;
		}
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
		UINT actualLevel;
		if (conventionalCascade)
		{
			actualLevel = level - 4;
		}
		else
		{
			actualLevel = level;
		}
		const UINT cascadeCount = static_cast<UINT>(RendererSettings::GetShadowCascadeCount());
		const float residentFraction =
			static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION) /
			static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION);


		const float residentRadius =
			RendererSettings::GetVirtualFirstLevelRadius() *
			static_cast<float>(1u << actualLevel);
		float radius;
		if (conventionalCascade)
		{
			radius = RendererSettings::GetShadowDistance() /
				static_cast<float>(1u << max(0, static_cast<int>(cascadeCount - 1 - actualLevel)));
		}
		else
		{
			radius = residentRadius / max(residentFraction, 0.01f);
		}

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
		float planeZ = XMVectorGetX(XMVector3Dot(camera, lightDirection));
		const float pageGrid = static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION);
		const float pageWorldSize = (radius * 2.0f) / pageGrid;

		if (RendererSettings::GetStabilizeVirtualClipmaps())
		{
			float snapSize;
			if (conventionalCascade)
			{
				snapSize = (radius * 2.0f) / static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE);
			}
			else
			{
				snapSize = pageWorldSize;
			}
			planeX = roundf(planeX / snapSize) * snapSize;
			planeY = roundf(planeY / snapSize) * snapSize;


			planeZ = roundf(planeZ / snapSize) * snapSize;
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


		const float biasScale = (300.0f - 0.1f) / max(shadowFarClip - 0.1f, 0.0001f);
		float virtualLevelBiasScale;
		if (conventionalCascade)
		{
			virtualLevelBiasScale = 1.0f;
		}
		else
		{
			virtualLevelBiasScale = static_cast<float>(1u << actualLevel);
		}
		outShadowMapParams = XMFLOAT4(
			1.0f / static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE),
			RendererSettings::GetShadowDepthBias() * biasScale * virtualLevelBiasScale,
			RendererSettings::GetShadowNormalBias() * biasScale * virtualLevelBiasScale,
			1.0f);
	}

}

void LightingResources::BeginFrame()
{
	++g_FrameSerial;
	g_LightCacheValid = false;
	m_TransientCbSlot = 0;
}

UINT LightingResources::GetShadowLightCount()
{
	if (!g_LightCacheValid)
	{
		RebuildLightCache();
	}
	return g_ShadowRenderPassCount;
}

void LightingResources::SetCurrentShadowPassIndex(UINT index)
{
	if ((g_ShadowRenderPassCount > 0))
	{
		g_CurrentShadowPassIndex = min(index, g_ShadowRenderPassCount - 1);
	}
	else
	{
		g_CurrentShadowPassIndex = 0;
	}
}

XMMATRIX LightingResources::GetCurrentShadowViewProjection()
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

D3D12_GPU_VIRTUAL_ADDRESS LightingResources::GetShadowConstantBufferAddress(UINT shadowIndex)
{
	if (!m_ShadowConstantBuffer)
	{
		return 0;
	}
	return m_ShadowConstantBuffer->GetGPUVirtualAddress() +
		GetShadowConstantBufferSlot(shadowIndex) * g_kSHADOW_CB_ALIGNED_SIZE;
}

D3D12_GPU_VIRTUAL_ADDRESS LightingResources::GetCurrentShadowConstantBufferAddress()
{
	return GetShadowConstantBufferAddress(g_CurrentShadowPassIndex);
}

D3D12_GPU_VIRTUAL_ADDRESS LightingResources::GetCurrentLightConstantBufferAddress()
{
	if (!m_LightConstantBuffer)
	{
		return 0;
	}
	return m_LightConstantBuffer->GetGPUVirtualAddress() +
		GraphicsDevice::GetFrameIndex() * g_kLIGHT_CB_ALIGNED_SIZE;
}

D3D12_GPU_VIRTUAL_ADDRESS LightingResources::GetCurrentLightTileIndexBufferAddress()
{
	return LightInstanceBuilder::GetTileIndexAddress(GraphicsDevice::GetFrameIndex());
}

D3D12_GPU_VIRTUAL_ADDRESS LightingResources::GetCurrentVolumetricLightIndexBufferAddress()
{
	return LightInstanceBuilder::GetVolumetricIndexAddress(GraphicsDevice::GetFrameIndex());
}

const LightingResources::LightGridStats& LightingResources::GetLightGridStats()
{
	return g_LightGridStats;
}

void LightingResources::UpdateLightConstantBuffer(float deferredLightStrength)
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


	const RuntimeLightState& directionalLight = GetCachedLightState(true);
	const RuntimeLightState* runtimeLightSelection = nullptr;
	if (directionalLight.HasLight)
	{
		runtimeLightSelection = &(directionalLight);
	}
	else
	{
		runtimeLightSelection = &(GetCachedLightState(false));
	}
	const RuntimeLightState& runtimeLight = *runtimeLightSelection;
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
		static_cast<float>(RendererSettings::GetContactShadowsEnabled()),
		RendererSettings::GetContactShadowLength(),
		static_cast<float>(RendererSettings::GetContactShadowSteps()),
		static_cast<float>(RendererSettings::GetShadowMapMethod()));
	constants.ShadowDebugGlobal = XMFLOAT4(
		static_cast<float>(RendererSettings::GetVirtualShadowDebugMode()),
		static_cast<float>(g_VirtualShadowCacheHit),
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
	float distanceFieldLightCount;
	if (RendererSettings::GetDistanceFieldShadowsEnabled())
	{
		distanceFieldLightCount = static_cast<float>(distanceFieldCount);
	}
	else
	{
		distanceFieldLightCount = 0.0f;
	}
	constants.DistanceFieldGlobal = XMFLOAT4(
		distanceFieldLightCount,
		RendererSettings::GetDistanceFieldShadowDistance(),
		static_cast<float>(RendererSettings::GetDistanceFieldShadowSteps()),
		0.0f);
	const auto& localFogVolumes = LocalHeightFog::GetVolumes();
	const UINT localFogCount = min(static_cast<UINT>(localFogVolumes.size()), RendererState::g_kMAX_LOCAL_HEIGHT_FOG_VOLUMES);
	for (UINT i = 0; i < localFogCount; ++i)
	{
		const auto& volume = localFogVolumes[i];
		constants.LocalFogData0[i] = XMFLOAT4(volume.Position.x, volume.Position.y, volume.Position.z, max(volume.Radius, 0.01f));
		constants.LocalFogData1[i] = XMFLOAT4(max(volume.HeightFalloff, 0.0f), max(volume.Density, 0.0f), static_cast<float>(volume.Shape), static_cast<float>(volume.Enabled));
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
		UINT slotBegin;
		if (decal)
		{
			slotBegin = physicalTileLightBudget;
		}
		else
		{
			slotBegin = 0u;
		}
		UINT slotEnd;
		if (decal)
		{
			slotEnd = physicalTileLightBudget + decalTileLightBudget;
		}
		else
		{
			slotEnd = physicalTileLightBudget;
		}
		if (light.AffectsOpaque || light.AffectsForward || enableVolumetrics)
		{
			LightInstanceBuilder::Input instance{};
			instance.TileBounds = XMUINT4(
				candidate.MinTileX, candidate.MinTileY,
				candidate.MaxTileX, candidate.MaxTileY);
			instance.Metadata = XMUINT4(
				lightIndex, slotBegin, slotEnd, static_cast<UINT>(enableVolumetrics));
			lightInstances.push_back(instance);
		}
		constants.LightFlags[lightIndex] = XMFLOAT4(
			static_cast<float>(light.AffectsOpaque),
			static_cast<float>(light.AffectsForward),
			static_cast<float>(enableVolumetrics),
			static_cast<float>(light.RenderMode));


		constants.LightShadowData[lightIndex] = XMFLOAT4(-1.0f, 0.0f, 0.0f, 0.0f);
		const int shadowIndex = FindShadowLightIndex(candidate.Entity);
		if (shadowIndex >= 0)
		{
			constants.LightViewProjections[lightIndex] = g_ShadowLightViewProjections[shadowIndex];
			float gridWidth;
			if ((candidate.Entity == g_VirtualShadowLightEntity))
			{
				gridWidth = -max(g_ShadowMapParams[shadowIndex].z, 0.0000001f);
			}
			else
			{
				gridWidth = g_ShadowMapParams[shadowIndex].z;
			}
			constants.LightShadowData[lightIndex] = XMFLOAT4(
				static_cast<float>(shadowIndex),
				g_ShadowMapParams[shadowIndex].x,
				g_ShadowMapParams[shadowIndex].y,
				gridWidth);
		}
	}

	const UINT slotsPerTile = physicalTileLightBudget + decalTileLightBudget;
	bool lightGridAvailable =
		tileCountX * tileCountY <= g_kMAX_LIGHT_TILE_COUNT &&
		slotsPerTile > 0 &&
		LightInstanceBuilder::Build(
			GraphicsDevice::GetCommandList(), GraphicsDevice::GetFrameIndex(),
			lightInstances, tileCountX, tileCountY, slotsPerTile);
	if (lightGridAvailable)
	{
		g_LightGridStats.MaxLightsPerTile = slotsPerTile;
	}
	constants.LocalFogGlobal.y = static_cast<float>(enabledVolumetricLights);

	float gridHeight;
	if (lightGridAvailable)
	{
		gridHeight = static_cast<float>(tileCountX);
	}
	else
	{
		gridHeight = 0.0f;
	}
	float gridLightCount;
	if (lightGridAvailable)
	{
		gridLightCount = static_cast<float>(tileCountY);
	}
	else
	{
		gridLightCount = 0.0f;
	}
	float shader6;
	if (lightGridAvailable)
	{
		shader6 = static_cast<float>(slotsPerTile);
	}
	else
	{
		shader6 = 0.0f;
	}
	constants.LightCount = XMFLOAT4(
		static_cast<float>(lightCount),
		gridHeight,
		gridLightCount,
		shader6);
	auto* lightDst = static_cast<UINT8*>(m_pLightCbvDataBegin) +
		GraphicsDevice::GetFrameIndex() * g_kLIGHT_CB_ALIGNED_SIZE;
	memcpy(lightDst, &constants, sizeof(constants));


	MaterialBindings::UpdateDeferredLightingMaterial();

	g_LightConstantsSerial = g_FrameSerial;
	g_LightConstantsStrength = deferredLightStrength;
}

void LightingResources::UpdateShadowConstantBuffer()
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

bool LightingResources::ShouldRenderShadowPass(UINT shadowIndex)
{
	if (!g_LightCacheValid) RebuildLightCache();
	if (shadowIndex >= g_ShadowRenderPassCount) return false;
	const ShadowRenderPass& pass = g_ShadowRenderPasses[shadowIndex];
	return !pass.VirtualPage || pass.NeedsRender;
}

bool LightingResources::ShouldDrawEntityInCurrentShadowPass(EntityID entity)
{
	if (!g_LightCacheValid) RebuildLightCache();
	if (g_CurrentShadowPassIndex >= g_ShadowRenderPassCount) return true;
	const ShadowRenderPass& pass = g_ShadowRenderPasses[g_CurrentShadowPassIndex];
	return IsShadowCasterVisible(entity, pass.ViewProjection);
}

bool LightingResources::IsCurrentShadowPassVirtualPage()
{
	if (!g_LightCacheValid) RebuildLightCache();
	return g_CurrentShadowPassIndex < g_ShadowRenderPassCount &&
		g_ShadowRenderPasses[g_CurrentShadowPassIndex].VirtualPage;
}

UINT LightingResources::GetCurrentShadowLodBias()
{


	return 0;
}

bool LightingResources::IsVirtualShadowCacheHit()
{
	if (!g_LightCacheValid) RebuildLightCache();
	return g_VirtualShadowCacheHit;
}

bool LightingResources::GetShadowPassInfo(UINT shadowIndex, UINT& layer, D3D12_VIEWPORT& viewport, D3D12_RECT& scissor, bool& clearLayer)
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
