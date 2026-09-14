#include "pch.h"
#include "animationsystem.h"
#include "animator.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "world.h"
#include "camera.h"
#include "instancingsystem.h"
#include <unordered_set>

void AnimationSystem::Update()
{
	auto animEntities = World::GetView<AnimationModelComponent>();
	unordered_set<int> updatedInstancedModels;

	static uint64_t s_AnimLodFrame = 0;
	++s_AnimLodFrame;

	// カメラ位置を1回だけ取得 (距離LOD用)。
	bool hasCameraPosition = false;
	XMFLOAT3 cameraPosition{};
	const EntityID cameraEntity = Camera::GetCameraEntity();
	if (ComponentManager::HasComponent<TransformComponent>(cameraEntity))
	{
		cameraPosition = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
		hasCameraPosition = true;
	}

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

		// 遠距離モデルは1フレームおきに更新して CPU スキニング負荷を半減する。
		if (hasCameraPosition && ComponentManager::HasComponent<TransformComponent>(i))
		{
			if (!InstancingSystem::IsEntityVisible(i))
			{
				continue;
			}

			float lodFarDistance = 35.0f;
			if (ComponentManager::HasComponent<LODComponent>(i))
			{
				const auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(i);
				if (lod.UseLOD)
				{
					lodFarDistance = max(lod.Lod2Distance, lod.Lod1Distance);
				}
			}
			const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(i);
			const float dx = transform.Position.x - cameraPosition.x;
			const float dy = transform.Position.y - cameraPosition.y;
			const float dz = transform.Position.z - cameraPosition.z;
			const float distanceSq = dx * dx + dy * dy + dz * dz;
			if (distanceSq > lodFarDistance * lodFarDistance &&
				((static_cast<uint64_t>(i) + s_AnimLodFrame) & 1ULL) != 0ULL)
			{
				continue;
			}
		}

		const bool useSharedInstancedPose =
			ComponentManager::HasComponent<InstancingComponent>(i) &&
			ComponentManager::GetComponentUnchecked<InstancingComponent>(i).UseInstancing;
		if (useSharedInstancedPose && !updatedInstancedModels.insert(animationComponent.ModelId).second)
		{
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
