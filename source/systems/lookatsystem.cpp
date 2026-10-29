#include "pch.h"
#include "lookatsystem.h"
#include "animator.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "projectmanager.h"
#include "world.h"

void LookAtSystem::Update()
{
    if (!ProjectManager::IsSimulationRunning()) return;
    ComponentManager::ForEach<AnimationModelComponent,TransformComponent>(
        [](EntityID entity,AnimationModelComponent& animation,TransformComponent& transform) {
        auto& runtime=animation.LookAt;
        if (!runtime.Active) return;
        // Also isolate components copied by scripts/prefabs while a LookAt is registered.
        if (runtime.Owner!=entity || runtime.PoseModelId!=animation.ModelId)
        {
            if (!runtime.Tracking || !Animator::LookAt(entity,runtime.Target,runtime.Settings))
            {
                runtime.Active=false;
                runtime.Tracking=false;
                runtime.State={};
                return;
            }
        }
        auto* model=ModelManager::GetAnimModel(animation.ModelId);
        if (!model) { runtime.Active=false; return; }
        aiVector3D target;
        if (runtime.Tracking)
        {
            if (!Registry::IsAlive(runtime.Target) || !ComponentManager::HasComponent<TransformComponent>(runtime.Target))
            {
                runtime.Tracking=false;
                runtime.Target=g_kINVALID_ENTITY;
            }
            else
            {
                const auto& targetTransform=ComponentManager::GetComponentUnchecked<TransformComponent>(runtime.Target);
                const auto& offset=runtime.Settings.TargetOffset;
                const XMVECTOR position=XMVectorSet(targetTransform.Position.x+offset.x,
                    targetTransform.Position.y+offset.y,targetTransform.Position.z+offset.z,1);
                XMVECTOR determinant;
                const XMMATRIX inverse=XMMatrixInverse(&determinant,XMLoadFloat4x4(&transform.WorldMatrix));
                const float det=XMVectorGetX(determinant);
                if (!isfinite(det) || fabsf(det)<0.00000001f)
                {
                    // This frame has no usable world-to-model transform. Let the gaze relax.
                    const bool tracking=runtime.Tracking;
                    runtime.Tracking=false;
                    model->ApplyLookAtPose(runtime,{},World::GetDeltaTime());
                    runtime.Tracking=tracking;
                    return;
                }
                XMFLOAT3 local;
                XMStoreFloat3(&local,XMVector3TransformCoord(position,inverse));
                target={local.x,local.y,local.z};
            }
        }
        model->ApplyLookAtPose(runtime,target,World::GetDeltaTime());
        if (!runtime.Tracking && Gaze::AtRest(runtime.State)) runtime.Active=false;
    });
}
