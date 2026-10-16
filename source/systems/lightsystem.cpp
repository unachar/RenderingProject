#include "pch.h"
#include "lightsystem.h"
#include "componentmanager.h"
#include "world.h"
#include "light.h"
#include "sun.h"
#include "daynightcycle.h"


	void EnsureDefaultLight()
	{
		if (!World::GetView<SunComponent>().empty())
		{
			return;
		}

		Sun::CreateDefault();
	}


void LightSystem::Init()
{
	EnsureDefaultLight();
}

void LightSystem::Update()
{
	if (!Sun::MigrateLegacyGeometryBeforeFrame())
	{
		Debug::Log("Sun: legacy geometry migration could not wait for the GPU.\n");
	}
	DayNightCycle::Update(World::GetDeltaTime());
	ComponentManager::ForEach<LightComponent, TransformComponent>(
		[](EntityID, LightComponent& lightComponent, TransformComponent& transform)
		{
		if (!lightComponent.IsActive || lightComponent.Intensity <= 0.0f)
		{
			return;
		}

		lightComponent.Position = transform.Position;
		});
}

