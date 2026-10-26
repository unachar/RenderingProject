#include "pch.h"
#include "animator.h"
#include "componentmanager.h"
#include "animationplayback.h"
#include "vmdanimationimpoter.h"
#include <cmath>

bool Animator::Play(CameraComponent& camera, bool restart)
{
    return Play(camera, camera.Animation.Path, restart);
}

bool Animator::Play(CameraComponent& camera, const char* vmdPath, bool restart)
{
    return Play(camera, string(vmdPath ? vmdPath : ""), restart);
}

bool Animator::Play(CameraComponent& camera, const string& vmdPath, bool restart)
{
    auto& animation = camera.Animation;
    if (vmdPath.empty())
    {
        animation.Error = "カメラ用VMDを指定してください。";
        return false;
    }
    const bool changed = animation.LoadedPath != vmdPath || !animation.Clip;
    if (changed || restart)
    {
        auto clip = make_shared<VmdAnimation>();
        if (!LoadVmdAnimationFile(vmdPath.c_str(), *clip))
        {
            animation.Error = "カメラ用VMDを読み込めません: " + vmdPath;
            Debug::Log("%s\n", animation.Error.c_str());
            return false;
        }
        if (clip->CameraTrack.empty())
        {
            animation.Error = "このVMDにはカメラのキーフレームがありません。";
            Debug::Log("%s: %s\n", animation.Error.c_str(), vmdPath.c_str());
            return false;
        }
        animation.Clip = move(clip);
        animation.LoadedPath = vmdPath;
    }
    animation.Path = vmdPath;
    if (changed || restart)
    {
        animation.CurrentTime = 0.0f;
        animation.LastFrame = -1.0f;
    }
    animation.Error.clear();
    animation.IsPlaying = true;
    animation.IsActive = true;
    return true;
}

void Animator::Stop(CameraComponent& camera)
{
    camera.Animation.IsPlaying = false;
    camera.Animation.IsActive = false;
    camera.Animation.CurrentTime = 0.0f;
    camera.Animation.LastFrame = -1.0f;
    camera.Animation.CutThisFrame = false;
}

void Animator::Pause(CameraComponent& camera)
{
    camera.Animation.IsPlaying = false;
}

void Animator::SetSpeed(CameraComponent& camera, float speed)
{
    camera.Animation.Speed = isfinite(speed) ? max(0.0f, speed) : 1.0f;
}

