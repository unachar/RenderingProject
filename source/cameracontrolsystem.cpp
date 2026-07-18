#include "pch.h"
#include "cameracontrolsystem.h"
#include "componentmanager.h"
#include "world.h"

void CameraControlSystem::Update()
{
	float dt = World::GetDeltaTime();
	const float frameScale = dt * 60.0f;

	ComponentManager::ForEach<CameraComponent, InputComponent, MoveComponent, TransformComponent>(
		[frameScale](EntityID, CameraComponent& camera, InputComponent& input,
			MoveComponent& move, TransformComponent& transform)
		{
		if (!input.IsActive)
		{
			return;
		}

		if (!move.CanMove)
		{
			return;
		}

		float rotInputY = 0.0f;
		if (input.RotateLeft) rotInputY = -1.0f;
		if (input.RotateRight) rotInputY = 1.0f;

		float rotInputZ = 0.0f;
		if (input.RotateUp) rotInputZ = -1.0f;
		if (input.RotateDown) rotInputZ = 1.0f;

		float rotSpeed = move.RotationSpeed;

		transform.Rotation.y += rotInputY * rotSpeed * frameScale;
		transform.Rotation.x += rotInputZ * rotSpeed * frameScale;

		if (rotInputY != 0.0f || rotInputZ != 0.0f)
		{
			XMVECTOR eye = XMLoadFloat3(&transform.Position);
			XMVECTOR target = XMLoadFloat3(&camera.Target);
			float distance = XMVectorGetX(XMVector3Length(XMVectorSubtract(target, eye)));
			if (distance <= 0.0001f)
			{
				distance = 1.0f;
			}

			transform.Rotation.x = max(-XM_PIDIV2 + 0.01f, min(XM_PIDIV2 - 0.01f, transform.Rotation.x));
			XMVECTOR forward = XMVector3TransformNormal(
				XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
				XMMatrixRotationRollPitchYaw(transform.Rotation.x, transform.Rotation.y, 0.0f));
			target = XMVectorAdd(eye, XMVectorScale(XMVector3Normalize(forward), distance));
			XMStoreFloat3(&camera.Target, target);
			camera.LockOnTarget = g_kINVALID_ENTITY;
		}

		transform.IsDirty = true;
		});
}
