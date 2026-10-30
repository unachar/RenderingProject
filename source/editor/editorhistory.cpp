#include "pch.h"
#include "materialmanager.h"
#include "imguimanager.h"
#include "editorwidgets.h"
#include "sun.h"
#include "world.h"
#include "scenehierarchy.h"
#include "graphicsdevice.h"
#include "temporalhistorycontext.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <DirectXCollision.h>
#include <ImGuizmo.h>

using namespace EditorWidgets;

namespace
{
    void PublishHistoryChange(bool lightChanged, bool lodChanged)
    {
        TemporalHistory::Context::NotifySceneChanged();
        if (lightChanged)
        {
            TemporalHistory::Context::NotifyLightingChanged();
        }
        if (lodChanged)
        {
            TemporalHistory::Context::NotifyLodChanged();
        }
    }

    template <typename T> void RestoreSnapshotComponent(EntityID entity, const T& component)
    {
        if (!ComponentManager::HasComponent<T>(entity))
        {
            ComponentManager::AddComponent(entity, ComponentTypeTraits<T>::value());
        }
        ComponentManager::GetComponentUnchecked<T>(entity) = component;
        GpuSceneChanges::Notify(entity);
    }

}

ImGuiManager::EntitySnapshot ImGuiManager::CaptureEntity(EntityID entity)
{
    EntitySnapshot snapshot{};
    snapshot.Entity = entity;
    if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity))
    {
        return snapshot;
    }
    snapshot.WasAlive = true;
    if (ComponentManager::HasComponent<NameComponent>(entity))
    {
        snapshot.HasName = true;
        snapshot.Name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
    }

    if (ComponentManager::HasComponent<TransformComponent>(entity))
    {
        snapshot.HasTransform = true;
        snapshot.Transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
    }

    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        snapshot.HasShader = true;
        snapshot.Shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
    }

    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
    {
        snapshot.HasStaticModel = true;
        snapshot.StaticModel = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
    }

    if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
    {
        snapshot.HasAnimationModel = true;
        snapshot.AnimationModel = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
    }

    if (ComponentManager::HasComponent<LightComponent>(entity))
    {
        snapshot.HasLight = true;
        snapshot.Light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
    }

    if (ComponentManager::HasComponent<SunComponent>(entity))
    {
        snapshot.HasSun = true;
        snapshot.Sun = ComponentManager::GetComponentUnchecked<SunComponent>(entity);
    }
    if (ComponentManager::HasComponent<ProceduralTerrainComponent>(entity))
    {
        snapshot.HasProceduralTerrain = true;
        snapshot.ProceduralTerrain = ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(entity);
    }
    if (ComponentManager::HasComponent<ProceduralMotionComponent>(entity))
    {
        snapshot.HasProceduralMotion = true;
        snapshot.ProceduralMotion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(entity);
    }

    if (ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        snapshot.HasMaterial = true;
        snapshot.Material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    }

    if (ComponentManager::HasComponent<AABBComponent>(entity))
    {
        snapshot.HasAabb = true;
        snapshot.Aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
    }

    if (ComponentManager::HasComponent<SpriteComponent>(entity))
    {
        snapshot.HasSprite = true;
        snapshot.Sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
    }

    if (ComponentManager::HasComponent<MeshComponent>(entity))
    {
        snapshot.HasMesh = true;
        snapshot.Mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
    }

    if (ComponentManager::HasComponent<CameraComponent>(entity))
    {
        snapshot.HasCamera = true;
        snapshot.Camera = ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
    }

    if (ComponentManager::HasComponent<PostProcessComponent>(entity))
    {
        snapshot.HasPostProcess = true;
        snapshot.PostProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(entity);
    }

    if (ComponentManager::HasComponent<InputComponent>(entity))
    {
        snapshot.HasInput = true;
        snapshot.Input = ComponentManager::GetComponentUnchecked<InputComponent>(entity);
    }

    if (ComponentManager::HasComponent<MoveComponent>(entity))
    {
        snapshot.HasMove = true;
        snapshot.Move = ComponentManager::GetComponentUnchecked<MoveComponent>(entity);
    }

    if (ComponentManager::HasComponent<PhysicsComponent>(entity))
    {
        snapshot.HasPhysics = true;
        snapshot.Physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
    }
    if (ComponentManager::HasComponent<TimelineComponent>(entity))
    {
        snapshot.HasTimeline = true;
        snapshot.Timeline = ComponentManager::GetComponentUnchecked<TimelineComponent>(entity);
    }

    if (ComponentManager::HasComponent<OBBComponent>(entity))
    {
        snapshot.HasObb = true;
        snapshot.Obb = ComponentManager::GetComponentUnchecked<OBBComponent>(entity);
    }

    if (ComponentManager::HasComponent<LODComponent>(entity))
    {
        snapshot.HasLod = true;
        snapshot.Lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
    }

    if (ComponentManager::HasComponent<DecalComponent>(entity))
    {
        snapshot.HasDecal = true;
        snapshot.Decal = ComponentManager::GetComponentUnchecked<DecalComponent>(entity);
    }

    if (ComponentManager::HasComponent<ReflectionProbeComponent>(entity))
    {
        snapshot.HasReflectionProbe = true;
        snapshot.ReflectionProbe = ComponentManager::GetComponentUnchecked<ReflectionProbeComponent>(entity);
    }

    return snapshot;
}

