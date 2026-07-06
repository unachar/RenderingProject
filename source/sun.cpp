#include "pch.h"
#include "sun.h"
#include "world.h"
#include "rendererresource.h"

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

	XMFLOAT3 NormalizeOr(const XMVECTOR& vector, const XMFLOAT3& fallback)
	{
		if (XMVectorGetX(XMVector3LengthSq(vector)) <= 0.000001f)
		{
			return fallback;
		}

		XMFLOAT3 result{};
		XMStoreFloat3(&result, XMVector3Normalize(vector));
		return result;
	}
}

EntityID Sun::CreateDefault()
{
	return Create({ -18.0f, 24.0f, -12.0f }, { 0.0f, 0.0f, 0.0f });
}

EntityID Sun::Create(const XMFLOAT3& position, const XMFLOAT3& target)
{
	Entity entity = World::CreateEntity()
		.Add<NameComponent>()
		.Add<TransformComponent>()
		.Add<MeshComponent>()
		.Add<MaterialComponent>()
		.Add<LightComponent>()
		.Add<SunComponent>();

	entity.SetName("Sun");

	auto& transform = entity.Get<TransformComponent>();
	transform.Position = position;
	transform.Scale = { 2.5f, 2.5f, 2.5f };
	XMStoreFloat4x4(&transform.WorldMatrix, BuildWorldMatrix(transform));
	transform.IsDirty = true;

	auto& sun = entity.Get<SunComponent>();
	sun.Target = target;
	sun.VisualRadius = 2.5f;
	sun.SyncDirectionalLight = true;

	auto& light = entity.Get<LightComponent>();
	light.Type = LightType::Directional;
	light.Color = { 1.0f, 0.94f, 0.82f, 1.0f };
	light.Intensity = 3.0f;
	light.Range = 80.0f;
	light.InnerAngle = 18.0f;
	light.OuterAngle = 32.0f;
	light.VolumeDensity = 0.22f;
	light.VolumeShape = 0;
	light.IsActive = true;
	light.DrawDebug = true;

	auto& material = entity.Get<MaterialComponent>();
	material.ShaderClassMode = MaterialMode::Manual;
	material.ShaderClass = ShaderClass::Unlit;
	material.BaseBrightness = 2.2f;
	material.UseTexture = false;

	VertexResource resource{};
	resource.entityid = entity.GetID();
	resource.color = Color::YELLOW;
	resource.objectType = ObjectType::SPHERE;
	resource.radius = 1.0f;
	RendererResource::CreateObjectVertex(resource);

	Sync(entity.GetID());
	return entity.GetID();
}

void Sun::SyncAll()
{
	for (EntityID entity : World::GetView<SunComponent, TransformComponent, LightComponent>())
	{
		Sync(entity);
	}
}

void Sun::Sync(EntityID entity)
{
	if (!Registry::IsAlive(entity) ||
		!ComponentManager::HasComponent<SunComponent>(entity) ||
		!ComponentManager::HasComponent<TransformComponent>(entity) ||
		!ComponentManager::HasComponent<LightComponent>(entity))
	{
		return;
	}

	const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
	const auto& sun = ComponentManager::GetComponentUnchecked<SunComponent>(entity);
	auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
	if (!sun.SyncDirectionalLight)
	{
		return;
	}

	const XMVECTOR sunPosition = XMLoadFloat3(&transform.Position);
	const XMVECTOR target = XMLoadFloat3(&sun.Target);
	light.Type = LightType::Directional;
	light.Direction = NormalizeOr(XMVectorSubtract(sunPosition, target), { 0.0f, 1.0f, -0.25f });
	light.Range = max(light.Range, 1.0f);
	light.Position = transform.Position;
}
