#include "pch.h"
#include "animator.h"
#include "componentmanager.h"

void Animator::Play(AnimationModelComponent& animation, bool restart)
{
	Play(animation, animation.CurrentAnimation, restart);
}

void Animator::Play(AnimationModelComponent& animation, const std::string& animationName, bool restart)
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

		// Retain the final occurrence so a repeated name still obeys the public
		// "later layer wins" rule (A, B, A must resolve conflicts in favor of A).
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
			[](const string& name, const AnimationPlaybackLayer& layer)
			{
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
					[&animationName](const AnimationPlaybackLayer& layer)
					{
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
			newLayers.push_back({ animationName, preservedTime });
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

void Animator::CrossFade(AnimationModelComponent& animation, const string& nextAnimation, float blendRate, bool restartNext)
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

void Animator::Resume(AnimationModelComponent& animation)
{
	animation.IsPlaying = true;
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

void Animator::Update(AnimationModelComponent& animation, const string& currentAnimation, const string& nextAnimation, float deltaTime)
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
