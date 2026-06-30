#include "pch.h"
#include "camerasystem.h"
#include "componentmanager.h"
#include "systemmanager.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "world.h"
#include "input.h"

void CameraSystem::Update()
{
	auto cameraEntities = World::GetView<CameraComponent, TransformComponent>();
	float aspectRatio = RendererCore::GetSceneAspectRatio();

	for (EntityID i : cameraEntities)
	{
		auto& cam = ComponentManager::GetComponentUnchecked<CameraComponent>(i);
		auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(i);

		if (Input::IsKeyHeld(VK_RBUTTON))
		{
			const POINT mouseDelta = Input::GetMouseDelta();
			const float sensitivity = 0.004f;
			transform.Rotation.y += static_cast<float>(mouseDelta.x) * sensitivity;
			transform.Rotation.x += static_cast<float>(mouseDelta.y) * sensitivity;
			transform.Rotation.x = max(-XM_PIDIV2 + 0.01f, min(XM_PIDIV2 - 0.01f, transform.Rotation.x));
			cam.LockOnTarget = g_kINVALID_ENTITY;

			XMVECTOR forward = XMVector3TransformNormal(
				XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
				XMMatrixRotationRollPitchYaw(transform.Rotation.x, transform.Rotation.y, 0.0f));
			XMVECTOR target = XMVectorAdd(XMLoadFloat3(&transform.Position), forward);
			XMStoreFloat3(&cam.Target, target);
		}

		if (cam.LockOnTarget != g_kINVALID_ENTITY &&
			Registry::IsAlive(cam.LockOnTarget) &&
			Registry::HasComponent(cam.LockOnTarget, ComponentType::TRANSFORM))
		{
			
			cam.Target.x = ComponentManager::GetComponentUnchecked<TransformComponent>(cam.LockOnTarget).Position.x + cam.LockOnOffset.x;
			cam.Target.y = ComponentManager::GetComponentUnchecked<TransformComponent>(cam.LockOnTarget).Position.y + cam.LockOnOffset.y;
			cam.Target.z = ComponentManager::GetComponentUnchecked<TransformComponent>(cam.LockOnTarget).Position.z + cam.LockOnOffset.z;
		}

		XMVECTOR eye = XMLoadFloat3(&transform.Position);
		XMVECTOR at = XMLoadFloat3(&cam.Target);
		XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		XMMATRIX view = XMMatrixLookAtLH(eye, at, up);

		XMMATRIX proj = XMMatrixPerspectiveFovLH(
			cam.Fov, aspectRatio, cam.NearClip, cam.FarClip);

		XMStoreFloat4x4(&cam.ViewMatrix, view);
		XMStoreFloat4x4(&cam.ProjectionMatrix, proj);
	}
}

