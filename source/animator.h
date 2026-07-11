#pragma once

#include <initializer_list>
#include <string>
#include <vector>

struct AnimationModelComponent;

class Animator
{
public:
	static void Play(AnimationModelComponent& animation, bool restart = true);
	static void Play(AnimationModelComponent& animation, const std::string& animationName, bool restart = true);

	static void Play(AnimationModelComponent& animation, const std::vector<std::string>& animationNames, bool restart = true);
	static void Play(AnimationModelComponent& animation, std::initializer_list<std::string> animationNames, bool restart = true);
	static void CrossFade(AnimationModelComponent& animation, float blendRate = 0.0f, bool restartNext = true);
	static void CrossFade(AnimationModelComponent& animation, const std::string& nextAnimation, float blendRate = 0.0f, bool restartNext = true);
	static void Stop(AnimationModelComponent& animation);
	static void Pause(AnimationModelComponent& animation);
	static void Resume(AnimationModelComponent& animation);
	static void SetSpeed(AnimationModelComponent& animation, float speed);
	static void SetBlendRate(AnimationModelComponent& animation, float blendRate);
	static void Update(AnimationModelComponent& animation, float deltaTime);
	static void Update(AnimationModelComponent& animation, const std::string& currentAnimation, const std::string& nextAnimation, float deltaTime);
};
