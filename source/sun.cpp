#include "pch.h"
#include "sun.h"
#include "world.h"
#include "rendererresource.h"


	static XMMATRIX BuildWorldMatrix(const TransformComponent& transform)
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
	light.Position = position;
	light.Color = { 1.0f, 0.94f, 0.82f, 1.0f };
	light.Intensity = 3.0f;
	light.Range = 80.0f;
	light.InnerAngle = 18.0f;
	light.OuterAngle = 32.0f;
	light.VolumeDensity = 0.22f;
	light.AffectsOpaque = true;
	light.AffectsForward = true;
	light.AffectsVolumetrics = true;
	light.RenderMode = LightRenderMode::Physical;
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

	const auto& sun = ComponentManager::GetComponentUnchecked<SunComponent>(entity);
	auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
	auto& writableTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);

	// Perf: 太陽はほぼ静止物のため、入力が変わっていない場合は行列再計算を省略。
	static EntityID s_LastEntity = g_kINVALID_ENTITY;
	static XMFLOAT3 s_LastPosition{};
	static XMFLOAT3 s_LastScale{};
	static XMFLOAT3 s_LastTarget{};
	static float s_LastVisualRadius = 0.0f;
	static bool s_LastSyncFlag = false;
	static bool s_HasLastState = false;
	const bool stateUnchanged = s_HasLastState && s_LastEntity == entity &&
		fabsf(s_LastPosition.x - writableTransform.Position.x) <= 0.000001f &&
		fabsf(s_LastPosition.y - writableTransform.Position.y) <= 0.000001f &&
		fabsf(s_LastPosition.z - writableTransform.Position.z) <= 0.000001f &&
		fabsf(s_LastScale.x - writableTransform.Scale.x) <= 0.000001f &&
		fabsf(s_LastScale.y - writableTransform.Scale.y) <= 0.000001f &&
		fabsf(s_LastScale.z - writableTransform.Scale.z) <= 0.000001f &&
		fabsf(s_LastTarget.x - sun.Target.x) <= 0.000001f &&
		fabsf(s_LastTarget.y - sun.Target.y) <= 0.000001f &&
		fabsf(s_LastTarget.z - sun.Target.z) <= 0.000001f &&
		fabsf(s_LastVisualRadius - sun.VisualRadius) <= 0.000001f &&
		s_LastSyncFlag == sun.SyncDirectionalLight;
	if (stateUnchanged)
	{
		return;
	}

	const float visualRadius = max(0.1f, sun.VisualRadius);
	if (fabsf(writableTransform.Scale.x - visualRadius) > 0.0001f ||
		fabsf(writableTransform.Scale.y - visualRadius) > 0.0001f ||
		fabsf(writableTransform.Scale.z - visualRadius) > 0.0001f)
	{
		writableTransform.Scale = { visualRadius, visualRadius, visualRadius };
		writableTransform.IsDirty = true;
	}

	// Perf: 次フレームの早期終了判定用に入力状態を記録 (スケール補正後の値で)。
	s_LastEntity = entity;
	s_LastPosition = writableTransform.Position;
	s_LastScale = writableTransform.Scale;
	s_LastTarget = sun.Target;
	s_LastVisualRadius = sun.VisualRadius;
	s_LastSyncFlag = sun.SyncDirectionalLight;
	s_HasLastState = true;

	XMStoreFloat4x4(&writableTransform.WorldMatrix, BuildWorldMatrix(writableTransform));

	if (!sun.SyncDirectionalLight)
	{
		return;
	}

	const XMVECTOR sunPosition = XMLoadFloat3(&writableTransform.Position);
	const XMVECTOR target = XMLoadFloat3(&sun.Target);
	light.Type = LightType::Directional;
	light.Direction = NormalizeOr(XMVectorSubtract(sunPosition, target), { 0.0f, 1.0f, -0.25f });
	light.Range = max(light.Range, 1.0f);
	light.Position = writableTransform.Position;
}
