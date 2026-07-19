#include "pch.h"
#include "timeline.h"
#include <cmath>


	float Clamp01(float value)
	{
		if (value < 0.0f) return 0.0f;
		if (value > 1.0f) return 1.0f;
		return value;
	}

	float ApplyEase(float t, TimeLineEase ease)
	{
		t = Clamp01(t);
		switch (ease)
		{
		case TimeLineEase::Step:
			return 0.0f;
		case TimeLineEase::EaseIn:
			return t * t;
		case TimeLineEase::EaseOut:
			return 1.0f - (1.0f - t) * (1.0f - t);
		case TimeLineEase::EaseInOut:
			return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) * 0.5f;
		case TimeLineEase::Linear:
		default:
			return t;
		}
	}

	XMFLOAT3 LerpVector(const XMFLOAT3& a, const XMFLOAT3& b, float t)
	{
		return
		{
			a.x + (b.x - a.x) * t,
			a.y + (b.y - a.y) * t,
			a.z + (b.z - a.z) * t
		};
	}

	float ClampTime(float time, float duration)
	{
		if (time < 0.0f) return 0.0f;
		if (duration > 0.0f && time > duration) return duration;
		return time;
	}


void TimeLineVectorCurve::AddKey(float time, const XMFLOAT3& value, TimeLineEase ease)
{
	TimeLineVectorKeyFrame key;
	key.Time = time;
	key.Value = value;
	key.Ease = ease;
	m_Keys.push_back(key);
	sort(m_Keys.begin(), m_Keys.end(), [](const TimeLineVectorKeyFrame& a, const TimeLineVectorKeyFrame& b)
		{
			return a.Time < b.Time;
		});
}

XMFLOAT3 TimeLineVectorCurve::Evaluate(float time) const
{
	if (m_Keys.empty())
	{
		return { 0.0f, 0.0f, 0.0f };
	}

	if (time <= m_Keys.front().Time)
	{
		return m_Keys.front().Value;
	}

	if (time >= m_Keys.back().Time)
	{
		return m_Keys.back().Value;
	}

	for (size_t i = 0; i + 1 < m_Keys.size(); ++i)
	{
		const auto& from = m_Keys[i];
		const auto& to = m_Keys[i + 1];
		if (time >= from.Time && time <= to.Time)
		{
			const float length = to.Time - from.Time;
			const float t = length > 0.0f ? (time - from.Time) / length : 0.0f;
			return LerpVector(from.Value, to.Value, ApplyEase(t, from.Ease));
		}
	}

	return m_Keys.back().Value;
}

TimeLineClip::TimeLineClip(float startTime, float duration)
	: m_StartTime(startTime), m_Duration(duration)
{
}

float TimeLineClip::GetLocalTime(float timelineTime) const
{
	return ClampTime(timelineTime - m_StartTime, m_Duration);
}

float TimeLineClip::GetWeight(float timelineTime) const
{
	if (!Contains(timelineTime))
	{
		return 0.0f;
	}

	float weight = 1.0f;
	const float localTime = GetLocalTime(timelineTime);

	if (m_BlendInDuration > 0.0f && localTime < m_BlendInDuration)
	{
		weight = min(weight, Clamp01(localTime / m_BlendInDuration));
	}

	if (m_BlendOutDuration > 0.0f)
	{
		const float timeToEnd = m_Duration - localTime;
		if (timeToEnd < m_BlendOutDuration)
		{
			weight = min(weight, Clamp01(timeToEnd / m_BlendOutDuration));
		}
	}

	return weight;
}

bool TimeLineClip::Contains(float timelineTime) const
{
	return timelineTime >= m_StartTime && timelineTime <= GetEndTime();
}

TimeLineClip& TimeLineClip::SetBlendIn(float duration)
{
	m_BlendInDuration = max(0.0f, duration);
	return *this;
}

TimeLineClip& TimeLineClip::SetBlendOut(float duration)
{
	m_BlendOutDuration = max(0.0f, duration);
	return *this;
}

TransformClip::TransformClip(float startTime, float duration)
	: TimeLineClip(startTime, duration)
{
}

TransformClip& TransformClip::AddPositionKey(float time, const XMFLOAT3& value, TimeLineEase ease)
{
	m_PositionCurve.AddKey(time, value, ease);
	return *this;
}

TransformClip& TransformClip::AddRotationKey(float time, const XMFLOAT3& value, TimeLineEase ease)
{
	m_RotationCurve.AddKey(time, value, ease);
	return *this;
}

TransformClip& TransformClip::AddScaleKey(float time, const XMFLOAT3& value, TimeLineEase ease)
{
	m_ScaleCurve.AddKey(time, value, ease);
	return *this;
}