void Animator::Update(CameraComponent& camera, TransformComponent& transform, float deltaTime)
{
    auto& animation = camera.Animation;
    if (!animation.IsActive || !animation.Clip || animation.Clip->CameraTrack.empty())
    {
        return;
    }
    const auto& keys = animation.Clip->CameraTrack;
    float frameAdvance = 0.0f;
    if (animation.IsPlaying && isfinite(deltaTime) && deltaTime > 0.0f)
    {
        const float elapsed = deltaTime * animation.Speed;
        animation.CurrentTime += elapsed;
        frameAdvance = elapsed * 30.0f;
    }
    float frame = max(0.0f, animation.CurrentTime) * 30.0f;
    const float lastFrame = static_cast<float>(keys.back().Frame);
    if (animation.Loop)
    {
        frame = fmod(frame, lastFrame + 1.0f);
    }
    else if (frame >= lastFrame)
    {
        frame = lastFrame;
        animation.CurrentTime = lastFrame / 30.0f;
        animation.IsPlaying = false;
    }
    // Keep cuts pending until CameraSystem consumes them, including the frame-zero
    // pose applied by ProjectManager before the first simulation update.
    animation.CutThisFrame |= animation.LastFrame < 0.0f || frame < animation.LastFrame ||
                               (animation.Loop && frameAdvance >= lastFrame + 1.0f);
    if (!animation.CutThisFrame)
    {
        auto next = upper_bound(keys.begin(), keys.end(), animation.LastFrame,
                                [](float value, const VmdCameraKeyframe& key) {
            return value < static_cast<float>(key.Frame);
        });
        for (; next != keys.end() && static_cast<float>(next->Frame) <= frame; ++next)
        {
            if (next != keys.begin() && next->Frame - (next - 1)->Frame == 1)
            {
                animation.CutThisFrame = true;
                break;
            }
        }
    }
    animation.LastFrame = frame;
    const VmdCameraSample sample = SampleVmdCameraTrack(keys, frame);
    const bool orthographic = !sample.Perspective;
    animation.CutThisFrame |= camera.IsOrthographic != orthographic;
    camera.IsOrthographic = orthographic;
    // MMD's orthographic zoom depends on distance, independently of the FOV.
    // A distance of 45 MMD units spans 25 units vertically.
    camera.OrthographicHeight = max(0.001f, fabsf(sample.Distance * animation.Scale) * (25.0f / 45.0f));
    const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(-sample.Rotation.x, -sample.Rotation.y,
                                                          -sample.Rotation.z);
    const XMVECTOR center = XMVectorAdd(XMVectorScale(XMLoadFloat3(&sample.Center), animation.Scale),
                                        XMLoadFloat3(&animation.Offset));
    const XMVECTOR eye = XMVectorAdd(center, XMVector3TransformNormal(
        XMVectorSet(0, 0, sample.Distance * animation.Scale, 0), rotation));
    const float forwardSign = orthographic && sample.Distance > 0.0f ? -1.0f : 1.0f;
    const XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, forwardSign, 0), rotation);
    XMStoreFloat3(&transform.Position, eye);
    // A direction-based target also works for zero or positive VMD distances.
    XMStoreFloat3(&camera.Target, XMVectorAdd(eye, forward));
    XMStoreFloat3(&camera.Up, XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rotation));
    transform.Rotation = {-sample.Rotation.x, -sample.Rotation.y, -sample.Rotation.z};
    transform.IsDirty = true;
    camera.Position = transform.Position;
    camera.Fov = sample.Fov;
}

void Animator::Play(AnimationModelComponent& animation, bool restart)
{
    Play(animation, animation.CurrentAnimation, restart);
}

void Animator::Play(AnimationModelComponent& animation, const string& animationName, bool restart)
{
    if (animationName.empty())
    {
        return;
    }

    if (restart || animation.CurrentAnimation != animationName)
    {
        animation.CurrentTime = 0.0f;
    }

    animation.CurrentAnimation = animationName;
    animation.ActiveAnimationLayers.clear();
    animation.NextAnimation.clear();
    animation.NextTime = 0.0f;
    animation.BlendRate = 0.0f;
    animation.IsPlaying = true;
}

void Animator::Play(AnimationModelComponent& animation, const vector<string>& animationNames, bool restart)
{
    vector<string> uniqueNames;
    uniqueNames.reserve(animationNames.size());
    for (const string& animationName : animationNames)
    {
        if (animationName.empty())
        {
            continue;
        }

        auto duplicate = find(uniqueNames.begin(), uniqueNames.end(), animationName);
        if (duplicate != uniqueNames.end())
        {
            uniqueNames.erase(duplicate);
        }
        uniqueNames.push_back(animationName);
    }

    if (uniqueNames.empty())
    {
        return;
    }
    if (uniqueNames.size() == 1)
    {
        Play(animation, uniqueNames.front(), restart);
        return;
    }

    const bool sameLayers = animation.ActiveAnimationLayers.size() == uniqueNames.size() &&
                            equal(uniqueNames.begin(), uniqueNames.end(), animation.ActiveAnimationLayers.begin(),
                                  [](const string& name, const AnimationPlaybackLayer& layer) {
                                      return name == layer.AnimationName;
                                  });

    if (!sameLayers)
    {
        vector<AnimationPlaybackLayer> newLayers;
        newLayers.reserve(uniqueNames.size());
        for (const string& animationName : uniqueNames)
        {
            float preservedTime = 0.0f;
            if (!restart)
            {
                auto oldLayer = find_if(animation.ActiveAnimationLayers.begin(), animation.ActiveAnimationLayers.end(),
                                        [&animationName](const AnimationPlaybackLayer& layer) {
                                            return layer.AnimationName == animationName;
                                        });
                if (oldLayer != animation.ActiveAnimationLayers.end())
                {
                    preservedTime = oldLayer->CurrentTime;
                }
                else if (animation.CurrentAnimation == animationName)
                {
                    preservedTime = animation.CurrentTime;
                }
            }
            newLayers.push_back({animationName, preservedTime});
        }
        animation.ActiveAnimationLayers = move(newLayers);
    }
    else if (restart)
    {
        for (AnimationPlaybackLayer& layer : animation.ActiveAnimationLayers)
        {
            layer.CurrentTime = 0.0f;
        }
    }

    animation.CurrentAnimation = animation.ActiveAnimationLayers.front().AnimationName;
    animation.CurrentTime = animation.ActiveAnimationLayers.front().CurrentTime;
    animation.NextAnimation.clear();
    animation.NextTime = 0.0f;
    animation.BlendRate = 0.0f;
    animation.IsPlaying = true;
}

