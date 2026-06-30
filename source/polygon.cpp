#include "pch.h"
#include "polygon.h"
#include "rendererdraw.h"
#include "world.h"
#include "texturemanager.h"

void Polygon::Create()
{
	const auto name = "polygon";
	const auto vsPath = "shader/hlsl/build/colorshaderVS.cso";
	const auto psPath = "shader/hlsl/build/colorshaderPS.cso";

	const auto texture = "asset/texture/neko.png";

	auto entity = World::CreateEntity()
		.Add<SpriteComponent>()
		.Add<ShaderComponent>()
		.Add<MaterialComponent>()
		.Add<NameComponent>();

	

	entity.SetName(name);
	entity.Get<ShaderComponent>().VsPath = vsPath;
	entity.Get<ShaderComponent>().PsPath = psPath;



	int textureID = TextureManager::LoadTexture(texture);
	entity.Get<MaterialComponent>().TextureID = textureID;
	entity.Get<MaterialComponent>().UseTexture = true;


	VertexResource vertexResource{};
	vertexResource.entityid = entity.GetID();
	vertexResource.shapetype = ShapeType::CIRCLE;
	vertexResource.radius = 0.5f;
	vertexResource.color = Color::WHITE;

	RendererResource::CreateSpriteVertex(vertexResource);
}
