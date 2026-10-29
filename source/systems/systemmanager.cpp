#include "pch.h"
#include "systemmanager.h"
#include "renderprofiler.h"
#include "graphicsdevice.h"
#include <typeinfo>
#include "framepipeline.h"
#include "componentmanager.h"
#include "world.h"

#include "inputsystem.h"
#include "transformsystem.h"
#include "movementsystem.h"
#include "timelinesystem.h"
#include "cameracontrolsystem.h"
#include "postprocessinputsystem.h"
#include "animationsystem.h"
#include "lookatsystem.h"
#include "physicssystem.h"
#include "projectmanager.h"
#include "materialsystem.h"
#include "lightsystem.h"
#include "camerasystem.h"
#include "gridsystem.h"
#include "rendersystem.h"
#include "modelsystem.h"
#include "instancingsystem.h"
#include "debugsystem.h"
#include "proceduralsystem.h"

bool SystemManager::Init()
{

    auto addSystem = [](unique_ptr<SystemBase> system) {
        SystemBase* rawPtr = system.get();
        m_Systems.push_back(move(system));
        m_SystemMap[type_index(typeid(*rawPtr))] = rawPtr;
    };

    addSystem(make_unique<InputSystem>());
    addSystem(make_unique<MovementSystem>());
    addSystem(make_unique<CameraControlSystem>());
    addSystem(make_unique<PostProcessInputSystem>());
    addSystem(make_unique<TimeLineSystem>());
    addSystem(make_unique<AnimationSystem>());

    addSystem(make_unique<ProceduralSystem>());
    addSystem(make_unique<PhysicsSystem>());
    addSystem(make_unique<MaterialSystem>());
    addSystem(make_unique<LightSystem>());
    addSystem(make_unique<CameraSystem>());
    addSystem(make_unique<TransformSystem>());
    addSystem(make_unique<LookAtSystem>());

    addSystem(make_unique<GridSystem>());
    addSystem(make_unique<RenderSystem>());
    addSystem(make_unique<ModelSystem>());
    addSystem(make_unique<InstancingSystem>());
    addSystem(make_unique<DebugSystem>());

    // 再生中だけ動かすシステムの判定は、毎フレームRTTIで比較せず初期化時に1回だけ行う
    m_PlayOnly.clear();
    m_PlayOnly.reserve(m_Systems.size());
    for (auto& system : m_Systems)
    {
        const type_index systemType(typeid(*system));
        const bool playOnlySystem =
            systemType == type_index(typeid(InputSystem)) || systemType == type_index(typeid(MovementSystem)) ||
            systemType == type_index(typeid(CameraControlSystem)) ||
            systemType == type_index(typeid(PostProcessInputSystem)) ||
            systemType == type_index(typeid(TimeLineSystem)) || systemType == type_index(typeid(AnimationSystem));
        m_PlayOnly.push_back(playOnlySystem ? 1u : 0u);
    }

    for (auto& system : m_Systems)
    {
        system->Init();
    }
    return true;
}

void SystemManager::Uninit()
{
    for (auto& system : m_Systems)
    {
        system->Uninit();
    }
    m_Systems.clear();
    m_SystemMap.clear();
    m_PlayOnly.clear();
}

void SystemManager::UpdateSystem()
{
    ComponentManager::ForEachComponent<TransformComponent>([](EntityID, TransformComponent& transform) {
        if (transform.HasPreviousWorld)
        {
            transform.PreviousWorldMatrix = transform.WorldMatrix;
        }
    });

    for (size_t index = 0; index < m_Systems.size(); ++index)
    {
        // 状態はシステムの途中で変わりうるので毎回読む（インライン比較なので軽い）
        if (m_PlayOnly[index] && !ProjectManager::IsSimulationRunning())
        {
            continue;
        }
        m_Systems[index]->Update();
    }

    ComponentManager::ForEachComponent<TransformComponent>([](EntityID, TransformComponent& transform) {
        if (!transform.HasPreviousWorld)
        {
            transform.PreviousWorldMatrix = transform.WorldMatrix;
            transform.HasPreviousWorld = true;
        }
    });
}

void SystemManager::DrawSystem(RenderPass renderPass, bool receivingPostProcessOnly)
{
    for (auto& system : m_Systems)
    {
        system->Draw(renderPass, receivingPostProcessOnly);
    }
}

void SystemManager::RenderFlow()
{
    FramePipeline::Execute();
}
