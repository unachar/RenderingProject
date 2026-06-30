#pragma once

#include "ecs.h"
#include "componentmanager.h"
#include <optional>
#include <type_traits>
#include <cmath>

enum class TimeLineEase
{
	Step,
	Linear,
	EaseIn,
	EaseOut,
	EaseInOut,
};

enum class TimeLineWrapMode
{
	Once,
	Loop,
};

struct TimeLineVectorKeyFrame
{
	float Time = 0.0f;
	XMFLOAT3 Value = { 0.0f, 0.0f, 0.0f };
	TimeLineEase Ease = TimeLineEase::Linear;
};

class TimeLineVectorCurve
{
private:
	vector<TimeLineVectorKeyFrame> m_Keys;

public:
	void AddKey(float time, const XMFLOAT3& value, TimeLineEase ease = TimeLineEase::Linear);
	bool HasKeys() const { return !m_Keys.empty(); }
	XMFLOAT3 Evaluate(float time) const;
	void Clear() { m_Keys.clear(); }
};

inline float TimeLineClamp01(float value)
{
	if (value < 0.0f) return 0.0f;
	if (value > 1.0f) return 1.0f;
	return value;
}

inline float TimeLineApplyEase(float t, TimeLineEase ease)
{
	t = TimeLineClamp01(t);
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

template<typename TValue>
struct TimeLineKeyFrame
{
	float Time = 0.0f;
	TValue Value {};
	TimeLineEase Ease = TimeLineEase::Linear;
};

template<typename TValue>
struct TimeLineValueTraits;

template<>
struct TimeLineValueTraits<float>
{
	static float Lerp(float a, float b, float t)
	{
		return a + (b - a) * t;
	}
};

template<>
struct TimeLineValueTraits<XMFLOAT2>
{
	static XMFLOAT2 Lerp(const XMFLOAT2& a, const XMFLOAT2& b, float t)
	{
		return
		{
			a.x + (b.x - a.x) * t,
			a.y + (b.y - a.y) * t
		};
	}
};

template<>
struct TimeLineValueTraits<XMFLOAT3>
{
	static XMFLOAT3 Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t)
	{
		return
		{
			a.x + (b.x - a.x) * t,
			a.y + (b.y - a.y) * t,
			a.z + (b.z - a.z) * t
		};
	}
};

template<>
struct TimeLineValueTraits<XMFLOAT4>
{
	static XMFLOAT4 Lerp(const XMFLOAT4& a, const XMFLOAT4& b, float t)
	{
		return
		{
			a.x + (b.x - a.x) * t,
			a.y + (b.y - a.y) * t,
			a.z + (b.z - a.z) * t,
			a.w + (b.w - a.w) * t
		};
	}
};

template<typename TValue>
class TimeLineCurve
{
private:
	vector<TimeLineKeyFrame<TValue>> m_Keys;

public:
	void AddKey(float time, const TValue& value, TimeLineEase ease = TimeLineEase::Linear)
	{
		TimeLineKeyFrame<TValue> key;
		key.Time = time;
		key.Value = value;
		key.Ease = ease;
		m_Keys.push_back(key);
		sort(m_Keys.begin(), m_Keys.end(), [](const TimeLineKeyFrame<TValue>& a, const TimeLineKeyFrame<TValue>& b)
			{
				return a.Time < b.Time;
			});
	}

	bool HasKeys() const { return !m_Keys.empty(); }

	TValue Evaluate(float time) const
	{
		if (m_Keys.empty())
		{
			return TValue {};
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
				return TimeLineValueTraits<TValue>::Lerp(from.Value, to.Value, TimeLineApplyEase(t, from.Ease));
			}
		}

		return m_Keys.back().Value;
	}
};

struct TimeLineTransformSample
{
	bool HasPosition = false;
	bool HasRotation = false;
	bool HasScale = false;
	XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 Rotation = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 Scale = { 1.0f, 1.0f, 1.0f };
	float Weight = 1.0f;
};

class TimeLineClip
{
protected:
	float m_StartTime = 0.0f;
	float m_Duration = 0.0f;
	float m_BlendInDuration = 0.0f;
	float m_BlendOutDuration = 0.0f;

public:
	TimeLineClip(float startTime, float duration);
	virtual ~TimeLineClip() = default;

