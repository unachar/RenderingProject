#include "pch.h"
#include "moca.h"
#include "game.h"
#include "world.h"
#include "modelmanager.h"
#include "animator.h"
#include "envflags.h"

void Moca::Create()
{
    const auto modelName = "Moca";
    const auto modelPosition = XMFLOAT3(3.0f, 0.0f, 0.0f);
    const auto modelScale = XMFLOAT3(1.5f, 1.5f, 1.5f);
    const auto modelRotation = XMFLOAT3(XM_PIDIV2, 0.0f, 0.0f);
    const auto modelPath = "asset\\model\\moca.vrm";

    auto entity = World::CreateEntity()
                      .Add<NameComponent>()
                      .Add<TransformComponent>()
                      .Add<MeshComponent>()
                      .Add<MaterialComponent>();

    entity.SetName(modelName);
    if (ReadEnvironmentFlagOnce("DX12_VMD_HUMANOID_TEST"))
    {
        entity.Add<AnimationModelComponent>().Add<AABBComponent>();
        auto& animation = entity.Get<AnimationModelComponent>();
        animation.ModelPath = modelPath;
        animation.ModelId = ModelManager::LoadAnimModel(modelPath);
        const char* vmdPath = "asset\\model\\animation\\kasou.vmd";
        if (ModelManager::LoadAnimation(animation.ModelId, vmdPath, "VMD"))
        {
            animation.AnimationPaths = {vmdPath};
            animation.Animations = {"VMD"};
            Animator::Play(animation, "VMD");
        }
        else animation.IsPlaying = false;
        if (const auto* model = ModelManager::GetAnimModel(animation.ModelId))
        {
            entity.Get<AABBComponent>().Center = model->GetAabbCenter();
            entity.Get<AABBComponent>().Extents = model->GetAabbExtents();
        }
    }
    else
    {
        entity.Add<StaticModelComponent>();
        auto& staticModel = entity.Get<StaticModelComponent>();
        staticModel.ModelPath = modelPath;
        staticModel.ModelId = ModelManager::LoadStaticModel(modelPath);
    }
    entity.Get<TransformComponent>().Position = modelPosition;
    entity.Get<TransformComponent>().Scale = modelScale;
    entity.Get<TransformComponent>().Rotation = modelRotation;

    auto& material = entity.Get<MaterialComponent>();
    material.UseTexture = true;
    material.Roughness = 1.0f;
    material.ShadowThreshold = 1.0f;
    material.ShadowSoftness = 0.5f;
    material.ShadowStrength = 0.066f;
    material.ShaderClassMode = MaterialMode::Manual;
    material.ShaderClass = ShaderClass::Lit;

    Game::RegisterEntity(this);
}
