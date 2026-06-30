#include "pch.h"
#include "xbot.h"
#include "world.h"
#include "animator.h"
#include "modelmanager.h"

void XBot::Create()
{
	const auto modelName = "XBot";

	const auto modelScale = XMFLOAT3(0.03f, 0.03f, 0.03f);

	const auto modelPath = "asset\\model\\XBot.fbx";
	const auto animPath1 = "asset\\model\\standup.fbx";
	const auto animPath2 = "asset\\model\\idle.fbx";
	constexpr bool isConvert = true;

	const auto animations = vector<string>{ "Standup", "Idle" };
	const auto currentAnimation = "Idle";


	auto& entity = World::CreateEntity()
		.Add<TransformComponent>()
		.Add<NameComponent>()
		.Add<MeshComponent>()
		.Add<AABBComponent>()
		.Add<MaterialComponent>()
		.Add<AnimationModelComponent>();
	
	entity.SetName(modelName);

	entity.Get<TransformComponent>().Scale = modelScale;
	entity.Get<MaterialComponent>().UseTexture = true;

	int modelID = ModelManager::LoadAnimModel(modelPath, isConvert);
	ModelManager::LoadAnimation(modelID, animPath1, "Standup");
	ModelManager::LoadAnimation(modelID, animPath2, "Idle");


	auto& anim = entity.Get<AnimationModelComponent>();
	anim.ModelId = modelID;
	anim.ModelPath = modelPath;
	anim.AnimationPaths = { animPath1, animPath2 };
	anim.Animations = animations;
	anim.CurrentAnimation = currentAnimation;

	Animator::Play(anim, currentAnimation);

	entity.Get<MaterialComponent>().ShaderClassMode = MaterialMode::Manual;
	entity.Get<MaterialComponent>().ShaderClass = ShaderClass::Metallic;


	if (auto* model = ModelManager::GetAnimModel(modelID))
	{
		entity.Get<AABBComponent>().Center = model->GetAabbCenter();
		entity.Get<AABBComponent>().Extents = model->GetAabbExtents();
	}
}

