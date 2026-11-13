#include "pch.h"
#include "animationsystem.h"
#include "animator.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "world.h"
#include "camera.h"
#include "instancingsystem.h"
#include "graphicsdevice.h"
#include <unordered_set>

void AnimationSystem::Update()
{
    // Remove last frame's procedural corrections before animation cache checks or physics.
    // This also restores paused/no-clip and animation-LOD-skipped characters.
    ComponentManager::ForEachComponent<AnimationModelComponent>([](EntityID entity, AnimationModelComponent& animation) {
        auto& gaze = animation.LookAt;
        // Isolate a copied component before either entity evaluates a shared pose or builds physics bodies.
        if (gaze.Active && gaze.Tracking && (gaze.Owner != entity || gaze.PoseModelId != animation.ModelId))
        {
            if (!Animator::LookAt(entity, gaze.Target, gaze.Settings))
            {
                gaze.Active = gaze.Tracking = false;
                gaze.State = {};
            }
        }
        if (gaze.Owner != entity || gaze.PoseModelId != animation.ModelId) return;
        if (auto* model = ModelManager::GetAnimModel(animation.ModelId)) model->RestoreLookAtPose();
    });
    auto animEntities = World::GetView<AnimationModelComponent>();
    unordered_set<int> updatedInstancedModels;

    static uint64_t s_AnimLodFrame = 0;
    ++s_AnimLodFrame;

    // カメラ位置を1回だけ取得 (距離LOD用)。
    bool hasCameraPosition = false;
    XMFLOAT3 cameraPosition{};
    const EntityID cameraEntity = Camera::GetCameraEntity();
    XMMATRIX cameraView = XMMatrixIdentity();
    XMMATRIX cameraProjection = XMMatrixIdentity();
    if (ComponentManager::HasComponent<TransformComponent>(cameraEntity))
    {
        cameraPosition = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
        Camera::GetCameraMatrices(cameraEntity, cameraView, cameraProjection);
        hasCameraPosition = true;
    }

    for (EntityID i : animEntities)
    {
        auto& animationComponent = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(i);
        if (animationComponent.ModelId < 0)
        {
            continue;
        }

        AnimationModelResource* model = ModelManager::GetAnimModel(animationComponent.ModelId);
        if (!model)
        {
            continue;
        }

        if (animationComponent.CurrentAnimation.empty() && animationComponent.ActiveAnimationLayers.empty() &&
            !animationComponent.Animations.empty())
        {
            Animator::Play(animationComponent, animationComponent.Animations[0]);
        }
        if (animationComponent.CurrentAnimation.empty() && animationComponent.ActiveAnimationLayers.empty())
        {
            continue;
        }

        Animator::Update(animationComponent, World::GetDeltaTime());

        // 画面上で小さいモデルはポーズ評価の頻度を落とす。再生時刻は毎フレーム進める。
        if (hasCameraPosition && ComponentManager::HasComponent<TransformComponent>(i))
        {
            if (!InstancingSystem::IsEntityVisible(i))
            {
                continue;
            }

            float lodFarDistance = 35.0f;
            if (ComponentManager::HasComponent<LODComponent>(i))
            {
                const auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(i);
                if (lod.UseLOD)
                {
                    lodFarDistance = max(lod.Lod2Distance, lod.Lod1Distance);
                }
            }
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(i);
            const float dx = transform.Position.x - cameraPosition.x;
            const float dy = transform.Position.y - cameraPosition.y;
            const float dz = transform.Position.z - cameraPosition.z;
            const float distanceSq = dx * dx + dy * dy + dz * dz;
            const auto bounds = model->GetAabbExtents();
            const auto center = model->GetAabbCenter();
            const XMMATRIX world = XMLoadFloat4x4(&transform.WorldMatrix);
            const XMMATRIX modelView = world * cameraView;
            const XMVECTOR viewCenter = XMVector3TransformCoord(XMLoadFloat3(&center), modelView);
            const XMVECTOR clipCenter = XMVector4Transform(viewCenter, cameraProjection);
            const float viewRadius = fabsf(XMVectorGetY(modelView.r[0])) * bounds.x +
                                     fabsf(XMVectorGetY(modelView.r[1])) * bounds.y +
                                     fabsf(XMVectorGetY(modelView.r[2])) * bounds.z;
            const float clipW = XMVectorGetW(clipCenter);
            const float screenHeight = clipW > 0.0f
                ? viewRadius * fabsf(XMVectorGetY(cameraProjection.r[1])) * GraphicsDevice::GetHeight() / clipW
                : 0.0f;
            unsigned interval = distanceSq > lodFarDistance * lodFarDistance ? 2u : 1u;
            if (screenHeight > 0.0f)
            {
                interval = screenHeight < 24.0f ? 4u : screenHeight < 64.0f ? 2u : 1u;
            }
            if ((static_cast<uint64_t>(i) + s_AnimLodFrame) % interval != 0)
            {
                continue;
            }
        }

        const bool useSharedInstancedPose =
            ComponentManager::HasComponent<InstancingComponent>(i) &&
            ComponentManager::GetComponentUnchecked<InstancingComponent>(i).UseInstancing;
        if (useSharedInstancedPose && !updatedInstancedModels.insert(animationComponent.ModelId).second)
        {
            continue;
        }

        if (animationComponent.ActiveAnimationLayers.size() > 1)
        {
            model->UpdateBoneMatrices(animationComponent.ActiveAnimationLayers);
            continue;
        }

        const string* nextAnimationSelection = nullptr;
        if (animationComponent.NextAnimation.empty())
        {
            nextAnimationSelection = &(animationComponent.CurrentAnimation);
        }
        else
        {
            nextAnimationSelection = &(animationComponent.NextAnimation);
        }
        const string& nextAnimation = *nextAnimationSelection;
        float nextTime;
        if (animationComponent.NextAnimation.empty())
        {
            nextTime = animationComponent.CurrentTime;
        }
        else
        {
            nextTime = animationComponent.NextTime;
        }

        model->UpdateBoneMatrices(animationComponent.CurrentAnimation.c_str(), animationComponent.CurrentTime,
                                  nextAnimation.c_str(), nextTime, animationComponent.BlendRate);
    }

    // Camera playback is independent of model visibility and animation LOD.
    ComponentManager::ForEach<CameraComponent, TransformComponent>(
        [](EntityID, CameraComponent& camera, TransformComponent& transform) {
            Animator::Update(camera, transform, World::GetDeltaTime());
        });
}
