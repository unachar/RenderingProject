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

    const auto modelPosition = XMFLOAT3(-5.0f, 10.0f, 10.0f);
    const auto modelScale = XMFLOAT3(0.01f, 0.01f, 0.01f);

    const auto modelPath = "asset\\model\\XBot.fbx";

    const char* vmdPath = "asset\\model\\animation\\kasou.vmd";
    constexpr bool isConvert = true;

    auto animations = vector<string>{"VMD"};
    string currentAnimation = "VMD";
    vector<string> animationPaths{vmdPath};
    const int modelID = ModelManager::LoadAnimModel(modelPath, isConvert);
    if (modelID < 0)
    {
        return;
    }
    ModelManager::LoadAnimation(modelID, vmdPath, "VMD");

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

        entity.Get<TransformComponent>().Position = {modelPosition.x, modelPosition.y, modelPosition.z};
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

    Game::RegisterEntity(this);
}
