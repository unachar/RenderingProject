#include "pch.h"
#include "animationsystem.h"
#include "animator.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "world.h"
#include <unordered_set>

void AnimationSystem::Update()
{
	auto animEntities = World::GetView<AnimationModelComponent>();
	std::unordered_set<int> updatedInstancedModels;

	for (EntityID i : animEntities)
	{
		auto& animationComponent = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(i);
		if (animationComponent.ModelId < 0)
		{
			continue;
		}

		AnimationModelResource* model = ModelManager::GetAnimModel(animationComponent.ModelId);
		if (!model)
		{
			continue;
		}

		if (animationComponent.CurrentAnimation.empty() &&
			animationComponent.ActiveAnimationLayers.empty() &&
			!animationComponent.Animations.empty())
		{
			Animator::Play(animationComponent, animationComponent.Animations[0]);
		}
		if (animationComponent.CurrentAnimation.empty() && animationComponent.ActiveAnimationLayers.empty())
		{
			continue;
		}

		Animator::Update(animationComponent, World::GetDeltaTime());

		const bool useSharedInstancedPose =
			ComponentManager::HasComponent<InstancingComponent>(i) &&
			ComponentManager::GetComponentUnchecked<InstancingComponent>(i).UseInstancing;
		if (useSharedInstancedPose && !updatedInstancedModels.insert(animationComponent.ModelId).second)
		{
			// GPU-instanced entities referencing the same model share one skinned
			// vertex stream, so evaluating the identical pose per Entity is wasted work.
			continue;
		}

		if (animationComponent.ActiveAnimationLayers.size() > 1)
		{
			model->UpdateBoneMatrices(animationComponent.ActiveAnimationLayers);
			continue;
		}

		const string& nextAnimation = animationComponent.NextAnimation.empty()
			? animationComponent.CurrentAnimation
			: animationComponent.NextAnimation;
		const float nextTime = animationComponent.NextAnimation.empty()
			? animationComponent.CurrentTime
			: animationComponent.NextTime;

		model->UpdateBoneMatrices(
			animationComponent.CurrentAnimation.c_str(), animationComponent.CurrentTime,
			nextAnimation.c_str(), nextTime, animationComponent.BlendRate);
	}
}
