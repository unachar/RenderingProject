#include "pch.h"
#include "alicia.h"
#include "world.h"
#include "modelmanager.h"

void Alicia::Create()
{

	const auto modelName = "Alicia";

	const auto modelPosition = XMFLOAT3(5.0f, 0.0f, 0.0f);
	const auto modelScale = XMFLOAT3(0.015f, 0.015f, 0.015f);
	const auto modelRotation = XMFLOAT3(XM_PIDIV2, 0.0f, 0.0f);


	const auto modelPath = "asset\\model\\Alicia\\Alicia_solid_Unity.FBX";


	auto entity = World::CreateEntity()
		.Add<NameComponent>()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<MaterialComponent>()
		.Add<StaticModelComponent>();


	entity.SetName(modelName);

	int modelID = ModelManager::LoadStaticModel(modelPath);
	auto& staticModel = entity.Get<StaticModelComponent>();
	staticModel.ModelPath = modelPath;
	staticModel.ModelId = modelID;

	entity.Get<TransformComponent>().Position = modelPosition;
	entity.Get<TransformComponent>().Scale = modelScale;
	entity.Get<TransformComponent>().Rotation = modelRotation;


	auto& material = entity.Get<MaterialComponent>();
	material.UseTexture = true;
	material.Roughness = 1.0f;
	material.ShadowThreshold = 1.0f;
	material.ShadowSoftness = 0.5f;
	material.ShadowStrength = 0.066f;
	material.ShaderClassMode = MaterialMode::Manual;
	material.ShaderClass = ShaderClass::Lit;
}