	float GetStartTime() const { return m_StartTime; }
	float GetDuration() const { return m_Duration; }
	float GetEndTime() const { return m_StartTime + m_Duration; }
	float GetLocalTime(float timelineTime) const;
	float GetWeight(float timelineTime) const;
	bool Contains(float timelineTime) const;

	TimeLineClip& SetBlendIn(float duration);
	TimeLineClip& SetBlendOut(float duration);
};

class TransformClip : public TimeLineClip
{
private:
	TimeLineVectorCurve m_PositionCurve;
	TimeLineVectorCurve m_RotationCurve;
	TimeLineVectorCurve m_ScaleCurve;

public:
	TransformClip(float startTime, float duration);

	TransformClip& AddPositionKey(float time, const XMFLOAT3& value, TimeLineEase ease = TimeLineEase::Linear);
	TransformClip& AddRotationKey(float time, const XMFLOAT3& value, TimeLineEase ease = TimeLineEase::Linear);
	TransformClip& AddScaleKey(float time, const XMFLOAT3& value, TimeLineEase ease = TimeLineEase::Linear);
	TransformClip& SetBlendIn(float duration);
	TransformClip& SetBlendOut(float duration);

	TimeLineTransformSample Evaluate(float timelineTime) const;
};

struct TimeLineEvent
{
	float Time = 0.0f;
	function<void()> Callback;
	bool Fired = false;
};

class TimeLineTrack
{
public:
	virtual ~TimeLineTrack() = default;
	virtual void Evaluate(float time, float previousTime, bool timeAdvanced) = 0;
	virtual void ResetState() {}
};

class TransformTrack : public TimeLineTrack
{
private:
	EntityID m_Target = g_kINVALID_ENTITY;
	vector<TransformClip> m_Clips;

	static XMFLOAT3 Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t);
	static void Accumulate(XMFLOAT3& total, float& totalWeight, const XMFLOAT3& value, float weight);

public:
	explicit TransformTrack(EntityID target);

	EntityID GetTarget() const { return m_Target; }
	TransformClip& AddClip(float startTime, float duration);
	void Evaluate(float time, float previousTime, bool timeAdvanced) override;
};

class EventTrack : public TimeLineTrack
{
private:
	vector<TimeLineEvent> m_Events;

public:
	EventTrack& AddEvent(float time, function<void()> callback);
	void Evaluate(float time, float previousTime, bool timeAdvanced) override;
	void ResetState() override;
};

template<typename TComponent>
class ComponentPropertyClipBase : public TimeLineClip
{
public:
	ComponentPropertyClipBase(float startTime, float duration)
		: TimeLineClip(startTime, duration)
	{
	}

	virtual void Evaluate(TComponent& component, float timelineTime) const = 0;
};

template<typename TComponent, typename TValue>
class ComponentPropertyClip : public ComponentPropertyClipBase<TComponent>
{
private:
	using MemberPointer = TValue TComponent::*;

	MemberPointer m_Member = nullptr;
	TimeLineCurve<TValue> m_Curve;

public:
	ComponentPropertyClip(float startTime, float duration, MemberPointer member)
		: ComponentPropertyClipBase<TComponent>(startTime, duration), m_Member(member)
	{
	}

	ComponentPropertyClip& AddKey(float time, const TValue& value, TimeLineEase ease = TimeLineEase::Linear)
	{
		m_Curve.AddKey(time, value, ease);
		return *this;
	}

	ComponentPropertyClip& SetBlendIn(float duration)
	{
		TimeLineClip::SetBlendIn(duration);
		return *this;
	}

	ComponentPropertyClip& SetBlendOut(float duration)
	{
		TimeLineClip::SetBlendOut(duration);
		return *this;
	}

	void Evaluate(TComponent& component, float timelineTime) const override
	{
		if (!m_Member || !m_Curve.HasKeys() || !this->Contains(timelineTime))
		{
			return;
		}

		const float weight = this->GetWeight(timelineTime);
		if (weight <= 0.0f)
		{
			return;
		}

		const TValue targetValue = m_Curve.Evaluate(this->GetLocalTime(timelineTime));
		TValue& property = component.*m_Member;
		property = weight < 1.0f
			? TimeLineValueTraits<TValue>::Lerp(property, targetValue, weight)
			: targetValue;

		if constexpr (is_same_v<TComponent, TransformComponent>)
		{
			component.IsDirty = true;
		}
	}
};

