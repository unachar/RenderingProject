#include "pch.h"
#include "movementsystem.h"
#include "componentmanager.h"
#include "world.h"
#include "camera.h"

void MovementSystem::Update()
{
	float dt = World::GetDeltaTime();
	const float frameScale = dt * 60.0f;

	EntityID cameraEntity = Camera::GetCameraEntity();
	bool hasCameraBasis = false;
	XMFLOAT3 cameraForward = { 0.0f, 0.0f, 1.0f };
	XMFLOAT3 cameraRight = { 1.0f, 0.0f, 0.0f };

	if (cameraEntity != g_kINVALID_ENTITY &&
		Registry::HasComponent(cameraEntity, ComponentType::TRANSFORM) &&
		Registry::HasComponent(cameraEntity, ComponentType::CAMERA))
	{
		auto& cameraTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
		auto& cameraComp = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);

		cameraForward =
		{
			cameraComp.Target.x - cameraTransform.Position.x,
			cameraComp.Target.y - cameraTransform.Position.y,
			cameraComp.Target.z - cameraTransform.Position.z
		};

		float forwardLenSq = cameraForward.x * cameraForward.x + cameraForward.y * cameraForward.y + cameraForward.z * cameraForward.z;
		if (forwardLenSq > 0.0001f)
		{
			float invLen = 1.0f / sqrtf(forwardLenSq);
			cameraForward.x *= invLen;
			cameraForward.y *= invLen;
			cameraForward.z *= invLen;
			XMVECTOR right = XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), XMLoadFloat3(&cameraForward));
			if (XMVectorGetX(XMVector3LengthSq(right)) > 0.0001f)
			{
				XMStoreFloat3(&cameraRight, XMVector3Normalize(right));
			}
			hasCameraBasis = true;
		}
	}

	auto applyMove = [&](EntityID i, bool usePhysics)
		{
			auto& input = ComponentManager::GetComponentUnchecked<InputComponent>(i);
			if (!input.IsActive)
			{
				return;
			}

			auto& move = ComponentManager::GetComponentUnchecked<MoveComponent>(i);
			if (!move.CanMove)
			{
				return;
			}

			auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(i);

			XMFLOAT3 moveInput = { 0.0f, 0.0f, 0.0f };
			if (input.MoveLeft) moveInput.x = -1.0f;
			if (input.MoveRight) moveInput.x = 1.0f;
			if (input.MoveForward) moveInput.z = 1.0f;
			if (input.MoveBackward) moveInput.z = -1.0f;
			if (input.MoveUp) moveInput.y = 1.0f;
			if (input.MoveDown) moveInput.y = -1.0f;

			XMFLOAT3 moveDir = moveInput;
			if (move.UseCameraRelativeMovement && hasCameraBasis)
			{
				moveDir.x = cameraRight.x * moveInput.x + cameraForward.x * moveInput.z;
				moveDir.y = cameraRight.y * moveInput.x + cameraForward.y * moveInput.z + moveInput.y;
				moveDir.z = cameraRight.z * moveInput.x + cameraForward.z * moveInput.z;
			}

			float speed = move.Speed;

			if (usePhysics)
			{
				auto& physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(i);
				physics.Velocity.x += moveDir.x * speed;
				physics.Velocity.y += moveDir.y * speed;
				physics.Velocity.z += moveDir.z * speed;
			}
			else
			{
				const float deltaX = moveDir.x * speed * frameScale;
				const float deltaY = moveDir.y * speed * frameScale;
				const float deltaZ = moveDir.z * speed * frameScale;
				transform.Position.x += deltaX;
				transform.Position.y += deltaY;
				transform.Position.z += deltaZ;

				if (Registry::HasComponent(i, ComponentType::CAMERA))
				{
					auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(i);
					if (camera.LockOnTarget == g_kINVALID_ENTITY)
					{
						camera.Target.x += deltaX;
						camera.Target.y += deltaY;
						camera.Target.z += deltaZ;
					}
				}
			}

			transform.IsDirty = true;
		};

	auto moveView = World::GetView<InputComponent, MoveComponent, TransformComponent>();
	for (EntityID i : moveView)
	{
		applyMove(i, Registry::HasComponent(i, ComponentType::PHYSICS));
	}
}