TransformClip& TransformClip::SetBlendIn(float duration)
{
	TimeLineClip::SetBlendIn(duration);
	return *this;
}

TransformClip& TransformClip::SetBlendOut(float duration)
{
	TimeLineClip::SetBlendOut(duration);
	return *this;
}

TimeLineTransformSample TransformClip::Evaluate(float timelineTime) const
{
	TimeLineTransformSample sample;
	sample.Weight = GetWeight(timelineTime);

	const float localTime = GetLocalTime(timelineTime);
	if (m_PositionCurve.HasKeys())
	{
		sample.HasPosition = true;
		sample.Position = m_PositionCurve.Evaluate(localTime);
	}

	if (m_RotationCurve.HasKeys())
	{
		sample.HasRotation = true;
		sample.Rotation = m_RotationCurve.Evaluate(localTime);
	}

	if (m_ScaleCurve.HasKeys())
	{
		sample.HasScale = true;
		sample.Scale = m_ScaleCurve.Evaluate(localTime);
	}

	return sample;
}

TransformTrack::TransformTrack(EntityID target)
	: m_Target(target)
{
}

XMFLOAT3 TransformTrack::Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
	return LerpVector(a, b, t);
}

void TransformTrack::Accumulate(XMFLOAT3& total, float& totalWeight, const XMFLOAT3& value, float weight)
{
	total.x += value.x * weight;
	total.y += value.y * weight;
	total.z += value.z * weight;
	totalWeight += weight;
}

TransformClip& TransformTrack::AddClip(float startTime, float duration)
{
	m_Clips.emplace_back(startTime, duration);
	return m_Clips.back();
}

void TransformTrack::Evaluate(float time, float previousTime, bool timeAdvanced)
{
	(void)previousTime;
	(void)timeAdvanced;

	if (m_Target == g_kINVALID_ENTITY ||
		!Registry::IsAlive(m_Target) ||
		!ComponentManager::HasComponent<TransformComponent>(m_Target))
	{
		return;
	}

	auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(m_Target);

	XMFLOAT3 positionTotal = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 rotationTotal = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 scaleTotal = { 0.0f, 0.0f, 0.0f };
	float positionWeight = 0.0f;
	float rotationWeight = 0.0f;
	float scaleWeight = 0.0f;

	for (const auto& clip : m_Clips)
	{
		if (!clip.Contains(time))
		{
			continue;
		}

		const auto sample = clip.Evaluate(time);
		if (sample.Weight <= 0.0f)
		{
			continue;
		}

		if (sample.HasPosition)
		{
			Accumulate(positionTotal, positionWeight, sample.Position, sample.Weight);
		}
		if (sample.HasRotation)
		{
			Accumulate(rotationTotal, rotationWeight, sample.Rotation, sample.Weight);
		}
		if (sample.HasScale)
		{
			Accumulate(scaleTotal, scaleWeight, sample.Scale, sample.Weight);
		}
	}

	bool dirty = false;
	if (positionWeight > 0.0f)
	{
		const XMFLOAT3 blended =
		{
			positionTotal.x / positionWeight,
			positionTotal.y / positionWeight,
			positionTotal.z / positionWeight
		};
		transform.Position = positionWeight < 1.0f ? Lerp(transform.Position, blended, positionWeight) : blended;
		dirty = true;
	}

	if (rotationWeight > 0.0f)
	{
		const XMFLOAT3 blended =
		{
			rotationTotal.x / rotationWeight,
			rotationTotal.y / rotationWeight,
			rotationTotal.z / rotationWeight
		};
		transform.Rotation = rotationWeight < 1.0f ? Lerp(transform.Rotation, blended, rotationWeight) : blended;
		dirty = true;
	}

	if (scaleWeight > 0.0f)
	{
		const XMFLOAT3 blended =
		{
			scaleTotal.x / scaleWeight,
			scaleTotal.y / scaleWeight,
			scaleTotal.z / scaleWeight
		};
		transform.Scale = scaleWeight < 1.0f ? Lerp(transform.Scale, blended, scaleWeight) : blended;
		dirty = true;
	}

	if (dirty)
	{
		transform.IsDirty = true;
	}
}

EventTrack& EventTrack::AddEvent(float time, function<void()> callback)
{
	TimeLineEvent event;
	event.Time = time;
	event.Callback = move(callback);
	m_Events.push_back(move(event));
	sort(m_Events.begin(), m_Events.end(), [](const TimeLineEvent& a, const TimeLineEvent& b)
		{
			return a.Time < b.Time;
		});
	return *this;
}

