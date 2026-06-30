#include "pch.h"
#include "kacchatta_hone.h"
#include "world.h"
#include "modelmanager.h"

void KacchattaHone::Create()
{
	const auto modelName = "Kacchatta_Hone";
	const auto modelPosition = XMFLOAT3(-3.0f, 0.0f, 0.0f);
	const auto modelScale = XMFLOAT3(0.5f, 0.5f, 0.5f);
	const auto modelPath = "asset\\model\\kacchatta_hone\\kacchatta_hone.pmx";

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
