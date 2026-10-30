#pragma once

#include <string>
#include <memory>
#include <DirectXMath.h>

struct VmdAnimation;

struct CameraAnimationPlayback
{
    std::string Path;
    bool PlayOnStart = true;
    bool Loop = true;
    float Speed = 1.0f;
    float Scale = 1.0f;
    DirectX::XMFLOAT3 Offset{};

    // Loaded data is immutable so editor snapshots can safely share it.
    std::shared_ptr<const VmdAnimation> Clip;
    std::string LoadedPath;
    std::string Error;
    float CurrentTime = 0.0f;
    float LastFrame = -1.0f;
    bool IsPlaying = false;
    bool IsActive = false;
    bool CutThisFrame = false;
    bool TimelineDriven = false;
};

struct AnimationPlaybackLayer
{
    string AnimationName{};
    float CurrentTime = 0.0f;
};
