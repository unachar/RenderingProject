#include "pch.h"
#include "xbot.h"
#include "world.h"
#include "animator.h"
#include "modelmanager.h"

void XBot::Create()
{
	const auto modelName = "XBot";

	const auto modelPosition = XMFLOAT3(-5.0f, 0.0f, 0.0f);
	const auto modelScale = XMFLOAT3(0.01f, 0.01f, 0.01f);

	const auto modelPath = "asset\\model\\XBot.fbx";
	const auto animPath1 = "asset\\model\\standup.fbx";
	const auto animPath2 = "asset\\model\\idle.fbx";
	constexpr bool isConvert = true;

	const auto animations = vector<string>{ "Standup", "Idle" };
	const auto currentAnimation = "Standup";
	const int modelID = ModelManager::LoadAnimModel(modelPath, isConvert);
	if (modelID < 0)
	{
		return;
	}
	ModelManager::LoadAnimation(modelID, animPath1, "Standup");
	ModelManager::LoadAnimation(modelID, animPath2, "Idle");

	for (int i = 0; i < 10; ++i)
	{
		auto entity = World::CreateEntity()
			.Add<TransformComponent>()
			.Add<NameComponent>()
			.Add<MeshComponent>()
			.Add<AABBComponent>()
			.Add<MaterialComponent>()
			.Add<InstancingComponent>()
			.Add<LODComponent>()
			.Add<AnimationModelComponent>();

		entity.SetName(modelName);

		entity.Get<TransformComponent>().Position = { modelPosition.x + (i % 10) * 2.0f,
			modelPosition.y,
			modelPosition.z + (i / 10) * 2.0f
		};
		entity.Get<TransformComponent>().Scale = modelScale;
		entity.Get<MaterialComponent>().UseTexture = true;

		auto& anim = entity.Get<AnimationModelComponent>();
		anim.ModelId = modelID;
		anim.ModelPath = modelPath;
		anim.AnimationPaths = { animPath1, animPath2 };
		anim.Animations = animations;
		anim.CurrentAnimation = currentAnimation;

		Animator::Play(anim, currentAnimation);

		entity.Get<MaterialComponent>().ShaderClassMode = MaterialMode::Manual;
		entity.Get<MaterialComponent>().ShaderClass = ShaderClass::Metallic;

		entity.Get<InstancingComponent>().UseInstancing = true;
		entity.Get<InstancingComponent>().EnableFrustumCulling = true;
		entity.Get<LODComponent>().UseLOD = true;


		if (auto* model = ModelManager::GetAnimModel(modelID))
		{
			entity.Get<AABBComponent>().Center = model->GetAabbCenter();
			entity.Get<AABBComponent>().Extents = model->GetAabbExtents();
		}
	}
}

