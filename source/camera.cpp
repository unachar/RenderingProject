#include "pch.h"
#include "camera.h"
#include "world.h"
#include "componentmanager.h"
#include "projectmanager.h"

namespace
{
	bool IsCameraEntity(EntityID entity)
	{
		return entity != g_kINVALID_ENTITY &&
			Registry::IsAlive(entity) &&
			ComponentManager::HasComponent<CameraComponent>(entity);
	}

	Entity CreateCameraEntity(
		const string& name,
		const XMFLOAT3& position,
		const XMFLOAT3& target,
		float fov,
		bool gameCamera)
	{
		auto entity = World::CreateEntity()
			.Add<NameComponent>()
			.Add<TransformComponent>()
			.Add<InputComponent>()
			.Add<MoveComponent>()
			.Add<PostProcessComponent>()
			.Add<CameraComponent>();

		entity.SetName(name);
		entity.Get<TransformComponent>().Position = position;

		auto& camera = entity.Get<CameraComponent>();
		camera.Target = target;
		camera.Fov = clamp(fov, XMConvertToRadians(1.0f), XMConvertToRadians(179.0f));
		camera.NearClip = 0.1f;
		camera.FarClip = 1000.0f;
		camera.LockOnTarget = g_kINVALID_ENTITY;
		camera.LockOnOffset = { 0.0f, 4.0f, 0.0f };
		camera.EnablePostProcess = true;
		camera.IsGameCamera = gameCamera;
		camera.IsMainGameCamera = false;
		camera.AllowUserControl = true;

		entity.Get<PostProcessComponent>().Type = PostProcessType::NONE;
		// Only the camera selected by SetMainGameCamera receives gameplay input.
		// Additional GameCameras remain available for timeline/cut switching.
		entity.Get<InputComponent>().IsActive = false;
		auto& move = entity.Get<MoveComponent>();
		move.Speed = 0.1f;
		move.RotationSpeed = 0.02f;
		move.UseCameraRelativeMovement = true;
		return entity;
	}
}

void Camera::Create(XMFLOAT3 target, float fov)
{
	Entity editorCamera = CreateCameraEntity(
		"EditorCamera", { 5.0f, 4.0f, -10.0f }, target, fov, false);
	m_EditorCameraEntity = editorCamera.GetID();
}

EntityID Camera::CreateGameCamera(
	const string& name,
	XMFLOAT3 position,
	XMFLOAT3 target,
	float fov,
	bool makeMain)
{
	Entity gameCamera = CreateCameraEntity(
		name.empty() ? "GameCamera" : name,
		position,
		target,
		fov,
		true);
	if (makeMain || GetGameCameraEntity() == g_kINVALID_ENTITY)
	{
		SetMainGameCamera(gameCamera.GetID());
	}
	return gameCamera.GetID();
}

EntityID Camera::GetCameraEntity()
{
	if (ProjectManager::IsPlaying())
	{
		const EntityID gameCamera = GetGameCameraEntity();
		if (gameCamera != g_kINVALID_ENTITY)
		{
			return gameCamera;
		}
	}
	return GetEditorCameraEntity();
}

EntityID Camera::GetEditorCameraEntity()
{
	if (IsCameraEntity(m_EditorCameraEntity) &&
		!ComponentManager::GetComponentUnchecked<CameraComponent>(
			m_EditorCameraEntity).IsGameCamera)
	{
		return m_EditorCameraEntity;
	}
	auto cameraEntities = World::GetView<CameraComponent>();
	for (EntityID entity : cameraEntities)
	{
		if (!ComponentManager::GetComponentUnchecked<CameraComponent>(entity).IsGameCamera)
		{
			m_EditorCameraEntity = entity;
			return entity;
		}
	}
	m_EditorCameraEntity = g_kINVALID_ENTITY;
	return g_kINVALID_ENTITY;
}

