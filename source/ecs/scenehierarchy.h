#pragma once
#include "componentmanager.h"
#include "hierarchytransforms.h"
#include "transformmath.h"
#include <cstring>

namespace SceneHierarchy
{
    inline bool HasTransform(EntityID entity)
    {
        return Registry::IsAlive(entity) && ComponentManager::HasComponent<TransformComponent>(entity);
    }

    inline EntityID Parent(EntityID entity)
    {
        if (!HasTransform(entity))
        {
            return g_kINVALID_ENTITY;
        }
        const EntityID parent = ComponentManager::GetComponentUnchecked<TransformComponent>(entity).Parent;
        if (!HasTransform(parent))
        {
            return g_kINVALID_ENTITY;
        }
        return parent;
    }

    inline std::vector<EntityID> Entities()
    {
        std::vector<EntityID> entities;
        ComponentManager::ForEachComponent<TransformComponent>([&](EntityID entity, TransformComponent&) {
            entities.push_back(entity);
        });
        return entities;
    }

    inline void ResetParentFrame(EntityID entity)
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        const EntityID parent = Parent(entity);
        transform.HasParentWorld = parent != g_kINVALID_ENTITY;
        if (transform.HasParentWorld)
        {
            const auto& parentTransform = ComponentManager::GetComponentUnchecked<TransformComponent>(parent);
            DirectX::XMStoreFloat4x4(&transform.ParentWorldMatrix, TransformMath::World(parentTransform));
        }
    }

    inline bool SetParent(EntityID entity, EntityID parent)
    {
        if (!HasTransform(entity) || (parent != g_kINVALID_ENTITY && !HasTransform(parent)) ||
            !EntityHierarchy::CanReparent(entity, parent, Parent))
        {
            return false;
        }
        ComponentManager::GetComponentUnchecked<TransformComponent>(entity).Parent = parent;
        ResetParentFrame(entity);
        return true;
    }

    inline void Update()
    {
        EntityHierarchy::UpdateTransforms(
            Entities(),
            [](EntityID entity) -> TransformComponent& {
                return ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            },
            Parent,
            [](EntityID entity) {
                GpuSceneChanges::Notify(entity, GpuSceneDirtyFlags::Transform | GpuSceneDirtyFlags::Bounds);
            });
    }
}
