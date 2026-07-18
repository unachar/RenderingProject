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
	ComponentManager::ForEach<LightComponent, TransformComponent>(
		[](EntityID, LightComponent& lightComponent, TransformComponent& transform)
		{
		if (!lightComponent.IsActive)
		{
			return;
		}

		lightComponent.Position = transform.Position;
		});
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

