#include "pch.h"
#include "karen.h"
#include "world.h"
#include "modelmanager.h"

void Karen::Create()
{
	const auto modelName = "Karen";
	const auto modelPosition = XMFLOAT3(1.0f, 0.0f, 0.0f);
	const auto modelScale = XMFLOAT3(0.15f, 0.15f, 0.15f);
	const auto modelPath = "asset\\model\\karenv1.3\\karem.pmx";

	auto& entity = World::CreateEntity()
		.Add<NameComponent>()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<MaterialComponent>()
		.Add<StaticModelComponent>();

	entity.SetName(modelName);
	auto& staticModel = entity.Get<StaticModelComponent>();
	staticModel.ModelPath = modelPath;
	staticModel.ModelId = ModelManager::LoadStaticModel(modelPath);
	entity.Get<TransformComponent>().Position = modelPosition;
	entity.Get<TransformComponent>().Scale = modelScale;

	auto& material = entity.Get<MaterialComponent>();
	material.UseTexture = true;
	material.Roughness = 1.0f;
	material.ShadowThreshold = 1.0f;
	material.ShadowSoftness = 0.5f;
	material.ShadowStrength = 0.066f;
	material.ShaderClassMode = MaterialMode::Manual;
	material.ShaderClass = ShaderClass::PBR;
}
