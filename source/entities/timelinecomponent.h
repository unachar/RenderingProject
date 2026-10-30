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
    LightIntensity,
    LightColor,
    LightDirection,
    LightRange,
    LightBeamDensity,
    BloomThreshold,
    BloomSoftKnee,
    BloomRadius,
    FogDensity,
    FogColor,
    AnimationTime,
    EmissiveIntensity,
    EmissiveColor,
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
    DirectX::XMFLOAT4 Value = {0, 0, 0, 0};
    TimelineInterpolation Interpolation = TimelineInterpolation::Linear;
};

struct TimelineClipData
{
    string Name = "Clip";
    float StartTime = 0.0f;
    float Duration = 1.0f;
    float BlendIn = 0.0f;
    float BlendOut = 0.0f;
    bool Enabled = true;
    vector<TimelineKeyframe> Keys{};
};

struct TimelineTrackData
{
    string Name = "Track";
    EntityID Target = g_kINVALID_ENTITY;
    TimelineProperty Property = TimelineProperty::TransformPosition;
    bool Enabled = true;
    bool HasDefaultValue = false;
    int FogVolumeIndex = 0;
    DirectX::XMFLOAT4 DefaultValue = {0, 0, 0, 0};
    vector<TimelineClipData> Clips{};
};

struct StageLightFixture
{
    EntityID Target = g_kINVALID_ENTITY;
    float Phase = 0.0f;
};

struct StageLightGroup
{
    bool Enabled = true;
    float Intensity = 10.0f;
    DirectX::XMFLOAT4 Color = {0.2f, 0.5f, 1.0f, 1.0f};
    DirectX::XMFLOAT4 AlternateColor = {1.0f, 0.1f, 0.4f, 1.0f};
    bool AlternateOnBeat = false;
    float Pan = 0.0f;
    float Tilt = 0.5f;
    float PanSweep = 0.6f;
    float TiltSweep = 0.2f;
    float SweepBeats = 4.0f;
    float StrobesPerBeat = 0.0f;
    float StrobeDuty = 0.25f;
    float BeamDensity = 0.2f;
    vector<StageLightFixture> Fixtures;
};

struct LiveMonitorBinding
{
    EntityID Target = g_kINVALID_ENTITY;
    EntityID SurroundLight = g_kINVALID_ENTITY;
    vector<string> Frames;
    float FramesPerSecond = 30.0f;
    float StartTime = 0.0f;
    float EmissiveIntensity = 3.0f;
    float LightIntensity = 5.0f;
    bool Loop = true;
};

struct TimelineComponent
{
    // LiveDirector uses the audio device position, rather than accumulating frame time.
    string MusicPath;
    float MusicOffset = 0.0f;
    float BeatsPerMinute = 120.0f;
    float BeatOffset = 0.0f;
    bool SeekRequested = false;
    string PlaybackError;
    float MusicDuration = 0.0f; // Runtime metadata; not serialized.
    float Duration = 5.0f;
    float CurrentTime = 0.0f;
    float Speed = 1.0f;
    bool PlayOnAwake = true;
    bool Loop = false;
    bool IsPlaying = false;
    uint64_t LastPlaySession = 0;
    vector<TimelineTrackData> Tracks{};
    vector<StageLightGroup> LightGroups;
    vector<LiveMonitorBinding> Monitors;
};
