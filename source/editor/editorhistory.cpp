#include "pch.h"
#include "imguimanager.h"
#include "editorwidgets.h"
#include "sun.h"
#include "world.h"
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
	template<typename T>
	void RestoreSnapshotComponent(EntityID entity, const T& component)
	{
		if (!ComponentManager::HasComponent<T>(entity))
		{
			ComponentManager::AddComponent(entity, ComponentTypeTraits<T>::value());
		}
		ComponentManager::GetComponentUnchecked<T>(entity) = component;
	}

}

ImGuiManager::EntitySnapshot ImGuiManager::CaptureEntity(EntityID entity)
{
	EntitySnapshot snapshot{};
	snapshot.Entity = entity;
	if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity)) return snapshot;
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
	{	snapshot.HasShader = true;
		snapshot.Shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
	}

	if (ComponentManager::HasComponent<StaticModelComponent>(entity))
	{	snapshot.HasStaticModel = true;
		snapshot.StaticModel = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
	}

	if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
	{	snapshot.HasAnimationModel = true;
		snapshot.AnimationModel = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
	}

	if (ComponentManager::HasComponent<LightComponent>(entity))
	{	snapshot.HasLight = true;
		snapshot.Light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
	}

	if (ComponentManager::HasComponent<SunComponent>(entity))
	{
		snapshot.HasSun = true;
		snapshot.Sun = ComponentManager::GetComponentUnchecked<SunComponent>(entity);
	}

	if (ComponentManager::HasComponent<MaterialComponent>(entity))
	{	snapshot.HasMaterial = true;
		snapshot.Material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
	}

	if (ComponentManager::HasComponent<AABBComponent>(entity))
	{	snapshot.HasAabb = true;
		snapshot.Aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
	}

	if (ComponentManager::HasComponent<SpriteComponent>(entity))
	{	snapshot.HasSprite = true;
		snapshot.Sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
	}

	if (ComponentManager::HasComponent<MeshComponent>(entity))
	{	snapshot.HasMesh = true;
		snapshot.Mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
	}

	if (ComponentManager::HasComponent<CameraComponent>(entity))
	{	snapshot.HasCamera = true;
		snapshot.Camera = ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
	}

	if (ComponentManager::HasComponent<PostProcessComponent>(entity))
	{	snapshot.HasPostProcess = true;
		snapshot.PostProcess = ComponentManager::GetComponentUnchecked<PostProcessComponent>(entity);
	}

	if (ComponentManager::HasComponent<InputComponent>(entity))
	{	snapshot.HasInput = true;
		snapshot.Input = ComponentManager::GetComponentUnchecked<InputComponent>(entity);
	}

	if (ComponentManager::HasComponent<MoveComponent>(entity))
	{	snapshot.HasMove = true;
		snapshot.Move = ComponentManager::GetComponentUnchecked<MoveComponent>(entity);
	}

	if (ComponentManager::HasComponent<PhysicsComponent>(entity))
	{	snapshot.HasPhysics = true;
		snapshot.Physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
	}
	if (ComponentManager::HasComponent<TimelineComponent>(entity))
	{
		snapshot.HasTimeline = true;
		snapshot.Timeline = ComponentManager::GetComponentUnchecked<TimelineComponent>(entity);
	}

	if (ComponentManager::HasComponent<OBBComponent>(entity))
	{	snapshot.HasObb = true;
		snapshot.Obb = ComponentManager::GetComponentUnchecked<OBBComponent>(entity);
	}

	if (ComponentManager::HasComponent<LODComponent>(entity))
	{
		snapshot.HasLod = true;
		snapshot.Lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
	}

	return snapshot;
}

