#include "pch.h"
#include "lookatsystem.h"
#include "animator.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "projectmanager.h"
#include "world.h"
#include "camera.h"
#include "graphicsdevice.h"
#include "texturemanager.h"
#include "gpuscene/gpuscenechanges.h"

namespace
{
    float FaceCorrectionWeight(const FaceCorrectionSettings& settings, AnimationModelResource& model,
                               const TransformComponent& transform, const XMFLOAT3& camera)
    {
        XMVECTOR determinant;
        const auto inverse = XMMatrixInverse(&determinant, XMLoadFloat4x4(&transform.WorldMatrix));
        const float det = XMVectorGetX(determinant);
        if (!std::isfinite(det) || fabsf(det) < 1e-8f)
        {
            return 0.0f;
        }

        XMVECTOR direction = XMVector3TransformCoord(XMLoadFloat3(&camera), inverse);
        XMFLOAT4X4 head{};
        if (!settings.HeadBone.empty() && model.GetBoneGlobalTransform(settings.HeadBone, head))
        {
            direction -= XMLoadFloat4x4(&head).r[3];
        }
        else
        {
            const auto center = model.GetAabbCenter();
            direction -= XMLoadFloat3(&center);
        }
        direction = XMVector3TransformNormal(direction, XMMatrixRotationY(-XMConvertToRadians(settings.FrontYaw)));
        XMFLOAT3 local{};
        XMStoreFloat3(&local, direction);
        if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z))
        {
            return 0.0f;
        }
        const float yaw = XMConvertToDegrees(atan2f(local.x, -local.z));
        const float angle = settings.Direction == 3
                                ? XMConvertToDegrees(atan2f(-local.y, hypotf(local.x, local.z)))
                                : settings.Direction == 1 ? yaw : settings.Direction == 2 ? -yaw : fabsf(yaw);
        const float start = clamp(settings.StartAngle, 0.0f, 90.0f);
        const float end = max(clamp(settings.FullAngle, 0.0f, 90.0f), start + 0.01f);
        const float amount = clamp((angle - start) / (end - start), 0.0f, 1.0f);
        const float smoothAmount = amount * amount * (3.0f - 2.0f * amount);
        return powf(smoothAmount, clamp(settings.CurvePower, 0.25f, 4.0f)) *
               clamp(settings.Strength, 0.0f, 1.0f);
    }

    void UpdateFaceCorrections()
    {
        const auto cameraEntity = Camera::GetCameraEntity();
        const bool hasCamera = ComponentManager::HasComponent<TransformComponent>(cameraEntity);
        const XMFLOAT3 camera = hasCamera
                                   ? ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position
                                   : XMFLOAT3{};
        ComponentManager::ForEach<AnimationModelComponent, TransformComponent>(
            [&](EntityID entity, AnimationModelComponent& animation, TransformComponent& transform) {
                const auto& settings = animation.FaceCorrection;
                const bool enabled = settings.Enabled && settings.Morph >= 0 && hasCamera;
                bool ownsPose = animation.FaceCorrectionOwner == entity &&
                                animation.FaceCorrectionModel == animation.ModelId;
                if (!enabled && (!ownsPose || animation.AppliedFaceWeight == 0.0f))
                {
                    return;
                }
                auto* model = ModelManager::GetAnimModel(animation.ModelId);
                if (!model)
                {
                    return;
                }
                if (!ownsPose)
                {
                    if (static_cast<size_t>(settings.Morph) >= model->GetPreviewExpressionNames().size())
                    {
                        return;
                    }
                    TextureManager::BeginTextureLoading();
                    auto instance = make_unique<AnimationModelResource>();
                    const bool loaded = instance->LoadPoseInstance(*model, GraphicsDevice::GetDevice());
                    TextureManager::EndTextureLoading();
                    if (!loaded)
                    {
                        return;
                    }
                    animation.ModelId = ModelManager::RegisterAnimModelInstance(move(instance), animation.PoseOwner);
                    animation.FaceCorrectionOwner = entity;
                    animation.FaceCorrectionModel = animation.ModelId;
                    animation.AppliedFaceMorph = -1;
                    animation.AppliedFaceWeight = 0.0f;
                    // Keep gaze bound to the new independent pose when it already belongs to this entity.
                    if (animation.LookAt.Owner == entity)
                    {
                        animation.LookAt.PoseModelId = animation.ModelId;
                    }
                    if (ComponentManager::HasComponent<InstancingComponent>(entity))
                    {
                        ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).UseInstancing = false;
                    }
                    GpuSceneChanges::Notify(entity);
                    model = animation.PoseOwner.get();
                }

                const int morph = enabled ? settings.Morph : animation.AppliedFaceMorph;
                const float weight = enabled ? FaceCorrectionWeight(settings, *model, transform, camera) : 0.0f;
                if ((animation.AppliedFaceMorph == morph && fabsf(animation.AppliedFaceWeight - weight) < 1e-4f) ||
                    (animation.AppliedFaceWeight == 0.0f && weight == 0.0f))
                {
                    return;
                }
                if (!GraphicsDevice::WaitForGpuIdle())
                {
                    return;
                }
                if (animation.AppliedFaceMorph != morph && animation.AppliedFaceWeight != 0.0f)
                {
                    model->SetPreviewExpression(animation.AppliedFaceMorph, 0.0f);
                    animation.AppliedFaceWeight = 0.0f;
                }
                if (model->SetPreviewExpression(morph, weight))
                {
                    animation.AppliedFaceMorph = morph;
                    animation.AppliedFaceWeight = weight;
                }
            });
    }
}

void LookAtSystem::Update()
{
    if (!ProjectManager::IsSimulationRunning())
    {
        UpdateFaceCorrections();
        return;
    }
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
    UpdateFaceCorrections();
}
