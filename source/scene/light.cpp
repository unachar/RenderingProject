#include "pch.h"
#include "transformmath.h"
#include "light.h"
#include "world.h"
#include "timelinesystem.h"


	static XMMATRIX BuildWorldMatrix(const TransformComponent& transform)
	{
		return TransformMath::World(transform);
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


Light::CreateDesc Light::MakeDefaultDesc(LightType type)
{
	CreateDesc desc{};
	desc.Type = type;
	desc.Name = string(GetTypeName(type)) + " Light";
	if ((type == LightType::Volume))
	{
		desc.Intensity = 1.5f;
	}
	else
	{
		desc.Intensity = 1.0f;
	}
	if ((type == LightType::Volume))
	{
		desc.Range = 6.0f;
	}
	else
	{
		desc.Range = 8.0f;
	}
	if ((type == LightType::Directional || type == LightType::Volume))
	{
		desc.VolumeDensity = 0.35f;
	}
	else
	{
		desc.VolumeDensity = 0.0f;
	}
	desc.AffectsOpaque = type != LightType::Volume;
	desc.AffectsForward = type != LightType::Volume;
	desc.AffectsVolumetrics =
		type == LightType::Directional || type == LightType::Volume;
	desc.CastShadow = (type == LightType::Directional);
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
	light.Position = desc.Position;
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
	light.AffectsOpaque = desc.AffectsOpaque;
	light.AffectsForward = desc.AffectsForward;
	light.AffectsVolumetrics = desc.AffectsVolumetrics;
	light.CastShadow = desc.CastShadow;
	light.Priority = desc.Priority;
	light.RenderMode = desc.RenderMode;

	return entity.GetID();
}

EntityID Light::Create(LightType type)
{
	return Create(MakeDefaultDesc(type));
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

