#include "pch.h"
#include "proceduralscene.h"
#include "proceduralworkgraph.h"
#include "primitivegeometry.h"
#include "graphicsdevice.h"
#include "projectmanager.h"
#include "world.h"
#include "vertexdeformation.h"
#include <fstream>
#include <cmath>

namespace
{
    bool g_LiveUpdate = true;

}

bool ProceduralScene::GetLiveUpdate()
{
    return g_LiveUpdate;
}

void ProceduralScene::SetLiveUpdate(bool enabled)
{
    g_LiveUpdate = enabled;
}

EntityID ProceduralScene::GenerateTerrain(const Procedural::TerrainSettings& settings, EntityID target)
{
    const bool replace =
        Registry::IsAlive(target) && ComponentManager::HasComponent<ProceduralTerrainComponent>(target);
    if (replace && !GraphicsDevice::WaitForGpuIdle())
    {
        return g_kINVALID_ENTITY;
    }
    vector<Vertex> vertices;
    try
    {
        vertices = ProceduralWorkGraph::BuildTerrainVertices(settings);
    }
    catch (const std::exception& error)
    {
        Debug::Log("Procedural terrain generation failed: %s\n", error.what());
        return g_kINVALID_ENTITY;
    }
    Entity entity(target);
    if (!replace)
    {
        entity = World::CreateEntity()
                     .Add<NameComponent>()
                     .Add<TransformComponent>()
                     .Add<MeshComponent>()
                     .Add<MaterialComponent>()
                     .Add<ProceduralTerrainComponent>();
        if (!entity.IsValid())
        {
            return g_kINVALID_ENTITY;
        }
        entity.SetName("Procedural Terrain " + to_string(entity.GetID()));
        XMStoreFloat4x4(&entity.Get<TransformComponent>().WorldMatrix, XMMatrixIdentity());
        auto& material = entity.Get<MaterialComponent>();
        material.ShaderClassMode = MaterialMode::Manual;
        material.ShaderClass = ShaderClass::PBR;
        material.UseTexture = false;
    }
    if (!PrimitiveGeometry::UploadVertices(entity.GetID(), vertices))
    {
        if (!replace)
        {
            World::DestroyEntity(entity);
        }
        return g_kINVALID_ENTITY;
    }
    entity.Get<ProceduralTerrainComponent>().Settings = settings;
    entity.Get<TransformComponent>().IsDirty = true;
    return entity.GetID();
}

void ProceduralScene::UpdateMotion(float deltaTime)
{
    if (ProjectManager::IsPaused())
    {
        return;
    }
    std::vector<EntityID> entities;
    std::vector<ProceduralWorkGraph::MotionInput> samples;
    for (EntityID entity : World::GetView<ProceduralMotionComponent, TransformComponent>())
    {
        auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(entity);
        if (ComponentManager::HasComponent<PhysicsComponent>(entity))
        {
            const auto& physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
            if (physics.UsePhysics && physics.BodyType != PhysicsBodyType::Kinematic)
            {
                continue;
            }
        }
        if (!motion.Enabled || (!ProjectManager::IsSimulationRunning() && !motion.PreviewInEditor))
        {
            continue;
        }
        motion.Time += clamp(static_cast<double>(deltaTime), 0.0, 0.1);
        if (motion.Target == Procedural::MotionTarget::MeshVertices)
        {
            continue;
        }
        entities.push_back(entity);
        samples.push_back({motion.Time, motion.Speed, static_cast<uint32_t>(motion.Method),
                           {motion.Amplitude.x, motion.Amplitude.y, motion.Amplitude.z}, motion.Seed});
    }
    // Physics and editor consumers require this frame's authoritative positions.
    // Evaluate the entire batch on the GPU, then retire one fence before applying it.
    const auto offsets = ProceduralWorkGraph::SampleMotionBatch(samples);
    for (size_t index = 0; index < entities.size(); ++index)
    {
        const EntityID entity = entities[index];
        const auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(entity);
        const auto& offset = offsets[index];
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        transform.Position = {motion.Origin.x + offset.X, motion.Origin.y + offset.Y, motion.Origin.z + offset.Z};
        transform.IsDirty = true;
    }
}

void ProceduralScene::ResetMotion(EntityID entity)
{
    if (!Registry::IsAlive(entity) || !ComponentManager::HasComponent<ProceduralMotionComponent>(entity) ||
        !ComponentManager::HasComponent<TransformComponent>(entity))
    {
        return;
    }
    auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(entity);
    auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
    motion.Time = 0.0;
    VertexDeformation::ResetHistory(entity);
    transform.Position = motion.Origin;
    transform.IsDirty = true;
}

bool ProceduralScene::SavePreset(const char* path, const Procedural::TerrainSettings& settings)
{
    ofstream file(path);
    return Procedural::WriteTerrainPreset(file, settings);
}

bool ProceduralScene::LoadPreset(const char* path, Procedural::TerrainSettings& settings)
{
    ifstream file(path);
    return Procedural::ReadTerrainPreset(file, settings);
}
