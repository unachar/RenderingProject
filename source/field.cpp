#include "pch.h"
#include "field.h"
#include "world.h"
#include "rendererdraw.h"
#include "texturemanager.h"
#include "systemmanager.h"
#include "materialsystem.h"


void Field::Create()
{
	const auto name = "field";
	const auto texture = "asset/texture/field.png";
	const auto normalMap = "asset/texture/field_normal.png";


	auto entity = World::CreateEntity()
		.Add<TransformComponent>()
		.Add<SpriteComponent>()
		.Add<AABBComponent>()
		.Add<MaterialComponent>()
		.Add<NameComponent>();


	entity.SetName(name);

	entity.Get<TransformComponent>().Rotation = { XM_PIDIV2, 0.0f, 0.0f };
	entity.Get<AABBComponent>().Center = { 0.0f, 0.0f, 0.0f };
	entity.Get<AABBComponent>().Extents = { 10.0f, 10.0f, 0.05f };

	entity.Get<SpriteComponent>().Is3D = true;


	int textureID = TextureManager::LoadTexture(texture);
	int normalMapID = TextureManager::LoadNormalTexture(normalMap);

	auto& material = entity.Get<MaterialComponent>();

	material.TextureID = textureID;
	material.TexturePath = texture;
	material.NormalMapID = normalMapID;
	material.UseTexture = true;
	material.Metallic = 0.f;
	material.Roughness = 0.82f;
	material.Fresnel = 0.04f;

	material.ShaderClassMode = MaterialMode::Manual;
	material.ShaderClass = ShaderClass::PBR;

	VertexResource vertexResource{};
	vertexResource.entityid = entity.GetID();
	vertexResource.shapetype = ShapeType::QUAD;
	vertexResource.radius =	10.f;
	vertexResource.color = Color::WHITE;

	RendererResource::CreateSpriteVertex(vertexResource);
}

