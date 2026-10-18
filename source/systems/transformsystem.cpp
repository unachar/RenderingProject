#include "pch.h"
#include "transformmath.h"
#include "transformsystem.h"
#include "componentmanager.h"
#include "world.h"

void TransformSystem::Update()
{
    ComponentManager::ForEachComponent<TransformComponent>([](EntityID entity, TransformComponent& transform) {
        if (transform.IsDirty)
        {
            const XMMATRIX world = TransformMath::World(transform);
            XMStoreFloat4x4(&transform.WorldMatrix, world);
            transform.IsDirty = false;
            GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Transform | GpuSceneDirtyFlags::Bounds);
        }
    });
}
