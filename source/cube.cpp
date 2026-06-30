#include "pch.h"
#include "cube.h"
#include "world.h"
#include "timelinesystem.h"

void Cube::Create()
{
	const auto name = "Cube";
	auto cubePosition = XMFLOAT3(-3.0f, 6.0f, 0.0f);
	auto cubeScale = XMFLOAT3(1.0f, 1.0f, 1.0f);



	//Entityの作成
	auto entity = World::CreateEntity()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<MaterialComponent>()
		.Add<NameComponent>();

	entity.SetName(name);
	entity.Get<TransformComponent>().Position = cubePosition;
	entity.Get<TransformComponent>().Scale = cubeScale;



	//MaterialComponentの設定
	auto& material = entity.Get<MaterialComponent>();
	material.Metallic = 0.0f;
	material.Roughness = 0.5f;
	material.Fresnel = 0.04f;
	material.ShaderClassMode = MaterialMode::Manual;
	material.ShaderClass = ShaderClass::PBR;
	material.UseTexture = false;




	//頂点情報の設定
	VertexResource resource{};
	resource.entityid = entity.GetID();
	resource.color = Color::WHITE;
	resource.objectType = ObjectType::CUBE;
	RendererResource::CreateObjectVertex(resource);





	//TimeLine設定
	auto& director = TimeLineSystem::CreateDirector();
	auto& asset = director.CreateAsset();



	// 5秒間のアニメーションを設定
	asset.SetDuration(5.0f);



	// TransformTrackの作成と回転アニメーションの設定
	auto& track = asset.AddTransformTrack(entity.GetID());
	auto& clip = track.AddClip(0.0f, 5.0f);




	// Metallicのアニメーションの設定
	auto& metallicTrack = asset.AddComponentTrack<MaterialComponent>(entity.GetID());
	auto& metallicClip = metallicTrack.AddFloatClip(0.0f, 5.0f, &MaterialComponent::Metallic);




	// Roughnessのアニメーションの設定
	auto& roughnessTrack = asset.AddComponentTrack<MaterialComponent>(entity.GetID());
	auto& roughnessClip = roughnessTrack.AddFloatClip(0.0f, 5.0f, &MaterialComponent::Roughness);

	


	//第一引数は時間、第二引数は（X, Y, Z）を指定

	// キーフレームの追加
	clip.AddRotationKey(0.0f, { 0.0f, 0.0f, 0.0f });
	clip.AddRotationKey(5.0f, { 0.0f, 10.0f, 0.0f });



	clip.AddScaleKey(0.0f, { 1.0f, 1.0f, 1.0f });
	clip.AddScaleKey(2.5f, { 10.0f,1.0f, 1.0f });
	clip.AddScaleKey(5.0f, { 1.0f, 1.0f, 1.0f });



	// 第一引数は時間、第二引数は値(float)を指定

	metallicClip.AddKey(0.0f, 0.0f);
	metallicClip.AddKey(5.0f, 1.0f);



	roughnessClip.AddKey(0.0f, 0.5f);
	roughnessClip.AddKey(5.0f, 0.0f);




	// TimeLineDirectorの設定
	director.SetLoop(true);
	director.Play();
}
