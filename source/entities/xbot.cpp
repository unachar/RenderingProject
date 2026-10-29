#include "pch.h"
#include "xbot.h"
#include "game.h"
#include "world.h"
#include "animator.h"
#include "modelmanager.h"
#include "envflags.h"

void XBot::Create()
{
    const auto modelName = "XBot";

    const auto modelPosition = XMFLOAT3(-5.0f, 0.0f, 0.0f);
    const auto modelScale = XMFLOAT3(0.01f, 0.01f, 0.01f);

    const auto modelPath = "asset\\model\\XBot.fbx";
    const auto animPath1 = "asset\\model\\standup.fbx";
    const auto animPath2 = "asset\\model\\idle.fbx";
    constexpr bool isConvert = true;

    auto animations = vector<string>{"Standup", "Idle"};
    string currentAnimation = "Standup";
    vector<string> animationPaths{animPath1, animPath2};
    const int modelID = ModelManager::LoadAnimModel(modelPath, isConvert);
    if (modelID < 0)
    {
        return;
    }
    ModelManager::LoadAnimation(modelID, animPath1, "Standup");
    ModelManager::LoadAnimation(modelID, animPath2, "Idle");
    const char* vmdPath = "asset\\model\\animation\\kasou.vmd";
    if (ReadEnvironmentFlagOnce("DX12_VMD_HUMANOID_TEST") &&
        ModelManager::LoadAnimation(modelID, vmdPath, "VMD"))
    {
        animations.emplace_back("VMD");
        animationPaths.emplace_back(vmdPath);
        currentAnimation = "VMD";
    }

    for (int i = 0; i < 1; ++i)
    {
        auto entity = World::CreateEntity()
                          .Add<TransformComponent>()
                          .Add<NameComponent>()
                          .Add<MeshComponent>()
                          .Add<AABBComponent>()
                          .Add<MaterialComponent>()
                          .Add<InstancingComponent>()
                          .Add<LODComponent>()
                          .Add<AnimationModelComponent>();

        entity.SetName(modelName);

        entity.Get<TransformComponent>().Position = {modelPosition.x + (i % 10) * 2.0f, modelPosition.y,
                                                     modelPosition.z + (i / 10) * 2.0f};
        entity.Get<TransformComponent>().Scale = modelScale;
        entity.Get<MaterialComponent>().UseTexture = true;

        auto& anim = entity.Get<AnimationModelComponent>();
        anim.ModelId = modelID;
        anim.ModelPath = modelPath;
        anim.AnimationPaths = animationPaths;
        anim.Animations = animations;
        anim.CurrentAnimation = currentAnimation;

        Animator::Play(anim, currentAnimation);

        entity.Get<MaterialComponent>().ShaderClassMode = MaterialMode::Manual;
        entity.Get<MaterialComponent>().ShaderClass = ShaderClass::Metallic;

        entity.Get<InstancingComponent>().UseInstancing = true;
        entity.Get<InstancingComponent>().EnableFrustumCulling = true;
        entity.Get<LODComponent>().UseLOD = true;

        if (auto* model = ModelManager::GetAnimModel(modelID))
        {
            entity.Get<AABBComponent>().Center = model->GetAabbCenter();
            entity.Get<AABBComponent>().Extents = model->GetAabbExtents();
        }
    }

    Game::RegisterEntity(this);
}
