#include "pch.h"
#include "proceduralscene.h"
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

std::vector<Vertex> BuildTerrainVertices(const Procedural::TerrainSettings& settings)
{
    const int resolution = std::clamp(settings.Resolution, 4, 256);
    const int side = resolution + 1;
    const float size = std::clamp(settings.Size, 1.0f, 500.0f);
    const float spacing = size / resolution;
    const auto heights = Procedural::BuildHeightMap(settings);
    std::vector<Vertex> lattice(heights.size());
    for (int row = 0; row < side; ++row)
    {
        for (int column = 0; column < side; ++column)
        {
            const int left = (std::max)(column - 1, 0);
            const int right = (std::min)(column + 1, resolution);
            const int lower = (std::max)(row - 1, 0);
            const int upper = (std::min)(row + 1, resolution);
            const float slopeX = (heights[row * side + right] - heights[row * side + left]) / ((right - left) * spacing);
            const float slopeZ = (heights[upper * side + column] - heights[lower * side + column]) / ((upper - lower) * spacing);
            Vertex& vertex = lattice[row * side + column];
            vertex.Pos = { column * spacing - size * 0.5f, heights[row * side + column], row * spacing - size * 0.5f };
            XMStoreFloat3(&vertex.Normal, XMVector3Normalize(XMVectorSet(-slopeX, 1.0f, -slopeZ, 0.0f)));
            vertex.Tex = { static_cast<float>(column) / resolution, static_cast<float>(row) / resolution };
            const float rock = std::clamp((1.0f - vertex.Normal.y) * 3.0f, 0.0f, 1.0f);
            vertex.Color = { 0.16f + rock * 0.32f, 0.36f + rock * 0.10f, 0.11f + rock * 0.31f, 1.0f };
        }
    }
    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(resolution) * resolution * 6);
    for (int row = 0; row < resolution; ++row)
    {
        for (int column = 0; column < resolution; ++column)
        {
            const int first = row * side + column;
            // Clockwise in the renderer's left-handed coordinate system.
            vertices.push_back(lattice[first]);
            vertices.push_back(lattice[first + side]);
            vertices.push_back(lattice[first + 1]);
            vertices.push_back(lattice[first + 1]);
            vertices.push_back(lattice[first + side]);
            vertices.push_back(lattice[first + side + 1]);
        }
    }
    return vertices;
}
}

bool ProceduralScene::GetLiveUpdate() { return g_LiveUpdate; }
void ProceduralScene::SetLiveUpdate(bool enabled) { g_LiveUpdate = enabled; }

EntityID ProceduralScene::GenerateTerrain(const Procedural::TerrainSettings& settings, EntityID target)
{
    const bool replace = Registry::IsAlive(target) &&
        ComponentManager::HasComponent<ProceduralTerrainComponent>(target);
    if (replace && !GraphicsDevice::WaitForGpuIdle())
    {
        return g_kINVALID_ENTITY;
    }
    Entity entity(target);
    if (!replace)
    {
        entity = World::CreateEntity().Add<NameComponent>().Add<TransformComponent>()
            .Add<MeshComponent>().Add<MaterialComponent>().Add<ProceduralTerrainComponent>();
        if (!entity.IsValid())
        {
            return g_kINVALID_ENTITY;
        }
        entity.SetName("Procedural Terrain " + std::to_string(entity.GetID()));
		XMStoreFloat4x4(&entity.Get<TransformComponent>().WorldMatrix, XMMatrixIdentity());
        auto& material = entity.Get<MaterialComponent>();
        material.ShaderClassMode = MaterialMode::Manual;
        material.ShaderClass = ShaderClass::PBR;
        material.UseTexture = false;
    }
    const auto vertices = BuildTerrainVertices(settings);
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
        motion.Time += std::clamp(static_cast<double>(deltaTime), 0.0, 0.1);
        if (motion.Target == Procedural::MotionTarget::MeshVertices)
        {
            continue;
        }
        const auto offset = Procedural::SampleMotion(motion.Method, motion.Time, motion.Speed,
            { motion.Amplitude.x, motion.Amplitude.y, motion.Amplitude.z }, motion.Seed);
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        transform.Position = { motion.Origin.x + offset.X, motion.Origin.y + offset.Y, motion.Origin.z + offset.Z };
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
    std::ofstream file(path);
    return Procedural::WriteTerrainPreset(file, settings);
}

bool ProceduralScene::LoadPreset(const char* path, Procedural::TerrainSettings& settings)
{
    std::ifstream file(path);
    return Procedural::ReadTerrainPreset(file, settings);
}
