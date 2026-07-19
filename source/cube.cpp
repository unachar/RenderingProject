#include "pch.h"
#include "cube.h"
#include "world.h"
#include "timelinesystem.h"

void Cube::Create()
{
	const auto name = "Cube";
	auto cubePosition = XMFLOAT3(-3.0f, 6.0f, 0.0f);
	auto cubeScale = XMFLOAT3(1.0f, 1.0f, 1.0f);




	auto entity = World::CreateEntity()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<MaterialComponent>()
		.Add<NameComponent>();

	entity.SetName(name);
	entity.Get<TransformComponent>().Position = cubePosition;
	entity.Get<TransformComponent>().Scale = cubeScale;




	auto& material = entity.Get<MaterialComponent>();
	material.Metallic = 0.0f;
	material.Roughness = 0.5f;
	material.Fresnel = 0.04f;
	material.ShaderClassMode = MaterialMode::Manual;
	material.ShaderClass = ShaderClass::PBR;
	material.UseTexture = false;





	VertexResource resource{};
	resource.entityid = entity.GetID();
	resource.color = Color::WHITE;
	resource.objectType = ObjectType::CUBE;
	RendererResource::CreateObjectVertex(resource);






	auto& director = TimeLineSystem::CreateDirector();
	auto& asset = director.CreateAsset();




	asset.SetDuration(5.0f);




	auto& track = asset.AddTransformTrack(entity.GetID());
	auto& clip = track.AddClip(0.0f, asset.GetDuration());





	auto& metallicTrack = asset.AddComponentTrack<MaterialComponent>(entity.GetID());
	auto& metallicClip = metallicTrack.AddFloatClip(0.0f, asset.GetDuration(), &MaterialComponent::Metallic);





	auto& roughnessTrack = asset.AddComponentTrack<MaterialComponent>(entity.GetID());
	auto& roughnessClip = roughnessTrack.AddFloatClip(0.0f, asset.GetDuration(), &MaterialComponent::Roughness);







	clip.AddRotationKey(0.0f, { 0.0f, 0.0f, 0.0f });
	clip.AddRotationKey(5.0f, { 0.0f, 10.0f, 0.0f });



	clip.AddScaleKey(0.0f, { 1.0f, 1.0f, 1.0f });
	clip.AddScaleKey(2.5f, { 10.0f,1.0f, 1.0f });
	clip.AddScaleKey(5.0f, { 1.0f, 1.0f, 1.0f });





	metallicClip.AddKey(0.0f, 0.0f);
	metallicClip.AddKey(5.0f, 1.0f);



	roughnessClip.AddKey(0.0f, 0.5f);
	roughnessClip.AddKey(5.0f, 0.0f);





	director.SetLoop(true);
	director.Play();
}
