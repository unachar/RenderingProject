#include "pch.h"
#include "animationsystem.h"
#include "animator.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "world.h"

void AnimationSystem::Update()
{
	auto animEntities = World::GetView<AnimationModelComponent>();

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

		if (animationComponent.CurrentAnimation.empty() && !animationComponent.Animations.empty())
		{
			Animator::Play(animationComponent, animationComponent.Animations[0]);
		}
		if (animationComponent.CurrentAnimation.empty())
		{
			continue;
		}

		Animator::Update(animationComponent, World::GetDeltaTime());

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