void ImGuiManager::ApplySnapshot(const EntitySnapshot& snapshot)
{
    if (!GraphicsDevice::WaitForGpuIdle())
    {
        return;
    }
    PublishHistoryChange(snapshot.HasLight, snapshot.HasLod);
    if (snapshot.Entity == g_kINVALID_ENTITY)
    {
        return;
    }
    if (!snapshot.WasAlive)
    {
        if (Registry::IsAlive(snapshot.Entity))
        {
            World::DestroyEntity(Entity(snapshot.Entity));
        }
        return;
    }

    if (!Registry::IsAlive(snapshot.Entity) && !Registry::RestoreEntity(snapshot.Entity))
    {
        return;
    }

    if (snapshot.HasName)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Name);
        World::RegisterName(snapshot.Entity, snapshot.Name.Name);
    }
    if (snapshot.HasTransform)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Transform);
        ComponentManager::GetComponentUnchecked<TransformComponent>(snapshot.Entity).IsDirty = true;
    }
    if (snapshot.HasShader)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Shader);
    }
    if (snapshot.HasStaticModel)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.StaticModel);
    }
    if (snapshot.HasAnimationModel)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.AnimationModel);
    }
    if (snapshot.HasLight)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Light);
        ApplyLightEntityToRuntime(snapshot.Entity);
    }
    if (snapshot.HasSun)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Sun);
        Sun::Sync(snapshot.Entity);
    }
    if (snapshot.HasMaterial)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Material);
    }
    if (snapshot.HasAabb)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Aabb);
    }
    if (snapshot.HasSprite)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Sprite);
    }
    if (snapshot.HasMesh)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Mesh);
    }
    if (snapshot.HasProceduralTerrain)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.ProceduralTerrain);
    }
    if (snapshot.HasProceduralMotion)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.ProceduralMotion);
    }
    if (!snapshot.HasProceduralMotion && ComponentManager::HasComponent<ProceduralMotionComponent>(snapshot.Entity))
    {
        ComponentManager::RemoveComponent(snapshot.Entity, ComponentType::PROCEDURAL_MOTION);
    }
    if (snapshot.HasCamera)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Camera);
    }
    if (snapshot.HasPostProcess)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.PostProcess);
    }
    if (snapshot.HasInput)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Input);
    }
    if (snapshot.HasMove)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Move);
    }
    if (snapshot.HasPhysics)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Physics);
    }
    else if (ComponentManager::HasComponent<PhysicsComponent>(snapshot.Entity))
    {
        ComponentManager::RemoveComponent(snapshot.Entity, ComponentType::PHYSICS);
    }
    if (snapshot.HasTimeline)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Timeline);
    }
    else if (ComponentManager::HasComponent<TimelineComponent>(snapshot.Entity))
    {
        ComponentManager::RemoveComponent(snapshot.Entity, ComponentType::TIMELINE);
    }
    if (snapshot.HasObb)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Obb);
    }
    if (snapshot.HasLod)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Lod);
    }
    else if (ComponentManager::HasComponent<LODComponent>(snapshot.Entity))
    {
        ComponentManager::RemoveComponent(snapshot.Entity, ComponentType::LOD);
    }
    if (snapshot.HasDecal)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.Decal);
    }
    else if (ComponentManager::HasComponent<DecalComponent>(snapshot.Entity))
    {
        ComponentManager::RemoveComponent(snapshot.Entity, ComponentType::DECAL);
    }
    if (snapshot.HasReflectionProbe)
    {
        RestoreSnapshotComponent(snapshot.Entity, snapshot.ReflectionProbe);
    }
    else if (ComponentManager::HasComponent<ReflectionProbeComponent>(snapshot.Entity))
    {
        ComponentManager::RemoveComponent(snapshot.Entity, ComponentType::REFLECTION_PROBE);
    }
}

