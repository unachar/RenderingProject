#include "pch.h"
#include "imguimanager.h"
#include "editorwidgets.h"
#include "camera.h"
#include "graphicsdevice.h"
#include "light.h"
#include "rendertargets.h"
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

void ImGuiManager::DrawSceneViewWindow()
{
	m_IsSceneViewHovered = false;
	ImGui::SetNextWindowSize(ImVec2(860.0f, 520.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("シーンビュー"))
	{
		ImGui::End();
		return;
	}

	const char* toolLabels[] = { "移動 Q", "回転 W", "スケール E" };
	const float toolbarWidth = ImGui::GetContentRegionAvail().x;
	const float toolSpacing = ImGui::GetStyle().ItemSpacing.x;
	float rowWidth = 0.0f;
	for (int tool = 0; tool < 3; ++tool)
	{
		const float toolWidth = ImGui::CalcTextSize(toolLabels[tool]).x + 16.0f;
		if (tool > 0 && rowWidth + toolSpacing + toolWidth <= toolbarWidth)
		{
			ImGui::SameLine();
			rowWidth += toolSpacing;
		}
		else rowWidth = 0.0f;
		if (ImGui::Selectable(toolLabels[tool], m_GizmoOperation == tool, 0,
			ImVec2(toolWidth, ImGui::GetFrameHeight())))
		{
			m_GizmoOperation = tool;
		}
		rowWidth += toolWidth;
	}
	const float lightToggleWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("ライト可視化").x;
	if (rowWidth + toolSpacing + lightToggleWidth <= toolbarWidth) ImGui::SameLine();
	ImGui::Checkbox("ライト可視化", &m_ShowLightDebug);
	ImGui::Separator();
	const bool sceneFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (sceneFocused && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
		!ImGuizmo::IsUsing() && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		DeleteSelectedEntity();
	}
	ImVec2 available = ImGui::GetContentRegionAvail();
	available.y -= ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	const float sceneAspect = GraphicsDevice::GetSceneAspectRatio();
	if (available.x <= 1.0f || available.y <= 1.0f)
	{
		ImGui::End();
		return;
	}

	ImVec2 imageSize = available;
	if (sceneAspect > 0.001f)
	{
		const float availableAspect = available.x / available.y;
		if (availableAspect > sceneAspect)
		{
			imageSize.x = available.y * sceneAspect;
		}
		else
		{
			imageSize.y = available.x / sceneAspect;
		}
	}

	const ImVec2 imageCursor = ImGui::GetCursorPos();
	ImGui::SetCursorPos(ImVec2(imageCursor.x + (available.x - imageSize.x) * 0.5f,
		imageCursor.y + (available.y - imageSize.y) * 0.5f));
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	m_SceneViewPos = cursor;
	m_SceneViewSize = imageSize;

	D3D12_GPU_DESCRIPTOR_HANDLE handle = RenderTargets::GetEditorSceneSrvHandle();
	ImGui::Image((ImTextureData*)handle.ptr, imageSize);
	m_IsSceneViewHovered = ImGui::IsItemHovered();
	DrawTransformGizmo();
	if (m_IsSceneViewHovered && fabsf(ImGui::GetIO().MouseWheel) > 0.001f)
	{
		EntityID cameraEntity = Camera::GetCameraEntity();
		if (cameraEntity != g_kINVALID_ENTITY &&
			Registry::IsAlive(cameraEntity) &&
			ComponentManager::HasComponent<CameraComponent>(cameraEntity) &&
			ComponentManager::HasComponent<TransformComponent>(cameraEntity))
		{
			auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);
			auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
			XMVECTOR eye = XMLoadFloat3(&transform.Position);
			XMVECTOR target = XMLoadFloat3(&camera.Target);
			XMVECTOR forward = XMVectorSubtract(target, eye);
			if (XMVectorGetX(XMVector3LengthSq(forward)) <= 0.000001f)
			{
				forward = XMVector3TransformNormal(
					XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
					XMMatrixRotationRollPitchYaw(transform.Rotation.x, transform.Rotation.y, transform.Rotation.z));
			}
			forward = XMVector3Normalize(forward);
			const float zoomStep = 1.15f;
			const float wheel = ImGui::GetIO().MouseWheel;
			XMVECTOR delta = XMVectorScale(forward, wheel * zoomStep);
			eye = XMVectorAdd(eye, delta);
			target = XMVectorAdd(target, delta);
			XMStoreFloat3(&transform.Position, eye);
			XMStoreFloat3(&camera.Target, target);
			camera.LockOnTarget = g_kINVALID_ENTITY;
		transform.IsDirty = true;
		}
	}

	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
		{
			const char* path = static_cast<const char*>(payload->Data);
			if (path)
			{
				ImVec2 sceneMouse = ImGui::GetIO().MousePos;
				sceneMouse.x -= m_SceneViewPos.x;
				sceneMouse.y -= m_SceneViewPos.y;
				PlaceAssetInScene(path, sceneMouse);
			}
		}
		ImGui::EndDragDropTarget();
	}

	ImGui::SetCursorPos(ImVec2(imageCursor.x, imageCursor.y + available.y + ImGui::GetStyle().ItemSpacing.y));
	ImGui::TextDisabled("左クリック: 選択 / 右ドラッグ: カメラ / ホイール: ズーム");
	ImGui::End();
}

