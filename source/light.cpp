#include "pch.h"
#include "light.h"
#include "world.h"
#include "timelinesystem.h"

namespace
{
	XMMATRIX BuildWorldMatrix(const TransformComponent& transform)
	{
		return XMMatrixScaling(transform.Scale.x, transform.Scale.y, transform.Scale.z) *
			XMMatrixRotationX(transform.Rotation.x) *
			XMMatrixRotationY(transform.Rotation.y) *
			XMMatrixRotationZ(transform.Rotation.z) *
			XMMatrixTranslation(transform.Position.x, transform.Position.y, transform.Position.z);
	}

	XMFLOAT3 NormalizeDirection(const XMFLOAT3& direction)
	{
		const XMVECTOR dir = XMLoadFloat3(&direction);
		if (XMVectorGetX(XMVector3LengthSq(dir)) <= 0.000001f)
		{
			return { 0.0f, 1.0f, -1.0f };
		}

		XMFLOAT3 normalized{};
		XMStoreFloat3(&normalized, XMVector3Normalize(dir));
		return normalized;
	}
}

Light::CreateDesc Light::MakeDefaultDesc(LightType type)
{
	CreateDesc desc{};
	desc.Type = type;
	desc.Name = string(GetTypeName(type)) + " Light";
	desc.Intensity = (type == LightType::Volume) ? 1.5f : 1.0f;
	desc.Range = (type == LightType::Volume) ? 6.0f : 8.0f;
	return desc;
}

EntityID Light::Create(const CreateDesc& desc)
{
	Entity entity = World::CreateEntity()
		.Add<NameComponent>()
		.Add<TransformComponent>()
		.Add<LightComponent>();


	string lightName;
	if (desc.Name.empty())
	{
		lightName = string(GetTypeName(desc.Type)) + " Light";
	}
	else
	{
		lightName = desc.Name;
	}


	const string name = lightName;
	entity.SetName(name);

	auto& transform = entity.Get<TransformComponent>();
	transform.Position = desc.Position;
	transform.Rotation = desc.Rotation;
	transform.Scale = desc.Scale;
	XMStoreFloat4x4(&transform.WorldMatrix, BuildWorldMatrix(transform));
	transform.IsDirty = true;

	auto& light = entity.Get<LightComponent>();
	light.Type = desc.Type;
	light.Direction = NormalizeDirection(desc.Direction);
	light.Color = desc.Color;
	light.Intensity = max(0.0f, desc.Intensity);
	light.Range = max(0.01f, desc.Range);
	light.InnerAngle = max(0.1f, min(89.0f, desc.InnerAngle));
	light.OuterAngle = max(light.InnerAngle + 0.1f, min(89.5f, desc.OuterAngle));
	light.VolumeDensity = max(0.0f, min(3.0f, desc.VolumeDensity));
	light.VolumeShape = desc.VolumeShape;
	light.IsActive = desc.IsActive;
	light.DrawDebug = desc.DrawDebug;

	return entity.GetID();
}

EntityID Light::Create(LightType type)
{
	return Create(MakeDefaultDesc(type));
}

EntityID Light::CreateDefaultDirectional()
{
	CreateDesc desc{};
	desc.Name = "Directional Light";
	desc.Type = LightType::Directional;
	desc.Position = { 0.0f, 5.0f, -5.0f };
	desc.Direction = { 0.25f, 1.0f, -0.25f };
	desc.Color = { 1.0f, 0.97f, 0.90f, 1.0f };
	desc.Intensity = 1.0f;
	desc.Range = 12.0f;
	desc.InnerAngle = 18.0f;
	desc.OuterAngle = 32.0f;
	desc.VolumeDensity = 0.35f;
	desc.IsActive = true;
	desc.DrawDebug = true;
	return Create(desc);
}

const char* Light::GetTypeName(LightType type)
{
	switch (type)
	{
	case LightType::Directional: return "Directional";
	case LightType::Point: return "Point";
	case LightType::Spot: return "Spot";
	case LightType::Volume: return "Volume";
	default: return "Light";
	}
}

void Light::AttachLightTimeLine(EntityID entity)
{
	if (entity == g_kINVALID_ENTITY ||
		!Registry::IsAlive(entity) ||
		!ComponentManager::HasComponent<TransformComponent>(entity))
	{
		return;
	}

	const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
	auto& director = TimeLineSystem::CreateDirector();
	auto& asset = director.CreateAsset();

	asset.SetDuration(10.0f);

	auto& lightTrack = asset.AddTransformTrack(entity);
	auto& lightClip = lightTrack.AddClip(0.0f, asset.GetDuration());

	lightClip.AddPositionKey(0.0f, transform.Position);
	lightClip.AddPositionKey(5.0f, { transform.Position.x + 3.0f, transform.Position.y + 2.0f, transform.Position.z + 3.0f });
	lightClip.AddPositionKey(10.0f, transform.Position);

	auto& colorTrack = asset.AddComponentTrack<LightComponent>(entity);
	auto& colorClip = colorTrack.AddVector4Clip(0.0f, asset.GetDuration(), &LightComponent::Color);
	colorClip.AddKey(0.0f, { 1.0f, 0.2f, 0.2f, 1.0f });
	colorClip.AddKey(5.0f, { 0.2f, 1.0f, 0.3f, 1.0f });
	colorClip.AddKey(10.0f, { 0.2f, 0.4f, 1.0f, 1.0f });

	director.SetLoop(true);
	director.Play();
}