EntityID Camera::GetGameCameraEntity()
{
	if (IsCameraEntity(m_GameCameraEntity))
	{
		const auto& cached =
			ComponentManager::GetComponentUnchecked<CameraComponent>(m_GameCameraEntity);
		if (cached.IsGameCamera && cached.IsMainGameCamera)
		{
			return m_GameCameraEntity;
		}
	}

	EntityID fallback = g_kINVALID_ENTITY;
	int fallbackPriority = numeric_limits<int>::min();
	for (EntityID entity : World::GetView<CameraComponent>())
	{
		const auto& camera =
			ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
		if (!camera.IsGameCamera)
		{
			continue;
		}
		if (camera.IsMainGameCamera)
		{
			m_GameCameraEntity = entity;
			return entity;
		}
		if (fallback == g_kINVALID_ENTITY || camera.Priority > fallbackPriority)
		{
			fallback = entity;
			fallbackPriority = camera.Priority;
		}
	}
	m_GameCameraEntity = fallback;
	return fallback;
}

void Camera::SetMainGameCamera(EntityID entity)
{
	if (!IsCameraEntity(entity))
	{
		return;
	}
	for (EntityID cameraEntity : World::GetView<CameraComponent>())
	{
		auto& camera =
			ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);
		if (camera.IsGameCamera)
		{
			camera.IsMainGameCamera = cameraEntity == entity;
			if (ComponentManager::HasComponent<InputComponent>(cameraEntity))
			{
				ComponentManager::GetComponentUnchecked<InputComponent>(
					cameraEntity).IsActive = cameraEntity == entity;
			}
		}
	}
	auto& selected = ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
	selected.IsGameCamera = true;
	selected.IsMainGameCamera = true;
	if (ComponentManager::HasComponent<InputComponent>(entity))
	{
		ComponentManager::GetComponentUnchecked<InputComponent>(entity).IsActive = true;
	}
	m_GameCameraEntity = entity;
}

void Camera::GetCameraMatrices(EntityID camera, XMMATRIX& view, XMMATRIX& proj)
{
	view = XMMatrixIdentity();
	proj = XMMatrixIdentity();
	if (camera != g_kINVALID_ENTITY && Registry::HasComponent(camera, ComponentType::CAMERA))
	{
		auto& cam = ComponentManager::GetComponentUnchecked<CameraComponent>(camera);
		view = XMLoadFloat4x4(&cam.ViewMatrix);
		proj = XMLoadFloat4x4(&cam.ProjectionMatrix);
	}
}

void Camera::SetCameraPostProcess(PostProcessType type)
{
	const EntityID cameraEntity = GetCameraEntity();
	if (cameraEntity != g_kINVALID_ENTITY && Registry::HasComponent(cameraEntity, ComponentType::POST_PROCESS))
	{
		auto& postProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(cameraEntity);
		postProcess.Type = type;
		Debug::Log("Camera post-process set to %d\n", static_cast<int>(type));
	}
}

PostProcessType Camera::GetCameraPostProcess()
{
	const EntityID cameraEntity = GetCameraEntity();
	if (cameraEntity != g_kINVALID_ENTITY && Registry::HasComponent(cameraEntity, ComponentType::POST_PROCESS))
	{
		auto& postProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(cameraEntity);
		return postProcess.Type;
	}
	return PostProcessType::NONE;
}

void Camera::SetCameraPostProcessIntensity(float intensity)
{
	const EntityID cameraEntity = GetCameraEntity();
	if (cameraEntity != g_kINVALID_ENTITY && Registry::HasComponent(cameraEntity, ComponentType::POST_PROCESS))
	{
		auto& postProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(cameraEntity);
		postProcess.Intensity = intensity;
		Debug::Log("Camera post-process intensity set to %f\n", intensity);
	}
}

float Camera::GetCameraPostProcessIntensity()
{
	const EntityID cameraEntity = GetCameraEntity();
	if (cameraEntity != g_kINVALID_ENTITY && Registry::HasComponent(cameraEntity, ComponentType::POST_PROCESS))
	{
		auto& postProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(cameraEntity);
		return postProcess.Intensity;
	}
	return 0.0f;
}