void ImGuiManager::DrawTransformGizmo()
{
	if (m_SelectedEntity == g_kINVALID_ENTITY ||
		!Registry::IsAlive(m_SelectedEntity) ||
		!ComponentManager::HasComponent<TransformComponent>(m_SelectedEntity))
	{
		return;
	}

	EntityID cameraEntity = Camera::GetCameraEntity();
	if (cameraEntity == g_kINVALID_ENTITY || !Registry::IsAlive(cameraEntity))
	{
		return;
	}

	XMMATRIX view;
	XMMATRIX proj;
	Camera::GetCameraMatrices(cameraEntity, view, proj);

	XMFLOAT4X4 viewMatrix{};
	XMFLOAT4X4 projectionMatrix{};
	XMStoreFloat4x4(&viewMatrix, view);
	XMStoreFloat4x4(&projectionMatrix, proj);

	auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(m_SelectedEntity);
	EntitySnapshot before = CaptureEntity(m_SelectedEntity);

	XMFLOAT4X4 modelMatrix{};
	XMStoreFloat4x4(&modelMatrix, BuildWorldMatrix(transform));

	const ImGuizmo::OPERATION operation = GetGizmoOperationFromIndex(m_GizmoOperation);
	ImGuizmo::MODE mode;
	if (operation == ImGuizmo::ROTATE)
	{
		mode = ImGuizmo::LOCAL;
	}
	else
	{
		mode = ImGuizmo::WORLD;
	}

	ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetRect(m_SceneViewPos.x, m_SceneViewPos.y, m_SceneViewSize.x, m_SceneViewSize.y);

	if (!ImGuizmo::Manipulate(
		&viewMatrix._11,
		&projectionMatrix._11,
		operation,
		mode,
		&modelMatrix._11))
	{
		return;
	}

	BeginUndoCapture(m_SelectedEntity, before);

	float translation[3]{};
	float rotationDeg[3]{};
	float scale[3]{};
	ImGuizmo::DecomposeMatrixToComponents(&modelMatrix._11, translation, rotationDeg, scale);

	transform.Position = { translation[0], translation[1], translation[2] };
	transform.Rotation =
	{
		rotationDeg[0] * kDegToRad,
		rotationDeg[1] * kDegToRad,
		rotationDeg[2] * kDegToRad
	};
	transform.Scale = ClampScale(scale, false);
	XMStoreFloat4x4(&transform.WorldMatrix, BuildWorldMatrix(transform));
	transform.IsDirty = true;
	if (ComponentManager::HasComponent<SunComponent>(m_SelectedEntity))
	{
		Sun::Sync(m_SelectedEntity);
	}
	else
	{
		ApplyLightEntityToRuntime(m_SelectedEntity);
	}
}

