#include "pch.h"
#include "sky.h"
#include "world.h"
#include "texturemanager.h"
#include "modelmanager.h"

void Sky::Create()
{
	auto name = "Sky";
	auto scale = XMFLOAT3(50.0f, 50.0f, 50.0f);

	auto modelPath = "asset\\model\\sky\\sky.obj";
	auto texturePath = "asset\\model\\sky\\charolettenbrunn_park_2k.DDS";


	auto entity = World::CreateEntity()
		.Add<StaticModelComponent>()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<MaterialComponent>()
		.Add<NameComponent>();

	
	entity.SetName(name);

	entity.Get<TransformComponent>().Scale = scale;

	int modelID = ModelManager::LoadStaticModel(modelPath);
	int textureID = TextureManager::LoadTexture(texturePath);
	auto& staticModel = entity.Get<StaticModelComponent>();
	staticModel.ModelPath = modelPath;
	staticModel.ModelId = modelID;


	auto& material = entity.Get<MaterialComponent>();
	material.TextureID = textureID;
	material.TexturePath = texturePath;
	material.ShaderClassMode = MaterialMode::Manual;
	material.ShaderClass = ShaderClass::Unlit;
	material.UseTexture = true;
}
