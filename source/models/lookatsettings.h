#pragma once
#include "types.h"
#include <array>
#include <cstdint>
#include <string>

struct LookAtJointSettings
{
    float Weight = 1.0f;
    float MaxYawDegrees = 30.0f;
    float MaxPitchDegrees = 20.0f;
    float ResponseSpeed = 12.0f; // Exponential response in inverse seconds.
    float MaxSpeedDegrees = 360.0f;
};

struct LookAtSettings
{
    float Weight = 1.0f;
    LookAtJointSettings Neck{0.35f, 25.0f, 15.0f, 5.0f, 90.0f};
    LookAtJointSettings Head{0.8f, 45.0f, 30.0f, 7.0f, 140.0f};
    LookAtJointSettings Eyes{1.0f, 30.0f, 20.0f, 18.0f, 360.0f};
    float EyeComfortDegrees = 10.0f;
    float RearFadeStartDegrees = 100.0f;
    float RearFadeEndDegrees = 140.0f;
    // Anatomical facing/up in the MODEL'S BIND space, not the eye bone's local axes.
    bool AutoDetectForward = true; // Infer facing from bind-pose foot-to-toe directions when available.
    aiVector3D ModelForward{0, 0, 1};
    aiVector3D ModelUp{0, 1, 0};
    aiVector3D TargetOffset{}; // World-space offset from the target Transform position.
    std::string NeckBone, HeadBone, LeftEyeBone, RightEyeBone; // Optional exact-name overrides.
};

namespace Gaze
{
struct Angles { float Yaw = 0, Pitch = 0; };
struct State { std::array<Gaze::Angles, 4> Angles{}; }; // Neck, head, left eye, right eye.
}

struct LookAtRuntime
{
    bool Active = false; // Remains true until a clear has eased back to animation.
    bool Tracking = false;
    uint32_t Target = UINT32_MAX;
    uint32_t Owner = UINT32_MAX;
    int PoseModelId = -1;
    LookAtSettings Settings{};
    Gaze::State State{};
};

// Called synchronously from Registry::DestroyEntity, before IDs enter the free list.
inline void InvalidateLookAtTarget(LookAtRuntime& runtime, uint32_t destroyed)
{
    if (runtime.Tracking && runtime.Target == destroyed)
    {
        runtime.Tracking = false;
        runtime.Target = UINT32_MAX;
    }
}
