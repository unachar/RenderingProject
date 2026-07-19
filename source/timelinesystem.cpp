#include "pch.h"
#include "timelinesystem.h"
#include "world.h"
#include "componentmanager.h"
#include "projectmanager.h"
#include <algorithm>


	float ApplyEase(float value, TimelineInterpolation interpolation)
	{
		value = clamp(value, 0.0f, 1.0f);
		switch (interpolation)
		{
		case TimelineInterpolation::Step: return 0.0f;
		case TimelineInterpolation::EaseIn: return value * value;
		case TimelineInterpolation::EaseOut:
			return 1.0f - (1.0f - value) * (1.0f - value);
		case TimelineInterpolation::EaseInOut:
			return value < 0.5f
				? 2.0f * value * value
				: 1.0f - powf(-2.0f * value + 2.0f, 2.0f) * 0.5f;
		default: return value;
		}
	}

	XMFLOAT4 LerpValue(const XMFLOAT4& a, const XMFLOAT4& b, float t)
	{
		return
		{
			a.x + (b.x - a.x) * t,
			a.y + (b.y - a.y) * t,
			a.z + (b.z - a.z) * t,
			a.w + (b.w - a.w) * t
		};
	}

	XMFLOAT4 SampleClip(const TimelineClipData& clip, float localTime)
	{
		if (clip.Keys.empty())
		{
			return {};
		}
		if (localTime <= clip.Keys.front().Time)
		{
			return clip.Keys.front().Value;
		}
		for (size_t i = 1; i < clip.Keys.size(); ++i)
		{
			const auto& right = clip.Keys[i];
			if (localTime <= right.Time)
			{
				const auto& left = clip.Keys[i - 1];
				const float span = max(0.00001f, right.Time - left.Time);
				const float t = ApplyEase(
					(localTime - left.Time) / span, left.Interpolation);
				return LerpValue(left.Value, right.Value, t);
			}
		}
		return clip.Keys.back().Value;
	}

	void ApplyProperty(EntityID target, TimelineProperty property, const XMFLOAT4& value)
	{
		if (!Registry::IsAlive(target))
		{
			return;
		}
		if (property <= TimelineProperty::TransformScale &&
			ComponentManager::HasComponent<TransformComponent>(target))
		{
			auto& transform =
				ComponentManager::GetComponentUnchecked<TransformComponent>(target);
			XMFLOAT3* destination = property == TimelineProperty::TransformPosition
				? &transform.Position
				: (property == TimelineProperty::TransformRotation
					? &transform.Rotation : &transform.Scale);
			*destination = { value.x, value.y, value.z };
			transform.IsDirty = true;
			return;
		}
		if (property >= TimelineProperty::CameraTarget &&
			property <= TimelineProperty::CameraLockOnOffset &&
			ComponentManager::HasComponent<CameraComponent>(target))
		{
			auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(target);
			switch (property)
			{
			case TimelineProperty::CameraTarget:
				camera.Target = { value.x, value.y, value.z }; break;
			case TimelineProperty::CameraFov:
				camera.Fov = clamp(value.x, XMConvertToRadians(1.0f), XMConvertToRadians(179.0f)); break;
			case TimelineProperty::CameraNearClip:
				camera.NearClip = max(0.001f, value.x); break;
			case TimelineProperty::CameraFarClip:
				camera.FarClip = max(camera.NearClip + 0.001f, value.x); break;
			case TimelineProperty::CameraLockOnOffset:
				camera.LockOnOffset = { value.x, value.y, value.z }; break;
			default: break;
			}
			return;
		}
		if (property == TimelineProperty::PostProcessIntensity &&
			ComponentManager::HasComponent<PostProcessComponent>(target))
		{
			ComponentManager::GetComponentUnchecked<PostProcessComponent>(
				target).Intensity = clamp(value.x, 0.0f, 1.0f);
		}
	}


vector<unique_ptr<TimeLineDirector>> TimeLineSystem::m_Directors;

void TimeLineSystem::Update()
{
	const float deltaTime = World::GetDeltaTime();
	for (auto& director : m_Directors)
	{
		if (director)
		{
			director->Update(deltaTime);
		}
	}

	ComponentManager::ForEachComponent<TimelineComponent>(
		[&](EntityID, TimelineComponent& timeline)
		{
			const uint64_t session = ProjectManager::GetPlaySession();
			if (timeline.LastPlaySession != session)
			{
				timeline.LastPlaySession = session;
				timeline.CurrentTime = 0.0f;
				timeline.IsPlaying = timeline.PlayOnAwake;
			}
			if (timeline.IsPlaying)
			{
				timeline.CurrentTime += deltaTime * timeline.Speed;
				if (timeline.CurrentTime > timeline.Duration)
				{
					if (timeline.Loop && timeline.Duration > 0.0f)
					{
						timeline.CurrentTime = fmodf(timeline.CurrentTime, timeline.Duration);
					}
					else
					{
						timeline.CurrentTime = timeline.Duration;
						timeline.IsPlaying = false;
					}
				}
			}
			EvaluateComponent(timeline);
		});
}