void Animator::Play(AnimationModelComponent& animation, initializer_list<string> animationNames, bool restart)
{
    Play(animation, vector<string>(animationNames), restart);
}

void Animator::CrossFade(AnimationModelComponent& animation, float blendRate, bool restartNext)
{
    CrossFade(animation, animation.NextAnimation, blendRate, restartNext);
}

void Animator::CrossFade(AnimationModelComponent& animation, const string& nextAnimation, float blendRate,
                         bool restartNext)
{
    animation.ActiveAnimationLayers.clear();
    if (nextAnimation.empty())
    {
        animation.NextAnimation.clear();
        animation.NextTime = 0.0f;
        animation.BlendRate = 0.0f;
        return;
    }

    if (restartNext || animation.NextAnimation != nextAnimation)
    {
        animation.NextTime = 0.0f;
    }

    animation.NextAnimation = nextAnimation;
    SetBlendRate(animation, blendRate);
    animation.IsPlaying = true;
}

void Animator::Stop(AnimationModelComponent& animation)
{
    animation.IsPlaying = false;
    animation.CurrentTime = 0.0f;
    animation.NextTime = 0.0f;
    for (AnimationPlaybackLayer& layer : animation.ActiveAnimationLayers)
    {
        layer.CurrentTime = 0.0f;
    }
}

void Animator::Pause(AnimationModelComponent& animation)
{
    animation.IsPlaying = false;
}

void Animator::SetSpeed(AnimationModelComponent& animation, float speed)
{
    animation.Speed = speed;
}

void Animator::SetBlendRate(AnimationModelComponent& animation, float blendRate)
{
    animation.BlendRate = clamp(blendRate, 0.0f, 1.0f);
}

void Animator::Update(AnimationModelComponent& animation, float deltaTime)
{
    if (!animation.IsPlaying)
    {
        return;
    }

    const float scaledDelta = deltaTime * animation.Speed;
    if (animation.ActiveAnimationLayers.size() > 1)
    {
        for (AnimationPlaybackLayer& layer : animation.ActiveAnimationLayers)
        {
            layer.CurrentTime += scaledDelta;
        }
        animation.CurrentAnimation = animation.ActiveAnimationLayers.front().AnimationName;
        animation.CurrentTime = animation.ActiveAnimationLayers.front().CurrentTime;
        return;
    }

    if (animation.CurrentAnimation.empty())
    {
        return;
    }
    animation.CurrentTime += scaledDelta;
    if (!animation.NextAnimation.empty())
    {
        animation.NextTime += scaledDelta;
    }
}

void Animator::Update(AnimationModelComponent& animation, const string& currentAnimation, const string& nextAnimation,
                      float deltaTime)
{
    if (!currentAnimation.empty() && animation.CurrentAnimation != currentAnimation)
    {
        Play(animation, currentAnimation);
    }

    if (!nextAnimation.empty() && animation.NextAnimation != nextAnimation)
    {
        CrossFade(animation, nextAnimation, animation.BlendRate);
    }
    else if (nextAnimation.empty())
    {
        CrossFade(animation, "", 0.0f);
    }

    Update(animation, deltaTime);
}