void ImGuiManager::ApplySnapshot(const EntitySnapshot& snapshot)
{
	if (snapshot.Entity == g_kINVALID_ENTITY) return;
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

	if (snapshot.HasName) { RestoreSnapshotComponent(snapshot.Entity, snapshot.Name); World::RegisterName(snapshot.Entity, snapshot.Name.Name); }
	if (snapshot.HasTransform) { RestoreSnapshotComponent(snapshot.Entity, snapshot.Transform); ComponentManager::GetComponentUnchecked<TransformComponent>(snapshot.Entity).IsDirty = true; }
	if (snapshot.HasShader) RestoreSnapshotComponent(snapshot.Entity, snapshot.Shader);
	if (snapshot.HasStaticModel) RestoreSnapshotComponent(snapshot.Entity, snapshot.StaticModel);
	if (snapshot.HasAnimationModel) RestoreSnapshotComponent(snapshot.Entity, snapshot.AnimationModel);
	if (snapshot.HasLight) { RestoreSnapshotComponent(snapshot.Entity, snapshot.Light); ApplyLightEntityToRuntime(snapshot.Entity); }
	if (snapshot.HasSun) { RestoreSnapshotComponent(snapshot.Entity, snapshot.Sun); Sun::Sync(snapshot.Entity); }
	if (snapshot.HasMaterial) RestoreSnapshotComponent(snapshot.Entity, snapshot.Material);
	if (snapshot.HasAabb) RestoreSnapshotComponent(snapshot.Entity, snapshot.Aabb);
	if (snapshot.HasSprite) RestoreSnapshotComponent(snapshot.Entity, snapshot.Sprite);
	if (snapshot.HasMesh) RestoreSnapshotComponent(snapshot.Entity, snapshot.Mesh);
	if (snapshot.HasCamera) RestoreSnapshotComponent(snapshot.Entity, snapshot.Camera);
	if (snapshot.HasPostProcess) RestoreSnapshotComponent(snapshot.Entity, snapshot.PostProcess);
	if (snapshot.HasInput) RestoreSnapshotComponent(snapshot.Entity, snapshot.Input);
	if (snapshot.HasMove) RestoreSnapshotComponent(snapshot.Entity, snapshot.Move);
	if (snapshot.HasPhysics) RestoreSnapshotComponent(snapshot.Entity, snapshot.Physics);
	else if (ComponentManager::HasComponent<PhysicsComponent>(snapshot.Entity))
		ComponentManager::RemoveComponent(snapshot.Entity, ComponentType::PHYSICS);
	if (snapshot.HasTimeline) RestoreSnapshotComponent(snapshot.Entity, snapshot.Timeline);
	else if (ComponentManager::HasComponent<TimelineComponent>(snapshot.Entity))
		ComponentManager::RemoveComponent(snapshot.Entity, ComponentType::TIMELINE);
	if (snapshot.HasObb) RestoreSnapshotComponent(snapshot.Entity, snapshot.Obb);
	if (snapshot.HasLod)
	{
		RestoreSnapshotComponent(snapshot.Entity, snapshot.Lod);
	}
	else if (ComponentManager::HasComponent<LODComponent>(snapshot.Entity))
	{
		ComponentManager::RemoveComponent(snapshot.Entity, ComponentType::LOD);
	}
}

void ImGuiManager::BeginUndoCapture(EntityID entity, const EntitySnapshot& before)
{
	if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity)) return;
	if (!m_HasPendingUndo) { m_PendingUndo = before; m_HasPendingUndo = true; }
}

void ImGuiManager::FinalizeUndoCaptureIfIdle()
{
	if (!m_HasPendingUndo || ImGui::IsAnyItemActive() || ImGuizmo::IsUsing()) return;
	PushUndoSnapshot(m_PendingUndo);
	m_HasPendingUndo = false;
	m_PendingUndo = {};
}

void ImGuiManager::PushUndoSnapshot(const EntitySnapshot& snapshot)
{
	if (snapshot.Entity == g_kINVALID_ENTITY) return;
	m_UndoStack.push_back(snapshot);
	if (m_UndoStack.size() > 10) m_UndoStack.erase(m_UndoStack.begin());
	m_RedoStack.clear();
}

void ImGuiManager::Undo()
{
	FinalizeUndoCaptureIfIdle();
	if (m_UndoStack.empty()) return;
	EntitySnapshot snapshot = m_UndoStack.back();
	m_UndoStack.pop_back();
	m_RedoStack.push_back(CaptureEntity(snapshot.Entity));
	if (m_RedoStack.size() > 10) m_RedoStack.erase(m_RedoStack.begin());
	ApplySnapshot(snapshot);
	if (snapshot.WasAlive)
	{
		m_SelectedEntity = snapshot.Entity;
	}
	else
	{
		m_SelectedEntity = g_kINVALID_ENTITY;
	}
}

void ImGuiManager::Redo()
{
	if (m_RedoStack.empty()) return;
	EntitySnapshot snapshot = m_RedoStack.back();
	m_RedoStack.pop_back();
	m_UndoStack.push_back(CaptureEntity(snapshot.Entity));
	if (m_UndoStack.size() > 10) m_UndoStack.erase(m_UndoStack.begin());
	ApplySnapshot(snapshot);
	if (snapshot.WasAlive)
	{
		m_SelectedEntity = snapshot.Entity;
	}
	else
	{
		m_SelectedEntity = g_kINVALID_ENTITY;
	}
}