void TimeLineSystem::Uninit()
{
	Clear();
}

TimeLineDirector& TimeLineSystem::CreateDirector()
{
	auto director = make_unique<TimeLineDirector>();
	TimeLineDirector& ref = *director;
	m_Directors.push_back(move(director));
	return ref;
}

void TimeLineSystem::AddDirector(unique_ptr<TimeLineDirector> director)
{
	if (director)
	{
		m_Directors.push_back(move(director));
	}
}

void TimeLineSystem::Clear()
{
	m_Directors.clear();
}

size_t TimeLineSystem::GetDirectorCount()
{
	return m_Directors.size();
}

XMFLOAT4 TimeLineSystem::ReadProperty(
	EntityID target,
	TimelineProperty property,
	bool* valid)
{
	if (valid) *valid = false;
	if (!Registry::IsAlive(target))
	{
		return {};
	}
	if (property <= TimelineProperty::TransformScale &&
		ComponentManager::HasComponent<TransformComponent>(target))
	{
		const auto& transform =
			ComponentManager::GetComponentUnchecked<TransformComponent>(target);
		const XMFLOAT3& value = property == TimelineProperty::TransformPosition
			? transform.Position
			: (property == TimelineProperty::TransformRotation
				? transform.Rotation : transform.Scale);
		if (valid) *valid = true;
		return { value.x, value.y, value.z, 0.0f };
	}
	if (property >= TimelineProperty::CameraTarget &&
		property <= TimelineProperty::CameraLockOnOffset &&
		ComponentManager::HasComponent<CameraComponent>(target))
	{
		const auto& camera =
			ComponentManager::GetComponentUnchecked<CameraComponent>(target);
		if (valid) *valid = true;
		switch (property)
		{
		case TimelineProperty::CameraTarget:
			return { camera.Target.x, camera.Target.y, camera.Target.z, 0 };
		case TimelineProperty::CameraFov: return { camera.Fov, 0, 0, 0 };
		case TimelineProperty::CameraNearClip: return { camera.NearClip, 0, 0, 0 };
		case TimelineProperty::CameraFarClip: return { camera.FarClip, 0, 0, 0 };
		case TimelineProperty::CameraLockOnOffset:
			return { camera.LockOnOffset.x, camera.LockOnOffset.y, camera.LockOnOffset.z, 0 };
		default: break;
		}
	}
	if (property == TimelineProperty::PostProcessIntensity &&
		ComponentManager::HasComponent<PostProcessComponent>(target))
	{
		if (valid) *valid = true;
		return
		{
			ComponentManager::GetComponentUnchecked<PostProcessComponent>(
				target).Intensity, 0, 0, 0
		};
	}
	return {};
}

void TimeLineSystem::EvaluateComponent(TimelineComponent& timeline)
{
	for (auto& track : timeline.Tracks)
	{
		if (!track.Enabled || !Registry::IsAlive(track.Target))
		{
			continue;
		}
		XMFLOAT4 accumulated{};
		float totalWeight = 0.0f;
		for (auto& clip : track.Clips)
		{
			if (!clip.Enabled || clip.Duration <= 0.0f ||
				timeline.CurrentTime < clip.StartTime ||
				timeline.CurrentTime > clip.StartTime + clip.Duration ||
				clip.Keys.empty())
			{
				continue;
			}
			const float local = timeline.CurrentTime - clip.StartTime;
			float weight = 1.0f;
			if (clip.BlendIn > 0.0f) weight = min(weight, local / clip.BlendIn);
			if (clip.BlendOut > 0.0f)
				weight = min(weight, (clip.Duration - local) / clip.BlendOut);
			weight = clamp(weight, 0.0f, 1.0f);
			const XMFLOAT4 value = SampleClip(clip, local);
			accumulated.x += value.x * weight;
			accumulated.y += value.y * weight;
			accumulated.z += value.z * weight;
			accumulated.w += value.w * weight;
			totalWeight += weight;
		}
		if (totalWeight <= 0.0f)
		{
			continue;
		}
		if (totalWeight < 1.0f && track.HasDefaultValue)
		{
			const float baseWeight = 1.0f - totalWeight;
			accumulated.x += track.DefaultValue.x * baseWeight;
			accumulated.y += track.DefaultValue.y * baseWeight;
			accumulated.z += track.DefaultValue.z * baseWeight;
			accumulated.w += track.DefaultValue.w * baseWeight;
			totalWeight = 1.0f;
		}
		const float invWeight = 1.0f / totalWeight;
		accumulated.x *= invWeight;
		accumulated.y *= invWeight;
		accumulated.z *= invWeight;
		accumulated.w *= invWeight;
		ApplyProperty(track.Target, track.Property, accumulated);
	}
}
