#include "pch.h"
#include "projectmanager.h"
#include "modelmanager.h"
#include "world.h"

void ProjectManager::Init()
{
	s_State = ProjectPlayState::Edit;
	s_RuntimeSnapshots.clear();
	s_PlaySession = 0;
}

void ProjectManager::Uninit()
{
	s_RuntimeSnapshots.clear();
	s_State = ProjectPlayState::Edit;
}

void ProjectManager::CaptureRuntimeState()
{
	s_RuntimeSnapshots.clear();
	const auto& entities = Registry::GetEntities();
	for (EntityID entity = 0; entity < static_cast<EntityID>(entities.size()); ++entity)
	{
		if (!entities[entity].IsAlive)
		{
			continue;
		}

		RuntimeSnapshot snapshot{};
		snapshot.Entity = entity;
		snapshot.HasTransform = ComponentManager::HasComponent<TransformComponent>(entity);
		if (snapshot.HasTransform)
		{
			snapshot.Transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		}
		snapshot.HasAnimation = ComponentManager::HasComponent<AnimationModelComponent>(entity);
		if (snapshot.HasAnimation)
		{
			snapshot.Animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
		}
		snapshot.HasCamera = ComponentManager::HasComponent<CameraComponent>(entity);
		if (snapshot.HasCamera)
			snapshot.Camera = ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
		snapshot.HasPostProcess = ComponentManager::HasComponent<PostProcessComponent>(entity);
		if (snapshot.HasPostProcess)
			snapshot.PostProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(entity);
		snapshot.HasTimeline = ComponentManager::HasComponent<TimelineComponent>(entity);
		if (snapshot.HasTimeline)
			snapshot.Timeline = ComponentManager::GetComponentUnchecked<TimelineComponent>(entity);
		s_RuntimeSnapshots.push_back(std::move(snapshot));
	}
}

void ProjectManager::RestoreRuntimeState()
{
	for (const RuntimeSnapshot& snapshot : s_RuntimeSnapshots)
	{
		if (!Registry::IsAlive(snapshot.Entity))
		{
			continue;
		}
		if (snapshot.HasTransform && ComponentManager::HasComponent<TransformComponent>(snapshot.Entity))
		{
			auto restored = snapshot.Transform;
			restored.IsDirty = true;
			ComponentManager::GetComponentUnchecked<TransformComponent>(snapshot.Entity) = restored;
		}
		if (snapshot.HasAnimation && ComponentManager::HasComponent<AnimationModelComponent>(snapshot.Entity))
		{
			ComponentManager::GetComponentUnchecked<AnimationModelComponent>(snapshot.Entity) = snapshot.Animation;
			AnimationModelResource* model = ModelManager::GetAnimModel(snapshot.Animation.ModelId);
			if (!model)
			{
				continue;
			}

			// Physics writes directly into the shared skeletal resource. Force the
			// saved edit-time pose back immediately, even when the cached animation
			// time is identical to the time at which play mode was entered.
			model->InvalidateAnimationPoseCache();
			if (!snapshot.Animation.ActiveAnimationLayers.empty())
			{
				model->UpdateBoneMatrices(snapshot.Animation.ActiveAnimationLayers);
			}
			else if (!snapshot.Animation.CurrentAnimation.empty())
			{
				const string& nextAnimation = snapshot.Animation.NextAnimation.empty()
					? snapshot.Animation.CurrentAnimation
					: snapshot.Animation.NextAnimation;
				const float nextTime = snapshot.Animation.NextAnimation.empty()
					? snapshot.Animation.CurrentTime
					: snapshot.Animation.NextTime;
				model->UpdateBoneMatrices(
					snapshot.Animation.CurrentAnimation.c_str(), snapshot.Animation.CurrentTime,
					nextAnimation.c_str(), nextTime, snapshot.Animation.BlendRate);
			}
			else
			{
				model->ResetBoneMatricesToBindPose();
			}
		}
		if (snapshot.HasCamera && ComponentManager::HasComponent<CameraComponent>(snapshot.Entity))
			ComponentManager::GetComponentUnchecked<CameraComponent>(snapshot.Entity) = snapshot.Camera;
		if (snapshot.HasPostProcess && ComponentManager::HasComponent<PostProcessComponent>(snapshot.Entity))
			ComponentManager::GetComponentUnchecked<PostProcessComponent>(snapshot.Entity) = snapshot.PostProcess;
		if (snapshot.HasTimeline && ComponentManager::HasComponent<TimelineComponent>(snapshot.Entity))
			ComponentManager::GetComponentUnchecked<TimelineComponent>(snapshot.Entity) = snapshot.Timeline;
	}
	s_RuntimeSnapshots.clear();
}

void ProjectManager::Play()
{
	if (s_State != ProjectPlayState::Edit)
	{
		return;
	}
	CaptureRuntimeState();
	++s_PlaySession;
	if (s_PlaySession == 0)
	{
		s_PlaySession = 1;
	}
	s_State = ProjectPlayState::Playing;
}

void ProjectManager::Stop()
{
	if (s_State == ProjectPlayState::Edit)
	{
		return;
	}
	RestoreRuntimeState();
	s_State = ProjectPlayState::Edit;
}

void ProjectManager::TogglePlay()
{
	if (s_State == ProjectPlayState::Edit)
	{
		Play();
	}
	else
	{
		Stop();
	}
}

void ProjectManager::TogglePause()
{
	if (s_State == ProjectPlayState::Playing)
	{
		s_State = ProjectPlayState::Paused;
	}
	else if (s_State == ProjectPlayState::Paused)
	{
		s_State = ProjectPlayState::Playing;
	}
}