void EventTrack::Evaluate(float time, float previousTime, bool timeAdvanced)
{
	if (!timeAdvanced)
	{
		return;
	}

	for (auto& event : m_Events)
	{
		if (!event.Fired && event.Time > previousTime && event.Time <= time)
		{
			event.Fired = true;
			if (event.Callback)
			{
				event.Callback();
			}
		}
	}
}

void EventTrack::ResetState()
{
	for (auto& event : m_Events)
	{
		event.Fired = false;
	}
}

void TimeLineAsset::SetDuration(float duration)
{
	m_Duration = max(0.0f, duration);
}

TransformTrack& TimeLineAsset::AddTransformTrack(EntityID entity)
{
	auto track = make_unique<TransformTrack>(entity);
	TransformTrack& ref = *track;
	m_Tracks.push_back(move(track));
	return ref;
}

EventTrack& TimeLineAsset::AddEventTrack()
{
	auto track = make_unique<EventTrack>();
	EventTrack& ref = *track;
	m_Tracks.push_back(move(track));
	return ref;
}

void TimeLineAsset::Evaluate(float time, float previousTime, bool timeAdvanced)
{
	for (auto& track : m_Tracks)
	{
		track->Evaluate(time, previousTime, timeAdvanced);
	}
}

void TimeLineAsset::ResetState()
{
	for (auto& track : m_Tracks)
	{
		track->ResetState();
	}
}

void TimeLineAsset::Clear()
{
	m_Tracks.clear();
	m_Duration = 0.0f;
}

TimeLineAsset& TimeLineDirector::CreateAsset()
{
	m_OwnedAsset = make_unique<TimeLineAsset>();
	m_Asset = m_OwnedAsset.get();
	m_CurrentTime = 0.0f;
	m_PreviousTime = 0.0f;
	return *m_Asset;
}

void TimeLineDirector::SetAsset(TimeLineAsset* asset)
{
	m_OwnedAsset.reset();
	m_Asset = asset;
	m_CurrentTime = 0.0f;
	m_PreviousTime = 0.0f;
	ResetAssetState();
}

void TimeLineDirector::Play()
{
	if (!m_Asset)
	{
		return;
	}
	m_IsPlaying = true;
}

void TimeLineDirector::Pause()
{
	m_IsPlaying = false;
}

void TimeLineDirector::Stop()
{
	m_IsPlaying = false;
	m_PreviousTime = m_CurrentTime;
	m_CurrentTime = 0.0f;
	ResetAssetState();
	EvaluateCurrent(false);
}

void TimeLineDirector::Evaluate(float time)
{
	SetTime(time);
	EvaluateCurrent(false);
}

void TimeLineDirector::Update(float deltaTime)
{
	if (!m_IsPlaying || !m_Asset)
	{
		return;
	}

	m_PreviousTime = m_CurrentTime;
	m_CurrentTime += deltaTime * m_Speed;

	const float duration = m_Asset->GetDuration();
	if (duration > 0.0f)
	{
		if (m_CurrentTime > duration)
		{
			if (m_WrapMode == TimeLineWrapMode::Loop)
			{
				m_Asset->Evaluate(duration, m_PreviousTime, true);
				ResetAssetState();
				m_PreviousTime = 0.0f;
				m_CurrentTime = fmodf(m_CurrentTime, duration);
			}
			else
			{
				m_CurrentTime = duration;
				m_IsPlaying = false;
			}
		}
		else if (m_CurrentTime < 0.0f)
		{
			if (m_WrapMode == TimeLineWrapMode::Loop)
			{
				ResetAssetState();
				m_CurrentTime = duration + fmodf(m_CurrentTime, duration);
				m_PreviousTime = 0.0f;
			}
			else
			{
				m_CurrentTime = 0.0f;
				m_IsPlaying = false;
			}
		}
	}

	EvaluateCurrent(true);
}

void TimeLineDirector::SetLoop(bool loop)
{
	m_WrapMode = loop ? TimeLineWrapMode::Loop : TimeLineWrapMode::Once;
}

void TimeLineDirector::SetWrapMode(TimeLineWrapMode wrapMode)
{
	m_WrapMode = wrapMode;
}

void TimeLineDirector::SetTime(float time)
{
	m_PreviousTime = m_CurrentTime;
	const float duration = m_Asset ? m_Asset->GetDuration() : 0.0f;
	m_CurrentTime = ClampTime(time, duration);
	ResetAssetState();
}

void TimeLineDirector::EvaluateCurrent(bool timeAdvanced)
{
	if (!m_Asset)
	{
		return;
	}
	m_Asset->Evaluate(m_CurrentTime, m_PreviousTime, timeAdvanced);
}

void TimeLineDirector::ResetAssetState()
{
	if (m_Asset)
	{
		m_Asset->ResetState();
	}
}