void ImGuiManager::BeginUndoCapture(EntityID entity, const EntitySnapshot& before)
{
    if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity))
    {
        return;
    }
    // 明示的な編集操作では履歴を無効化する。シミュレーションによる移動は
    // このトランザクション経路を通らず、通常の再投影を継続する。
    PublishHistoryChange(before.HasLight, before.HasLod);
    GpuSceneChanges::Notify(entity);
    ++m_EditRevision;
    auto append = [&](const EntitySnapshot& snapshot) {
        for (const auto& existing : m_PendingUndo)
        {
            if (existing.Entity == snapshot.Entity)
            {
                return;
            }
        }
        m_PendingUndo.push_back(snapshot);
    };
    append(before);
    for (EntityID child :
         EntityHierarchy::WithDescendants({entity}, SceneHierarchy::Entities(), SceneHierarchy::Parent))
    {
        const bool alreadyCaptured =
            any_of(m_PendingUndo.begin(), m_PendingUndo.end(), [&](const EntitySnapshot& snapshot) {
                return snapshot.Entity == child;
            });
        if (!alreadyCaptured)
        {
            append(CaptureEntity(child));
        }
    }
    m_HasPendingUndo = true;
}

void ImGuiManager::FinalizeUndoCaptureIfIdle(bool force)
{
    if (!m_HasPendingUndo || (!force && (ImGui::IsAnyItemActive() || ImGuizmo::IsUsing())))
    {
        return;
    }
    PushUndoGroup(m_PendingUndo);
    m_HasPendingUndo = false;
    m_PendingUndo = {};
}

void ImGuiManager::PushUndoSnapshot(const EntitySnapshot& snapshot)
{
    PushUndoGroup({snapshot});
}

void ImGuiManager::PushUndoGroup(const vector<EntitySnapshot>& snapshots)
{
    if (snapshots.empty())
    {
        return;
    }
    for (const auto& snapshot : snapshots)
    {
        PublishHistoryChange(snapshot.HasLight, snapshot.HasLod);
    }
    ++m_EditRevision;
    m_UndoStack.push_back(snapshots);
    if (m_UndoStack.size() > 10)
    {
        m_UndoStack.erase(m_UndoStack.begin());
    }
    m_RedoStack.clear();
}

