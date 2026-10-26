#pragma once

#include <initializer_list>
#include <string>
#include <vector>

struct AnimationModelComponent;
struct CameraComponent;
struct TransformComponent;

class Animator
{
  public:
    static bool Play(CameraComponent& camera, bool restart = true);
    static bool Play(CameraComponent& camera, const string& vmdPath, bool restart = true);
    static bool Play(CameraComponent& camera, const char* vmdPath, bool restart = true);
    static void Stop(CameraComponent& camera);
    static void Pause(CameraComponent& camera);
    static void SetSpeed(CameraComponent& camera, float speed);
    static void Update(CameraComponent& camera, TransformComponent& transform, float deltaTime);

    static void Play(AnimationModelComponent& animation, bool restart = true);
    static void Play(AnimationModelComponent& animation, const string& animationName, bool restart = true);

    static void Play(AnimationModelComponent& animation, const vector<string>& animationNames, bool restart = true);
    static void Play(AnimationModelComponent& animation, initializer_list<string> animationNames, bool restart = true);
    static void CrossFade(AnimationModelComponent& animation, float blendRate = 0.0f, bool restartNext = true);
    static void CrossFade(AnimationModelComponent& animation, const string& nextAnimation, float blendRate = 0.0f,
                          bool restartNext = true);
    static void Stop(AnimationModelComponent& animation);
    static void Pause(AnimationModelComponent& animation);
    static void SetSpeed(AnimationModelComponent& animation, float speed);
    static void SetBlendRate(AnimationModelComponent& animation, float blendRate);
    static void Update(AnimationModelComponent& animation, float deltaTime);
    static void Update(AnimationModelComponent& animation, const string& currentAnimation, const string& nextAnimation,
                       float deltaTime);
};
