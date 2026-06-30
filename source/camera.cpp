#include "pch.h"
#include "camera.h"
#include "world.h"
#include "componentmanager.h"


void Camera::Create(XMFLOAT3 target, float fov)
{
	const auto name = "MainCamera";
	const auto nearClip = 0.1f;
	const auto farClip = 1000.0f;
	auto cameraPosition = XMFLOAT3({ 5.0f, 4.0f, -10.0f });




	auto entity = World::CreateEntity()
		.Add<NameComponent>()
		.Add<TransformComponent>()
		.Add<InputComponent>()
		.Add<MoveComponent>()
		.Add<PostProcessComponent>()
		.Add<CameraComponent>();

	entity.SetName(name);
	entity.Get<TransformComponent>().Position = cameraPosition;

	auto& camera = entity.Get<CameraComponent>();
	camera.Target = target;
	camera.Fov = fov;
	camera.NearClip = nearClip;
	camera.FarClip = farClip;
	camera.LockOnTarget = g_kINVALID_ENTITY;
	camera.LockOnOffset = { 0.0f, 4.0f, 0.0f };

	entity.Get<PostProcessComponent>().Type = PostProcessType::NONE;
	entity.Get<InputComponent>().IsActive = true;
	entity.Get<CameraComponent>().EnablePostProcess = true;
	m_PrimaryCameraEntity = entity.GetID();

	entity.Get<MoveComponent>().Speed = 0.1f;
	entity.Get<MoveComponent>().UseCameraRelativeMovement = true;
}

EntityID Camera::GetCameraEntity()
{
	if (m_PrimaryCameraEntity != g_kINVALID_ENTITY &&
		Registry::IsAlive(m_PrimaryCameraEntity) &&
		Registry::HasComponent(m_PrimaryCameraEntity, ComponentType::CAMERA))
	{
		return m_PrimaryCameraEntity;
	}

	auto cameraEntities = World::GetView<CameraComponent>();
	if (!cameraEntities.empty())
	{
		m_PrimaryCameraEntity = *cameraEntities.begin();
		return m_PrimaryCameraEntity;
	}
	m_PrimaryCameraEntity = g_kINVALID_ENTITY;
	return g_kINVALID_ENTITY;
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
	if (m_PrimaryCameraEntity != g_kINVALID_ENTITY && Registry::HasComponent(m_PrimaryCameraEntity, ComponentType::POST_PROCESS))
	{
		auto& postProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(m_PrimaryCameraEntity);
		postProcess.Type = type;
		Debug::Log("Camera post-process set to %d\n", static_cast<int>(type));
	}
}

PostProcessType Camera::GetCameraPostProcess()
{
	if (m_PrimaryCameraEntity != g_kINVALID_ENTITY && Registry::HasComponent(m_PrimaryCameraEntity, ComponentType::POST_PROCESS))
	{
		auto& postProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(m_PrimaryCameraEntity);
		return postProcess.Type;
	}
	return PostProcessType::NONE;
}

void Camera::SetCameraPostProcessIntensity(float intensity)
{
	if (m_PrimaryCameraEntity != g_kINVALID_ENTITY && Registry::HasComponent(m_PrimaryCameraEntity, ComponentType::POST_PROCESS))
	{
		auto& postProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(m_PrimaryCameraEntity);
		postProcess.Intensity = intensity;
		Debug::Log("Camera post-process intensity set to %f\n", intensity);
	}
}

float Camera::GetCameraPostProcessIntensity()
{
	if (m_PrimaryCameraEntity != g_kINVALID_ENTITY && Registry::HasComponent(m_PrimaryCameraEntity, ComponentType::POST_PROCESS))
	{
		auto& postProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(m_PrimaryCameraEntity);
		return postProcess.Intensity;
	}
	return 0.0f;
}

