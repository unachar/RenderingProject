#include "pch.h"
#include "lightsystem.h"
#include "componentmanager.h"
#include "world.h"
#include "light.h"
#include "sun.h"

namespace
{
	void EnsureDefaultLight()
	{
		if (!World::GetView<LightComponent>().empty())
		{
			return;
		}

		Sun::CreateDefault();
	}
}

void LightSystem::Init()
{
	EnsureDefaultLight();
}

void LightSystem::Update()
{
	Sun::SyncAll();
	for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
	{
		auto& lightComponent = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
		if (!lightComponent.IsActive)
		{
			continue;
		}

		const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		if (lightComponent.Type == LightType::Spot || lightComponent.Type == LightType::Volume)
		{
			XMFLOAT3 target = { 0.0f, 0.0f, 0.0f };
			Entity alicia = World::GetEntityByName("Alicia");
			if (alicia.IsValid() &&
				Registry::IsAlive(alicia.GetID()) &&
				ComponentManager::HasComponent<TransformComponent>(alicia.GetID()))
			{
				const auto& aliciaTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(alicia.GetID());
				target = { aliciaTransform.Position.x, aliciaTransform.Position.y, aliciaTransform.Position.z };
			}

			XMVECTOR dir = XMVectorSubtract(XMLoadFloat3(&target), XMLoadFloat3(&transform.Position));
			if (XMVectorGetX(XMVector3LengthSq(dir)) > 0.000001f)
			{
				XMStoreFloat3(&lightComponent.Direction, XMVector3Normalize(dir));
			}
		}
	}
}

void LightSystem::SetPBRParam(EntityID entity, float metallic, float roughness, float fresnel)
{
	if (!Registry::IsAlive(entity) || !ComponentManager::HasComponent<LightComponent>(entity))
	{
		return;
	}
	auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
	light.Color.x = metallic;
	light.Color.y = roughness;
	light.Color.z = fresnel;
}

