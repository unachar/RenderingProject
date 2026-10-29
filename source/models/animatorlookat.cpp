#include "pch.h"
#include "animator.h"
#include "componentmanager.h"
#include "modelmanager.h"

bool Animator::LookAt(uint32_t character, uint32_t target, const LookAtSettings& settings)
{
    if (character == target || !Registry::IsAlive(character) || !Registry::IsAlive(target) ||
        !ComponentManager::HasComponent<AnimationModelComponent>(character) ||
        !ComponentManager::HasComponent<TransformComponent>(character) ||
        !ComponentManager::HasComponent<TransformComponent>(target)) return false;
    auto& animation=ComponentManager::GetComponentUnchecked<AnimationModelComponent>(character);
    auto* model=ModelManager::GetAnimModel(animation.ModelId);
    if (!model || !model->HasLookAtBones(settings)) return false;
    auto& runtime=animation.LookAt;
    if (runtime.Owner!=character || runtime.PoseModelId!=animation.ModelId)
    {
        const int instance=ModelManager::CreateAnimModelInstance(animation.ModelId);
        if (instance<0) return false;
        animation.ModelId=instance;
        runtime.State={};
        runtime.Owner=character;
        runtime.PoseModelId=instance;
    }
    // A skinned resource and its GPU buffers cannot have independent per-instance poses.
    if (ComponentManager::HasComponent<InstancingComponent>(character))
        ComponentManager::GetComponentUnchecked<InstancingComponent>(character).UseInstancing=false;
    runtime.Settings=settings;
    runtime.Target=target;
    runtime.Active=true;
    runtime.Tracking=true;
    return true;
}

void Animator::ClearLookAt(uint32_t character)
{
    if (!Registry::IsAlive(character) || !ComponentManager::HasComponent<AnimationModelComponent>(character)) return;
    auto& runtime=ComponentManager::GetComponentUnchecked<AnimationModelComponent>(character).LookAt;
    runtime.Tracking=false;
    runtime.Target=g_kINVALID_ENTITY;
}