void ImGuiManager::RestoreHistoryGroup(const vector<EntitySnapshot>& snapshots)
{
    for (const auto& snapshot : snapshots)
    {
        if (snapshot.WasAlive && Registry::IsAlive(snapshot.Entity))
        {
            if (!snapshot.HasName && ComponentManager::HasComponent<NameComponent>(snapshot.Entity))
            {
                World::UnregisterName(snapshot.Entity);
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<NameComponent>::value());
            }
            if (!snapshot.HasTransform && ComponentManager::HasComponent<TransformComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<TransformComponent>::value());
            }
            if (!snapshot.HasShader && ComponentManager::HasComponent<ShaderComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<ShaderComponent>::value());
            }
            if (!snapshot.HasStaticModel && ComponentManager::HasComponent<StaticModelComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<StaticModelComponent>::value());
            }
            if (!snapshot.HasAnimationModel && ComponentManager::HasComponent<AnimationModelComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity,
                                                  ComponentTypeTraits<AnimationModelComponent>::value());
            }
            if (!snapshot.HasLight && ComponentManager::HasComponent<LightComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<LightComponent>::value());
            }
            if (!snapshot.HasSun && ComponentManager::HasComponent<SunComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<SunComponent>::value());
            }
            if (!snapshot.HasProceduralTerrain &&
                ComponentManager::HasComponent<ProceduralTerrainComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity,
                                                  ComponentTypeTraits<ProceduralTerrainComponent>::value());
            }
            if (!snapshot.HasProceduralMotion &&
                ComponentManager::HasComponent<ProceduralMotionComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity,
                                                  ComponentTypeTraits<ProceduralMotionComponent>::value());
            }
            if (!snapshot.HasMaterial && ComponentManager::HasComponent<MaterialComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<MaterialComponent>::value());
            }
            if (!snapshot.HasAabb && ComponentManager::HasComponent<AABBComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<AABBComponent>::value());
            }
            if (!snapshot.HasSprite && ComponentManager::HasComponent<SpriteComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<SpriteComponent>::value());
            }
            if (!snapshot.HasMesh && ComponentManager::HasComponent<MeshComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<MeshComponent>::value());
            }
            if (!snapshot.HasCamera && ComponentManager::HasComponent<CameraComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<CameraComponent>::value());
            }
            if (!snapshot.HasPostProcess && ComponentManager::HasComponent<PostProcessComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<PostProcessComponent>::value());
            }
            if (!snapshot.HasInput && ComponentManager::HasComponent<InputComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<InputComponent>::value());
            }
            if (!snapshot.HasMove && ComponentManager::HasComponent<MoveComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<MoveComponent>::value());
            }
            if (!snapshot.HasPhysics && ComponentManager::HasComponent<PhysicsComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<PhysicsComponent>::value());
            }
            if (!snapshot.HasTimeline && ComponentManager::HasComponent<TimelineComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<TimelineComponent>::value());
            }
            if (!snapshot.HasObb && ComponentManager::HasComponent<OBBComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<OBBComponent>::value());
            }
            if (!snapshot.HasLod && ComponentManager::HasComponent<LODComponent>(snapshot.Entity))
            {
                ComponentManager::RemoveComponent(snapshot.Entity, ComponentTypeTraits<LODComponent>::value());
            }
        }
        ApplySnapshot(snapshot);
    }
    // Restoring both parent and child must not apply the parent's delta a second time.
    for (EntityID entity : SceneHierarchy::Entities())
    {
        SceneHierarchy::ResetParentFrame(entity);
    }
    SceneHierarchy::Update();
    m_SelectedEntities.clear();
    for (const auto& snapshot : snapshots)
    {
        if (snapshot.WasAlive && Registry::IsAlive(snapshot.Entity))
        {
            m_SelectedEntities.push_back(snapshot.Entity);
        }
    }
    m_SelectedEntity = g_kINVALID_ENTITY;
    if (!m_SelectedEntities.empty())
    {
        m_SelectedEntity = m_SelectedEntities.front();
    }
    MaterialManager::NotifyAssignmentsChanged();
}

void ImGuiManager::Undo()
{
    FinalizeUndoCaptureIfIdle();
    if (m_UndoStack.empty())
    {
        return;
    }
    const auto snapshots = m_UndoStack.back();
    m_UndoStack.pop_back();
    vector<EntitySnapshot> inverse;
    for (const auto& snapshot : snapshots)
    {
        inverse.push_back(CaptureEntity(snapshot.Entity));
    }
    m_RedoStack.push_back(move(inverse));
    ++m_EditRevision;
    RestoreHistoryGroup(snapshots);
}

void ImGuiManager::Redo()
{
    FinalizeUndoCaptureIfIdle();
    if (m_RedoStack.empty())
    {
        return;
    }
    const auto snapshots = m_RedoStack.back();
    m_RedoStack.pop_back();
    vector<EntitySnapshot> inverse;
    for (const auto& snapshot : snapshots)
    {
        inverse.push_back(CaptureEntity(snapshot.Entity));
    }
    m_UndoStack.push_back(move(inverse));
    ++m_EditRevision;
    RestoreHistoryGroup(snapshots);
}
