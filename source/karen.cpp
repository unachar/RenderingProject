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
	const auto animPath = "asset\\model\\animation\\kasou.vmd";
	const auto animPath2 = "asset\\model\\animation\\kasou_kao.vmd";
	const auto animName = "anim1";
	const auto animName2 = "anim2";

	auto entity = World::CreateEntity()
		.Add<NameComponent>()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<AABBComponent>()
		.Add<MaterialComponent>()
		.Add<AnimationModelComponent>()
		.Add<PhysicsComponent>();

	entity.SetName(modelName);
	entity.Get<TransformComponent>().Position = modelPosition;
	entity.Get<TransformComponent>().Scale = modelScale;

	const int modelID = ModelManager::LoadAnimModel(modelPath);
	const bool animationLoaded = ModelManager::LoadAnimation(modelID, animPath, animName);
	const bool animationLoaded2 = ModelManager::LoadAnimation(modelID, animPath2, "anim2");

	auto& anim = entity.Get<AnimationModelComponent>();
	anim.ModelId = modelID;
	anim.ModelPath = modelPath;
	if (animationLoaded && animationLoaded2)
	{
		anim.AnimationPaths = { animPath, animPath2 };
		anim.Animations = { animName, animName2 };
		Animator::Play(anim, { animName, animName2 });
	}

	else
	{
		anim.IsPlaying = false;
	}

	auto& physics = entity.Get<PhysicsComponent>();
	physics.UsePhysics = true;
	physics.UsePhysicsBone = true;
	physics.UsePhysicsEngine = PhysicsEngine::Bullet;

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