void ImGuiManager::DrawHierarchyWindow()
{
	ImGui::SetNextWindowPos(ImVec2(8.0f, 32.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(260.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("ヒエラルキー", &m_ShowHierarchyWindow))
	{
		ImGui::End();
		return;
	}

	const bool hierarchyFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (hierarchyFocused && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
		m_RenamingEntity == g_kINVALID_ENTITY && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		DeleteSelectedEntity();
	}
	ImGui::BeginDisabled(m_SelectedEntity == g_kINVALID_ENTITY);
	if (ImGui::Button("選択解除"))
	{
		m_SelectedEntity = g_kINVALID_ENTITY;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("+ 作成"))
	{
		ImGui::OpenPopup("HierarchyCreatePopup");
	}
	if (ImGui::BeginPopup("HierarchyCreatePopup"))
	{
		if (ImGui::MenuItem("GameCamera"))
		{
			int cameraNumber = 1;
			for (EntityID candidate : World::GetView<CameraComponent>())
			{
				if (ComponentManager::GetComponentUnchecked<CameraComponent>(
					candidate).IsGameCamera)
				{
					++cameraNumber;
				}
			}
			m_SelectedEntity = Camera::CreateGameCamera(
				"GameCamera " + to_string(cameraNumber),
				{ 5.0f, 4.0f, -10.0f },
				{ 0.0f, 3.0f, 0.0f },
				XM_PIDIV4,
				Camera::GetGameCameraEntity() == g_kINVALID_ENTITY);
			AddLog("GameCamera追加: #%u", m_SelectedEntity);
		}
		if (ImGui::BeginMenu("ライト"))
		{
			if (ImGui::MenuItem("Directional")) m_SelectedEntity = CreateLightEntity(LightType::Directional);
			if (ImGui::MenuItem("Point")) m_SelectedEntity = CreateLightEntity(LightType::Point);
			if (ImGui::MenuItem("Spot")) m_SelectedEntity = CreateLightEntity(LightType::Spot);
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
	}

	static ImGuiTextFilter entityFilter;
	DrawSearchField("EntitySearch", "名前で検索...", entityFilter);
	ImGui::Separator();
	int totalCount = 0;
	int visibleCount = 0;
	bool deleteRequested = false;
	ImGui::BeginChild("EntityList", ImVec2(0.0f, -ImGui::GetTextLineHeightWithSpacing()));
	for (EntityID entity : World::GetView<TransformComponent>())
	{
		if (!IsEditableEntity(entity))
		{
			continue;
		}

		++totalCount;
		const string displayName = GetEntityDisplayName(entity);
		if (m_RenamingEntity != entity && !entityFilter.PassFilter(displayName.c_str())) continue;
		++visibleCount;
		ImGui::PushID(static_cast<int>(entity));
		const bool selected = entity == m_SelectedEntity;
		if (m_RenamingEntity == entity)
		{
			DrawRenameInput(entity);
		}
		else if (ImGui::Selectable(displayName.c_str(), selected))
		{
			m_SelectedEntity = entity;
		}
		if (ImGui::BeginPopupContextItem("EntityActions"))
		{
			m_SelectedEntity = entity;
			if (ImGui::MenuItem("名前を変更", "F2", false, ComponentManager::HasComponent<NameComponent>(entity))) BeginRename(entity);
			if (ImGui::MenuItem("削除", "Delete")) deleteRequested = true;
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	if (visibleCount == 0)
	{
		ImGui::TextWrapped("表示するオブジェクトがありません。検索条件を変更するか、+ 作成から追加してください。");
	}
	ImGui::EndChild();
	if (deleteRequested) DeleteSelectedEntity();
	ImGui::TextDisabled("%d / %d 件", visibleCount, totalCount);
	ImGui::End();
}

void ImGuiManager::DeleteSelectedEntity()
{
	if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
	{
		return;
	}

	const EntityID entity = m_SelectedEntity;
	PushUndoSnapshot(CaptureEntity(entity));
	AddLog("エンティティ削除: %s", GetEntityDisplayName(entity));
	if (m_RenamingEntity == entity)
	{
		CancelRename();
	}
	World::DestroyEntity(Entity(entity));
	m_SelectedEntity = g_kINVALID_ENTITY;
}

EntityID ImGuiManager::CreateLightEntity(LightType type)
{
	Light::CreateDesc desc = Light::MakeDefaultDesc(type);
	desc.Name = string(GetLightTypeName(type)) + " Light";
	const EntityID entity = Light::Create(desc);
	ApplyLightEntityToRuntime(entity);
	AddLog("ライト追加: %s #%u", GetEntityDisplayName(entity), entity);
	return entity;
}

bool ImGuiManager::GetSceneViewRay(const ImVec2& sceneMouse, XMVECTOR& outOrigin, XMVECTOR& outDirection)
{
	if (m_SceneViewSize.x <= 1.0f || m_SceneViewSize.y <= 1.0f)
	{
		return false;
	}

	EntityID cameraEntity = Camera::GetCameraEntity();
	if (cameraEntity == g_kINVALID_ENTITY || !Registry::IsAlive(cameraEntity))
	{
		return false;
	}

	XMMATRIX view;
	XMMATRIX proj;
	Camera::GetCameraMatrices(cameraEntity, view, proj);
	outOrigin = XMVector3Unproject(
		XMVectorSet(sceneMouse.x, sceneMouse.y, 0.0f, 1.0f),
		0.0f, 0.0f, m_SceneViewSize.x, m_SceneViewSize.y, 0.0f, 1.0f,
		proj, view, XMMatrixIdentity());
	XMVECTOR farPoint = XMVector3Unproject(
		XMVectorSet(sceneMouse.x, sceneMouse.y, 1.0f, 1.0f),
		0.0f, 0.0f, m_SceneViewSize.x, m_SceneViewSize.y, 0.0f, 1.0f,
		proj, view, XMMatrixIdentity());
	outDirection = XMVector3Normalize(XMVectorSubtract(farPoint, outOrigin));
	return true;
}

void ImGuiManager::PickEntityFromMouse()
{
	ImGuiIO& io = ImGui::GetIO();
	if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
		!m_IsSceneViewHovered ||
		ImGuizmo::IsOver() ||
		ImGuizmo::IsUsing())
	{
		return;
	}

	const float width = m_SceneViewSize.x;
	const float height = m_SceneViewSize.y;
	if (width <= 0.0f || height <= 0.0f)
	{
		return;
	}

	ImVec2 mouse = io.MousePos;
	mouse.x -= m_SceneViewPos.x;
	mouse.y -= m_SceneViewPos.y;
	if (mouse.x < 0.0f || mouse.y < 0.0f || mouse.x >= width || mouse.y >= height)
	{
		return;
	}

	EntityID cameraEntity = Camera::GetCameraEntity();
	if (cameraEntity == g_kINVALID_ENTITY || !Registry::IsAlive(cameraEntity))
	{
		return;
	}

	XMMATRIX view;
	XMMATRIX proj;
	Camera::GetCameraMatrices(cameraEntity, view, proj);

	XMVECTOR nearPoint = XMVector3Unproject(
		XMVectorSet(mouse.x, mouse.y, 0.0f, 1.0f),
		0.0f, 0.0f, width, height, 0.0f, 1.0f,
		proj, view, XMMatrixIdentity());
	XMVECTOR farPoint = XMVector3Unproject(
		XMVectorSet(mouse.x, mouse.y, 1.0f, 1.0f),
		0.0f, 0.0f, width, height, 0.0f, 1.0f,
		proj, view, XMMatrixIdentity());
	XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint));

	struct PickHit
	{
		EntityID Entity = g_kINVALID_ENTITY;
		float Distance = 0.0f;
	};
	vector<PickHit> hits;

	for (EntityID entity : World::GetView<TransformComponent>())
	{
		if (!IsEditableEntity(entity))
		{
			continue;
		}

		XMFLOAT3 center{};
		XMFLOAT3 extents{};
		if (!GetLocalAabb(entity, center, extents))
		{
			continue;
		}

		auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
		BoundingOrientedBox localBox(center, extents, XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f));
		BoundingOrientedBox worldBox;
		localBox.Transform(worldBox, BuildWorldMatrix(transform));

		float distance = 0.0f;
		if (worldBox.Intersects(nearPoint, rayDir, distance))
		{
			hits.push_back({ entity, distance });
		}
	}

	sort(hits.begin(), hits.end(), [](const PickHit& lhs, const PickHit& rhs)
		{
			if (fabsf(lhs.Distance - rhs.Distance) > 0.0001f)
			{
				return lhs.Distance < rhs.Distance;
			}
			return lhs.Entity < rhs.Entity;
		});

	vector<EntityID> candidates;
	candidates.reserve(hits.size());
	for (const PickHit& hit : hits)
	{
		candidates.push_back(hit.Entity);
	}

	if (hits.empty())
	{
		m_SelectedEntity = g_kINVALID_ENTITY;
		m_LastPickCandidates.clear();
		m_LastPickMouse = mouse;
		return;
	}

	const float pickDx = mouse.x - m_LastPickMouse.x;
	const float pickDy = mouse.y - m_LastPickMouse.y;
	const bool samePickSpot = (pickDx * pickDx + pickDy * pickDy) <= 36.0f;
	const bool sameCandidateStack = samePickSpot && candidates == m_LastPickCandidates;

	size_t pickIndex = 0;
	if (sameCandidateStack)
	{
		for (size_t i = 0; i < candidates.size(); ++i)
		{
			if (candidates[i] == m_SelectedEntity)
			{
				pickIndex = (i + 1) % candidates.size();
				break;
			}
		}
	}

	m_SelectedEntity = hits[pickIndex].Entity;
	m_LastPickCandidates = move(candidates);
	m_LastPickMouse = mouse;
}

bool ImGuiManager::IsEditableEntity(EntityID entity)
{
	if (!Registry::IsAlive(entity) || !ComponentManager::HasComponent<TransformComponent>(entity))
	{
		return false;
	}

	if (ComponentManager::HasComponent<CameraComponent>(entity))
	{
		return true;
	}

	if (ComponentManager::HasComponent<MeshComponent>(entity) ||
		ComponentManager::HasComponent<StaticModelComponent>(entity) ||
		ComponentManager::HasComponent<AnimationModelComponent>(entity))
	{
		return true;
	}

	if (ComponentManager::HasComponent<SpriteComponent>(entity))
	{
		const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
		return sprite.Is3D;
	}

	if (ComponentManager::HasComponent<LightComponent>(entity))
	{
		return true;
	}

	return false;
}

void ImGuiManager::BeginRename(EntityID entity)
{
	if (entity == g_kINVALID_ENTITY ||
		!Registry::IsAlive(entity) ||
		!ComponentManager::HasComponent<NameComponent>(entity))
	{
		return;
	}

	m_RenamingEntity = entity;
	m_RenameNeedsFocus = true;
	m_RenameBuffer = ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name;
}

void ImGuiManager::CommitRename()
{
	if (m_RenamingEntity == g_kINVALID_ENTITY ||
		!Registry::IsAlive(m_RenamingEntity) ||
		!ComponentManager::HasComponent<NameComponent>(m_RenamingEntity))
	{
		CancelRename();
		return;
	}

	if (!m_RenameBuffer.empty())
	{
		PushUndoSnapshot(CaptureEntity(m_RenamingEntity));
		auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(m_RenamingEntity);
		name.Name = m_RenameBuffer;
		World::RegisterName(m_RenamingEntity, name.Name);
		AddLog("リネーム: %s #%u", name.Name.c_str(), m_RenamingEntity);
	}

	CancelRename();
}

void ImGuiManager::CancelRename()
{
	m_RenamingEntity = g_kINVALID_ENTITY;
	m_RenameBuffer.clear();
	m_RenameNeedsFocus = false;
}

bool ImGuiManager::DrawRenameInput(EntityID entity)
{
	if (entity != m_RenamingEntity)
	{
		return false;
	}

	char buffer[256] {};
	strncpy_s(buffer, m_RenameBuffer.c_str(), _TRUNCATE);
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::PushID(static_cast<int>(entity));
	if (m_RenameNeedsFocus)
	{
		ImGui::SetKeyboardFocusHere();
		m_RenameNeedsFocus = false;
	}
	const bool submitted = ImGui::InputText("##Rename", buffer, IM_ARRAYSIZE(buffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	m_RenameBuffer = buffer;
	const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();
	ImGui::PopID();

	if (submitted || deactivated)
	{
		CommitRename();
		return true;
	}
	return false;
}

const char* ImGuiManager::GetLightTypeName(LightType type)
{
	switch (type)
	{
	case LightType::Directional: return "Directional";
	case LightType::Point: return "Point";
	case LightType::Spot: return "Spot";
	case LightType::Volume: return "Volume";
	default: return "Unknown";
	}
}

const char* ImGuiManager::GetEntityDisplayName(EntityID entity)
{
	static string label;
	if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity))
	{
return "なし";
	}

	if (ComponentManager::HasComponent<NameComponent>(entity))
	{
		label = ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name;
	}
	else
	{
		label = "エンティティ";
	}
	label += " #";
	label += to_string(entity);
	return label.c_str();
}