template<typename TComponent>
class ComponentPropertyTrack : public TimeLineTrack
{
private:
	EntityID m_Target = g_kINVALID_ENTITY;
	vector<unique_ptr<ComponentPropertyClipBase<TComponent>>> m_Clips;

public:
	explicit ComponentPropertyTrack(EntityID target)
		: m_Target(target)
	{
	}

	EntityID GetTarget() const { return m_Target; }

	ComponentPropertyClip<TComponent, float>& AddFloatClip(float startTime, float duration, float TComponent::* member)
	{
		auto clip = make_unique<ComponentPropertyClip<TComponent, float>>(startTime, duration, member);
		auto& ref = *clip;
		m_Clips.push_back(move(clip));
		return ref;
	}

	ComponentPropertyClip<TComponent, XMFLOAT2>& AddVector2Clip(float startTime, float duration, XMFLOAT2 TComponent::* member)
	{
		auto clip = make_unique<ComponentPropertyClip<TComponent, XMFLOAT2>>(startTime, duration, member);
		auto& ref = *clip;
		m_Clips.push_back(move(clip));
		return ref;
	}

	ComponentPropertyClip<TComponent, XMFLOAT3>& AddVector3Clip(float startTime, float duration, XMFLOAT3 TComponent::* member)
	{
		auto clip = make_unique<ComponentPropertyClip<TComponent, XMFLOAT3>>(startTime, duration, member);
		auto& ref = *clip;
		m_Clips.push_back(move(clip));
		return ref;
	}

	ComponentPropertyClip<TComponent, XMFLOAT4>& AddVector4Clip(float startTime, float duration, XMFLOAT4 TComponent::* member)
	{
		auto clip = make_unique<ComponentPropertyClip<TComponent, XMFLOAT4>>(startTime, duration, member);
		auto& ref = *clip;
		m_Clips.push_back(move(clip));
		return ref;
	}

	void Evaluate(float time, float previousTime, bool timeAdvanced) override
	{
		(void)previousTime;
		(void)timeAdvanced;

		if (m_Target == g_kINVALID_ENTITY ||
			!Registry::IsAlive(m_Target) ||
			!ComponentManager::HasComponent<TComponent>(m_Target))
		{
			return;
		}

		auto& component = ComponentManager::GetComponentUnchecked<TComponent>(m_Target);
		for (const auto& clip : m_Clips)
		{
			if (clip)
			{
				clip->Evaluate(component, time);
			}
		}
	}
};

class TimeLineAsset
{
private:
	float m_Duration = 0.0f;
	vector<unique_ptr<TimeLineTrack>> m_Tracks;

public:
	void SetDuration(float duration);
	float GetDuration() const { return m_Duration; }

	TransformTrack& AddTransformTrack(EntityID entity);
	EventTrack& AddEventTrack();

	template<typename TComponent>
	ComponentPropertyTrack<TComponent>& AddComponentTrack(EntityID entity)
	{
		auto track = make_unique<ComponentPropertyTrack<TComponent>>(entity);
		auto& ref = *track;
		m_Tracks.push_back(move(track));
		return ref;
	}

	void Evaluate(float time, float previousTime, bool timeAdvanced);
	void ResetState();
	void Clear();
};

class TimeLineDirector
{
private:
	unique_ptr<TimeLineAsset> m_OwnedAsset;
	TimeLineAsset* m_Asset = nullptr;
	float m_CurrentTime = 0.0f;
	float m_PreviousTime = 0.0f;
	float m_Speed = 1.0f;
	bool m_IsPlaying = false;
	TimeLineWrapMode m_WrapMode = TimeLineWrapMode::Once;

	void EvaluateCurrent(bool timeAdvanced);
	void ResetAssetState();

public:
	TimeLineAsset& CreateAsset();
	void SetAsset(TimeLineAsset* asset);
	TimeLineAsset* GetAsset() { return m_Asset; }
	const TimeLineAsset* GetAsset() const { return m_Asset; }

	void Play();
	void Pause();
	void Stop();
	void Evaluate(float time);
	void Update(float deltaTime);

	void SetLoop(bool loop);
	void SetWrapMode(TimeLineWrapMode wrapMode);
	void SetSpeed(float speed) { m_Speed = speed; }
	void SetTime(float time);

	bool IsPlaying() const { return m_IsPlaying; }
	float GetTime() const { return m_CurrentTime; }
	float GetSpeed() const { return m_Speed; }
	TimeLineWrapMode GetWrapMode() const { return m_WrapMode; }
};
