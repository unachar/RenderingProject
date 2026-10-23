#pragma once
#include "entityhierarchy.h"
#include "transformmath.h"
#include <cstring>

namespace EntityHierarchy
{
    // Resolve parents before children regardless of component storage order.
    template <typename TransformLookup, typename ParentLookup, typename Notify>
    void UpdateTransforms(const std::vector<Entity>& entities, TransformLookup transformAt, ParentLookup parent,
                          Notify notify)
    {
        using namespace DirectX;
        const auto refreshWorld = [&](Entity entity, auto& transform) {
            if (transform.IsDirty)
            {
                if constexpr (requires { transform.HasAffineWorld; })
                {
                    if (transform.HasAffineWorld &&
                        (!TransformMath::Same(transform.Position, transform.AffinePosition) ||
                         !TransformMath::Same(transform.Rotation, transform.AffineRotation) ||
                         !TransformMath::Same(transform.Scale, transform.AffineScale)))
                    {
                        transform.HasAffineWorld = false;
                    }
                }
                XMStoreFloat4x4(&transform.WorldMatrix, TransformMath::World(transform));
                transform.IsDirty = false;
                notify(entity);
            }
        };
        std::unordered_set<Entity> completed;
        completed.reserve(entities.size());
        // Most scene objects have no parent; avoid allocating a traversal chain for each root.
        for (Entity entity : entities)
        {
            if (parent(entity) == None)
            {
                auto& transform = transformAt(entity);
                transform.Parent = None;
                transform.HasParentWorld = false;
                refreshWorld(entity, transform);
                completed.insert(entity);
            }
        }
        for (Entity entity : entities)
        {
            if (completed.count(entity))
            {
                continue;
            }
            std::vector<Entity> chain;
            std::unordered_set<Entity> visiting;
            for (Entity current = entity; current != None && !completed.count(current); current = parent(current))
            {
                if (!visiting.insert(current).second)
                {
                    auto& cyclic = transformAt(current);
                    cyclic.Parent = None;
                    cyclic.HasParentWorld = false;
                    break;
                }
                chain.push_back(current);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                auto& transform = transformAt(*it);
                const Entity parentEntity = parent(*it);
                if (parentEntity != None)
                {
                    const auto& parentTransform = transformAt(parentEntity);
                    XMFLOAT4X4 parentWorld;
                    XMStoreFloat4x4(&parentWorld, TransformMath::World(parentTransform));
                    if (transform.HasParentWorld &&
                        std::memcmp(&parentWorld, &transform.ParentWorldMatrix, sizeof(parentWorld)) != 0)
                    {
                        const XMMATRIX previous = XMLoadFloat4x4(&transform.ParentWorldMatrix);
                        TransformMath::InheritParent(transform, previous, XMLoadFloat4x4(&parentWorld));
                    }
                    transform.ParentWorldMatrix = parentWorld;
                    transform.HasParentWorld = true;
                }
                else
                {
                    transform.Parent = None;
                    transform.HasParentWorld = false;
                }
                refreshWorld(*it, transform);
                completed.insert(*it);
            }
        }
    }
}
