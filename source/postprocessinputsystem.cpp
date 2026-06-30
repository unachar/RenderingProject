#include "pch.h"
#include "postprocessinputsystem.h"
#include "componentmanager.h"
#include "world.h"

namespace
{
	void ApplyPostProcessFromInput(const InputComponent& input, PostProcessComponent& post, float dt, float minIntensity)
	{
		if (!input.IsActive)
		{
			return;
		}

		if (input.PostProcessNone) post.Type = PostProcessType::NONE;
		if (input.PostProcessBlur) post.Type = PostProcessType::BLUR;
		if (input.PostProcessSepia) post.Type = PostProcessType::SEPIA;
		if (input.PostProcessGrayscale) post.Type = PostProcessType::GRAYSCALE;
		if (input.PostProcessInvert) post.Type = PostProcessType::INVERT;

		float change = 0.0f;
		if (input.IntensityUp) change += 1.0f;
		if (input.IntensityDown) change -= 1.0f;

		if (change == 0.0f)
		{
			return;
		}

		post.Intensity += change * dt;
		if (post.Intensity < minIntensity) post.Intensity = minIntensity;
		if (post.Intensity > 1.0f) post.Intensity = 1.0f;
	}
}

void PostProcessInputSystem::Update()
{
	float dt = World::GetDeltaTime();

	for (EntityID i : World::GetView<InputComponent, PostProcessComponent>())
	{
		ApplyPostProcessFromInput(
			ComponentManager::GetComponentUnchecked<InputComponent>(i),
			ComponentManager::GetComponentUnchecked<PostProcessComponent>(i),
			dt,
			0.0f);
	}

	for (EntityID i : World::GetView<CameraComponent, InputComponent, PostProcessComponent>())
	{
		ApplyPostProcessFromInput(
			ComponentManager::GetComponentUnchecked<InputComponent>(i),
			ComponentManager::GetComponentUnchecked<PostProcessComponent>(i),
			dt,
			-1.0f);
	}
}
