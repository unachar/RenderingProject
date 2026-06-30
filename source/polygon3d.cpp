#include "pch.h"
#include "polygon3d.h"
#include "texturemanager.h"
#include "world.h"
#include "rendererdraw.h"
#include "vector.h"

void Polygon3D::Create()
{
	const auto name = "polygon3d";
	const auto vsPath = "shader/hlsl/build/colorshader3dVS.cso";
	const auto psPath = "shader/hlsl/build/colorshader3dPS.cso";
	const auto texture = "asset/texture/neko.png";
	const auto normalMap = "asset/texture/neko_normal.png";


	auto entity = World::CreateEntity()
		.Add<SpriteComponent>()
		.Add<TransformComponent>()
		.Add<AABBComponent>()
		.Add<ShaderComponent>()
		.Add<MaterialComponent>()
		.Add<NameComponent>();


	entity.SetName(name);

	entity.Get<ShaderComponent>().VsPath = vsPath;
	entity.Get<ShaderComponent>().PsPath = psPath;
	entity.Get<SpriteComponent>().Is3D = true;


	int textureID = TextureManager::LoadTexture(texture);
	entity.Get<MaterialComponent>().TextureID = textureID;
	entity.Get<MaterialComponent>().TexturePath = texture;
	entity.Get<MaterialComponent>().UseTexture = true;

	entity.Get<TransformComponent>().Position = Vector3(0.0f, 5.0f, 5.0f);
	entity.Get<AABBComponent>().Center = { 0.0f, 0.0f, 0.0f };
	entity.Get<AABBComponent>().Extents = { 0.5f, 0.5f, 0.05f };


	VertexResource vertexResource{};
	vertexResource.entityid = entity.GetID();
	vertexResource.shapetype = ShapeType::QUAD;
	vertexResource.radius = 0.5f;
	vertexResource.color = Color::WHITE;

	auto& material = entity.Get<MaterialComponent>();
	material.ShaderClassMode = MaterialMode::Manual;
	material.ShaderClass = ShaderClass::PBR;

	RendererResource::CreateSpriteVertex(vertexResource);
}

