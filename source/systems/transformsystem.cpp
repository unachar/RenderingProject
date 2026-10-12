#include "pch.h"
#include "transformsystem.h"
#include "componentmanager.h"
#include "world.h"

void TransformSystem::Update()
{
	ComponentManager::ForEachComponent<TransformComponent>([](EntityID entity, TransformComponent& transform)
		{
		if (transform.IsDirty)
		{
			XMMATRIX world = XMMatrixScaling(transform.Scale.x, transform.Scale.y, transform.Scale.z) *
				XMMatrixRotationX(transform.Rotation.x) *
				XMMatrixRotationY(transform.Rotation.y) *
				XMMatrixRotationZ(transform.Rotation.z) *
				XMMatrixTranslation(transform.Position.x, transform.Position.y, transform.Position.z);
			XMStoreFloat4x4(&transform.WorldMatrix, world);
			transform.IsDirty = false;
            GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Transform | GpuSceneDirtyFlags::Bounds);
		}
		});
}

