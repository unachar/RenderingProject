#include "pch.h"
#include "moca.h"
#include "world.h"
#include "modelmanager.h"

void Moca::Create()
{
	const auto modelName = "Moca";
	const auto modelPosition = XMFLOAT3(3.0f, 0.0f, 0.0f);
	const auto modelScale = XMFLOAT3(1.5f, 1.5f, 1.5f);
	const auto modelRotation = XMFLOAT3(XM_PIDIV2, 0.0f, 0.0f);
	const auto modelPath = "asset\\model\\moca.vrm";

	auto entity = World::CreateEntity()
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
