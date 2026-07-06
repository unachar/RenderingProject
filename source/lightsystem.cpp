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
		if (!World::GetView<SunComponent>().empty())
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
		lightComponent.Position = transform.Position;
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

