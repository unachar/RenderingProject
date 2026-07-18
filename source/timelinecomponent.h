#pragma once

#include "ecs.h"
#include <DirectXMath.h>
#include <string>
#include <vector>

enum class TimelineProperty : int
{
	TransformPosition = 0,
	TransformRotation,
	TransformScale,
	CameraTarget,
	CameraFov,
	CameraNearClip,
	CameraFarClip,
	CameraLockOnOffset,
	PostProcessIntensity,
};

enum class TimelineInterpolation : int
{
	Step = 0,
	Linear,
	EaseIn,
	EaseOut,
	EaseInOut,
};

struct TimelineKeyframe
{
	float Time = 0.0f;
	DirectX::XMFLOAT4 Value = { 0, 0, 0, 0 };
	TimelineInterpolation Interpolation = TimelineInterpolation::Linear;
};

struct TimelineClipData
{
	std::string Name = "Clip";
	float StartTime = 0.0f;
	float Duration = 1.0f;
	float BlendIn = 0.0f;
	float BlendOut = 0.0f;
	bool Enabled = true;
	std::vector<TimelineKeyframe> Keys{};
};

struct TimelineTrackData
{
	std::string Name = "Track";
	EntityID Target = g_kINVALID_ENTITY;
	TimelineProperty Property = TimelineProperty::TransformPosition;
	bool Enabled = true;
	bool HasDefaultValue = false;
	DirectX::XMFLOAT4 DefaultValue = { 0, 0, 0, 0 };
	std::vector<TimelineClipData> Clips{};
};

struct TimelineComponent
{
	float Duration = 5.0f;
	float CurrentTime = 0.0f;
	float Speed = 1.0f;
	bool PlayOnAwake = true;
	bool Loop = false;
	bool IsPlaying = false;
	uint64_t LastPlaySession = 0;
	std::vector<TimelineTrackData> Tracks{};
};
