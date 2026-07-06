#include "pch.h"
#include "karen.h"
#include "world.h"
#include "animator.h"
#include "modelmanager.h"

void Karen::Create()
{
	const auto modelName = "Karen";
	const auto modelPosition = XMFLOAT3(1.0f, 0.0f, 0.0f);
	const auto modelScale = XMFLOAT3(0.15f, 0.15f, 0.15f);
	const auto modelPath = "asset\\model\\karenv1.3\\karem.pmx";
	const auto animPath = "asset\\model\\animation\\musicx2.vmd";
	const auto animName = "anim1";

	auto& entity = World::CreateEntity()
		.Add<NameComponent>()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<AABBComponent>()
		.Add<MaterialComponent>()
		.Add<AnimationModelComponent>();

	entity.SetName(modelName);
	entity.Get<TransformComponent>().Position = modelPosition;
	entity.Get<TransformComponent>().Scale = modelScale;

	const int modelID = ModelManager::LoadAnimModel(modelPath);
	const bool animationLoaded = ModelManager::LoadAnimation(modelID, animPath, animName);

	auto& anim = entity.Get<AnimationModelComponent>();
	anim.ModelId = modelID;
	anim.ModelPath = modelPath;
	if (animationLoaded)
	{
		anim.AnimationPaths = { animPath };
		anim.Animations = { animName };
		anim.CurrentAnimation = animName;
		Animator::Play(anim, animName);
	}
	else
	{
		anim.IsPlaying = false;
	}

	auto& material = entity.Get<MaterialComponent>();
	material.UseTexture = true;
	material.Roughness = 1.0f;
	material.ShadowThreshold = 1.0f;
	material.ShadowSoftness = 0.5f;
	material.ShadowStrength = 0.066f;
	material.ShaderClassMode = MaterialMode::Manual;
	material.ShaderClass = ShaderClass::PBR;

	if (auto* model = ModelManager::GetAnimModel(modelID))
	{
		entity.Get<AABBComponent>().Center = model->GetAabbCenter();
		entity.Get<AABBComponent>().Extents = model->GetAabbExtents();
	}
}
