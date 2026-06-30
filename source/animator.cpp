#include "pch.h"
#include "animator.h"
#include "componentmanager.h"

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
	animation.NextAnimation.clear();
	animation.NextTime = 0.0f;
	animation.BlendRate = 0.0f;
	animation.IsPlaying = true;
}

void Animator::CrossFade(AnimationModelComponent& animation, float blendRate, bool restartNext)
{
	CrossFade(animation, animation.NextAnimation, blendRate, restartNext);
}

void Animator::CrossFade(AnimationModelComponent& animation, const string& nextAnimation, float blendRate, bool restartNext)
{
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
	if (!animation.IsPlaying || animation.CurrentAnimation.empty())
	{
		return;
	}

	const float scaledDelta = deltaTime * animation.Speed;
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
