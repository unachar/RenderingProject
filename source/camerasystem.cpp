#include "pch.h"
#include "camerasystem.h"
#include "componentmanager.h"
#include "systemmanager.h"
#include "rendererdraw.h"
#include "renderercore.h"
#include "world.h"
#include "input.h"
#include "camera.h"
#include "projectmanager.h"

static float Halton(int index, int base)
{
    float result = 0.0f;
    float invBase = 1.0f / (float)base;
    float f = invBase;
    while (index > 0)
    {
        result += (float)(index % base) * f;
        index /= base;
        f *= invBase;
    }
    return result;
}

void CameraSystem::Update()
{
	auto cameraEntities = World::GetView<CameraComponent, TransformComponent>();
	float aspectRatio = RendererCore::GetSceneAspectRatio();
	const EntityID activeCamera = Camera::GetCameraEntity();
	static EntityID previousActiveCamera = g_kINVALID_ENTITY;

	for (EntityID i : cameraEntities)
	{
		auto& cam = ComponentManager::GetComponentUnchecked<CameraComponent>(i);
		auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(i);
		const bool isActive = i == activeCamera;

		if (isActive)
		{
			if (previousActiveCamera == activeCamera)
			{
				RendererState::m_PrevViewMatrix = cam.ViewMatrix;
				RendererState::m_PrevProjMatrix = cam.ProjectionMatrix;
			}
			else
			{
				RendererState::m_PrevViewMatrix = {};
				RendererState::m_PrevProjMatrix = {};
			}
		}

		if (isActive && cam.AllowUserControl && Input::IsKeyHeld(VK_RBUTTON))
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

		// Edit mode does not execute gameplay input/movement systems.  Keep a
		// dedicated editor fly camera available there without running scripts.
		if (isActive && !ProjectManager::IsPlaying() && !cam.IsGameCamera &&
			cam.AllowUserControl && Input::IsKeyHeld(VK_RBUTTON))
		{
			XMFLOAT3 input = {};
			if (Input::IsKeyHeld('A')) input.x -= 1.0f;
			if (Input::IsKeyHeld('D')) input.x += 1.0f;
			if (Input::IsKeyHeld('Q')) input.y -= 1.0f;
			if (Input::IsKeyHeld('E')) input.y += 1.0f;
			if (Input::IsKeyHeld('S')) input.z -= 1.0f;
			if (Input::IsKeyHeld('W')) input.z += 1.0f;

			const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
				transform.Rotation.x, transform.Rotation.y, 0.0f);
			XMVECTOR direction = XMVectorAdd(
				XMVectorScale(XMVector3TransformNormal(
					XMVectorSet(1, 0, 0, 0), rotation), input.x),
				XMVectorAdd(
					XMVectorScale(XMVectorSet(0, 1, 0, 0), input.y),
					XMVectorScale(XMVector3TransformNormal(
						XMVectorSet(0, 0, 1, 0), rotation), input.z)));
			if (XMVectorGetX(XMVector3LengthSq(direction)) > 0.0001f)
			{
				direction = XMVector3Normalize(direction);
				float speed = 0.1f;
				if (ComponentManager::HasComponent<MoveComponent>(i))
				{
					speed = ComponentManager::GetComponentUnchecked<MoveComponent>(i).Speed;
				}
				if (Input::IsKeyHeld(VK_SHIFT))
				{
					speed *= 4.0f;
				}
				const XMVECTOR delta = XMVectorScale(
					direction, speed * World::GetDeltaTime() * 60.0f);
				XMVECTOR position = XMVectorAdd(XMLoadFloat3(&transform.Position), delta);
				XMStoreFloat3(&transform.Position, position);
				if (cam.LockOnTarget == g_kINVALID_ENTITY)
				{
					XMVECTOR target = XMVectorAdd(XMLoadFloat3(&cam.Target), delta);
					XMStoreFloat3(&cam.Target, target);
				}
				transform.IsDirty = true;
			}
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

		if (RendererState::m_AntiAliasingMode == AntiAliasingMode::TAA)
		{
			float haltonX = Halton(RendererState::m_TaaFrameIndex, 2);
			float haltonY = Halton(RendererState::m_TaaFrameIndex, 3);

			// TAA runs before FSR/NIS at the internal render resolution. Jitter by
			// one internal pixel so low resolution modes retain useful sub-pixel
			// coverage instead of shrinking the sample pattern by the upscale ratio.
			float jitterX = (haltonX - 0.5f) * 2.0f /
				max((float)RendererCore::GetSceneWidth(), 1.0f);
			float jitterY = (haltonY - 0.5f) * 2.0f /
				max((float)RendererCore::GetSceneHeight(), 1.0f);

			proj.r[2].m128_f32[0] += jitterX;
			proj.r[2].m128_f32[1] += jitterY;
		}

		XMStoreFloat4x4(&cam.ViewMatrix, view);
		XMStoreFloat4x4(&cam.ProjectionMatrix, proj);
	}
	previousActiveCamera = activeCamera;
}

