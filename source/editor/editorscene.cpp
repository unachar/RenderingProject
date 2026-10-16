#include "pch.h"
#include "editorscenenavigation.h"
#include "imguimanager.h"
#include "editorwidgets.h"
#include "camera.h"
#include "graphicsdevice.h"
#include "light.h"
#include "rendertargets.h"
#include "sun.h"
#include "world.h"
#include "primitivegeometry.h"
#include "materialmanager.h"
#include "gpuscene/gpuscenechanges.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <DirectXCollision.h>
#include <ImGuizmo.h>
#include "virtualgeometry/virtualgeometrydebug.h"



using namespace EditorWidgets;

namespace
{
	struct PrimitiveCreationState
	{
		EntityID Entity = g_kINVALID_ENTITY;
		ObjectType Type = ObjectType::NONE;
		PrimitiveSettings Settings;
		bool Open = false;
		bool FocusRequested = false;
		bool PendingUpdate = false;
	};
	PrimitiveCreationState primitiveCreation;
}

void ImGuiManager::DrawSceneViewWindow()
{
	m_IsSceneViewHovered = false;
	ImGui::SetNextWindowSize(ImVec2(860.0f, 520.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("シーンビュー"))
	{
		ImGui::End();
		return;
	}

	// Unityと同じ並び・キー：ハンド Q / 移動 W / 回転 E / スケール R、X でローカル/グローバル切り替え
	const char* toolLabels[] = { "ハンド Q", "移動 W", "回転 E", "スケール R" };
	const int toolOperations[] = { 3, 0, 1, 2 };
	const float toolbarWidth = ImGui::GetContentRegionAvail().x;
	const float toolSpacing = ImGui::GetStyle().ItemSpacing.x;
	float rowWidth = 0.0f;
	for (int tool = 0; tool < 4; ++tool)
	{
		const float toolWidth = ImGui::CalcTextSize(toolLabels[tool]).x + 16.0f;
		if (tool > 0 && rowWidth + toolSpacing + toolWidth <= toolbarWidth)
		{
			ImGui::SameLine();
			rowWidth += toolSpacing;
		}
		else rowWidth = 0.0f;
		if (ImGui::Selectable(toolLabels[tool], m_GizmoOperation == toolOperations[tool], 0,
			ImVec2(toolWidth, ImGui::GetFrameHeight())))
		{
			m_GizmoOperation = toolOperations[tool];
		}
		rowWidth += toolWidth;
	}
	{
		const char* spaceLabel = m_GizmoLocalSpace ? "ローカル X" : "グローバル X";
		const float spaceWidth = ImGui::CalcTextSize(spaceLabel).x + 16.0f;
		if (rowWidth + toolSpacing + spaceWidth <= toolbarWidth) { ImGui::SameLine(); rowWidth += toolSpacing; }
		else rowWidth = 0.0f;
		if (ImGui::Selectable(spaceLabel, false, 0, ImVec2(spaceWidth, ImGui::GetFrameHeight())))
		{
			m_GizmoLocalSpace = !m_GizmoLocalSpace;
		}
		rowWidth += spaceWidth;
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
    DrawMaterialDropTarget();
	VirtualGeometryDebug::DrawViewport(ImGui::GetWindowDrawList(),cursor,imageSize,m_SelectedEntity);
	DrawTransformGizmo();
	// Unity風のシーンビュー操作（ホイール / 中ボタンパン / Alt+左オービット / Fフォーカス）と統計表示
	EditorSceneNavigation::Update(m_IsSceneViewHovered, m_SelectedEntity, ImGuizmo::IsUsing());
	EditorSceneNavigation::DrawOverlay(ImGui::GetWindowDrawList(), cursor, imageSize);

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
	ImGui::TextDisabled("左: 選択 / 右ドラッグ+WASD: 飛行 / 中ドラッグ: パン / Alt+左: 回転 / ホイール: ズーム / F: フォーカス");
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

	if (m_GizmoOperation == 3)
	{
		return; // ハンドツール：ギズモを出さない
	}
	const ImGuizmo::OPERATION operation = GetGizmoOperationFromIndex(m_GizmoOperation);
	// スケールは常にローカル。それ以外は X キーで切り替えたモードに従う（Unityと同じ）。
	const ImGuizmo::MODE mode = (m_GizmoLocalSpace || operation == ImGuizmo::SCALE) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

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

void ImGuiManager::DrawPrimitiveCreationMenu()
{
	struct PrimitiveEntry
	{
		const char* Label;
		const char* Name;
		ObjectType Type;
	};
	const PrimitiveEntry shapes2D[] = {
		{ "円", "Circle", ObjectType::CIRCLE },
		{ "四角", "Square", ObjectType::QUAD },
		{ "三角", "Triangle", ObjectType::TRIANGLE }
	};
	const PrimitiveEntry shapes3D[] = {
		{ "球", "Sphere", ObjectType::SPHERE },
		{ "キューブ", "Cube", ObjectType::CUBE },
		{ "カプセル", "Capsule", ObjectType::CAPSULE },
		{ "円柱", "Cylinder", ObjectType::CYLINDER },
		{ "円錐", "Cone", ObjectType::CONE },
		{ "三角錐", "Tetrahedron", ObjectType::TETRAHEDRON },
		{ "三角柱", "Triangular Prism", ObjectType::TRIANGULAR_PRISM },
		{ "平面", "Plane", ObjectType::PLANE }
	};
	const auto drawEntries = [&](const auto& entries)
	{
		for (const auto& entry : entries)
		{
			if (!ImGui::MenuItem(entry.Label)) continue;
			auto entity = World::CreateEntity()
				.Add<TransformComponent>().Add<MeshComponent>()
				.Add<MaterialComponent>().Add<NameComponent>();
			entity.SetName(entry.Name);
			auto& material = entity.Get<MaterialComponent>();
			material.Metallic = 0.0f;
			material.Roughness = 0.5f;
			material.Fresnel = 0.04f;
			material.ShaderClassMode = MaterialMode::Manual;
			material.ShaderClass = ShaderClass::PBR;
			material.UseTexture = false;
			VertexResource resource{};
			resource.entityid = entity.GetID();
			resource.color = Color::WHITE;
			resource.objectType = entry.Type;
			PrimitiveSettings settings;
			if (entry.Type == ObjectType::CAPSULE) settings.Rings = 8;
			PrimitiveGeometry::CreateObjectVertex(resource, settings);
			if (!entity.Get<MeshComponent>().VertexBuffer)
			{
				AddLog("シェイプ作成失敗: %s", entry.Name);
				World::DestroyEntity(entity);
				continue;
			}
			m_SelectedEntity = entity.GetID();
			primitiveCreation = {};
			primitiveCreation.Entity = m_SelectedEntity;
			primitiveCreation.Type = entry.Type;
			primitiveCreation.Settings = settings;
			primitiveCreation.Open = true;
			primitiveCreation.FocusRequested = true;
			AddLog("シェイプ追加: %s #%u", entry.Name, m_SelectedEntity);
		}
	};
	if (ImGui::BeginMenu("2Dオブジェクト"))
	{
		drawEntries(shapes2D);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("3Dオブジェクト"))
	{
		drawEntries(shapes3D);
		ImGui::EndMenu();
	}
}

void ImGuiManager::DrawPrimitiveSettingsWindow()
{
	auto& state = primitiveCreation;
	if (!state.Open) return;
	if (m_SelectedEntity != state.Entity || !Registry::IsAlive(state.Entity) ||
		!ComponentManager::HasComponent<MeshComponent>(state.Entity))
	{
		state = {};
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(310.0f, 0.0f), ImGuiCond_Appearing);
	if (state.FocusRequested)
	{
		ImGui::SetNextWindowFocus();
		state.FocusRequested = false;
	}
	if (ImGui::Begin("シェイプ作成設定", &state.Open, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted(GetEntityDisplayName(state.Entity));
		ImGui::Separator();
		auto& settings = state.Settings;
		const bool rectangular = state.Type == ObjectType::CUBE ||
			state.Type == ObjectType::QUAD || state.Type == ObjectType::PLANE;
		bool changed = false;
		if (rectangular || state.Type == ObjectType::TRIANGLE ||
			state.Type == ObjectType::TETRAHEDRON || state.Type == ObjectType::TRIANGULAR_PRISM)
			changed |= ImGui::SliderInt("面の分割数", &settings.FaceDivisions, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
		if (rectangular)
			changed |= ImGui::DragFloat("幅", &settings.Width, 0.01f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		else
			changed |= ImGui::DragFloat("半径", &settings.Radius, 0.01f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (state.Type != ObjectType::CIRCLE && state.Type != ObjectType::TRIANGLE &&
			state.Type != ObjectType::SPHERE && state.Type != ObjectType::PLANE)
			changed |= ImGui::DragFloat(state.Type == ObjectType::CAPSULE ? "円柱部分の高さ" : "高さ",
				&settings.Height, 0.01f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (state.Type == ObjectType::CUBE || state.Type == ObjectType::PLANE)
			changed |= ImGui::DragFloat("奥行き", &settings.Depth, 0.01f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (state.Type == ObjectType::CIRCLE || state.Type == ObjectType::SPHERE ||
			state.Type == ObjectType::CAPSULE || state.Type == ObjectType::CYLINDER || state.Type == ObjectType::CONE)
			changed |= ImGui::SliderInt("周方向の分割数", &settings.Segments, 3, 128, "%d", ImGuiSliderFlags_AlwaysClamp);
		if (state.Type == ObjectType::SPHERE || state.Type == ObjectType::CAPSULE)
			changed |= ImGui::SliderInt(state.Type == ObjectType::CAPSULE ? "半球の分割数" : "縦方向の分割数",
				&settings.Rings, 2, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
		state.PendingUpdate |= changed;
		if (ImGui::Button("完了")) state.Open = false;
	}
	ImGui::End();
	if (state.PendingUpdate && !ImGui::IsAnyItemActive())
	{
		VertexResource resource{};
		resource.entityid = state.Entity;
		resource.objectType = state.Type;
		resource.color = Color::WHITE;
		PrimitiveGeometry::CreateObjectVertex(resource, state.Settings);
		state.PendingUpdate = false;
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
		DrawPrimitiveCreationMenu();
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
	bool duplicateRequested = false;
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
			if (ImGui::MenuItem("複製", "Ctrl+D")) duplicateRequested = true;
			if (ImGui::MenuItem("削除", "Delete")) deleteRequested = true;
			if (ImGui::BeginMenu("オブジェクトの作成"))
			{
				DrawPrimitiveCreationMenu();
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	if (visibleCount == 0)
	{
		ImGui::TextWrapped("表示するオブジェクトがありません。検索条件を変更するか、+ 作成から追加してください。");
	}
	if (ImGui::BeginPopupContextWindow("HierarchyBackgroundActions", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		if (ImGui::BeginMenu("オブジェクトの作成"))
		{
			DrawPrimitiveCreationMenu();
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
	}
	ImGui::EndChild();
	if (duplicateRequested) DuplicateSelectedEntity();
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

// Unity の "Cube" → "Cube (1)" → "Cube (2)" と同じ規則で、重複しない名前を作る。
string ImGuiManager::MakeDuplicateName(const string& sourceName)
{
	string baseName = sourceName;
	// 末尾の " (数字)" を外して基準名にする
	if (!baseName.empty() && baseName.back() == ')')
	{
		const size_t open = baseName.rfind(" (");
		if (open != string::npos && open + 2 < baseName.size() - 1)
		{
			bool digits = true;
			for (size_t i = open + 2; i + 1 < baseName.size(); ++i)
			{
				if (!isdigit(static_cast<unsigned char>(baseName[i]))) { digits = false; break; }
			}
			if (digits) baseName.erase(open);
		}
	}

	unordered_set<string> usedNames;
	for (EntityID entity : World::GetView<NameComponent>())
	{
		usedNames.insert(ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name);
	}
	for (int index = 1; index < 100000; ++index)
	{
		string candidate = baseName + " (" + to_string(index) + ")";
		if (usedNames.find(candidate) == usedNames.end()) return candidate;
	}
	return baseName + " (copy)";
}

// Ctrl+D：選択中のエンティティを同じ位置に複製し、複製した側を選択する（Unityと同じ）。
// ・モデル / メッシュ / マテリアル / GPUバッファは参照を共有する（静的モデルのキャッシュと同じ扱い）
// ・物理ボディは物理システムが新しいエンティティ用に作り直す
// ・太陽(SunComponent)は1つだけの前提なので、複製側には付けない（平行光源として複製される）
// ・メインのゲームカメラ指定は複製しない
// ・Ctrl+Z で複製を取り消せる
EntityID ImGuiManager::DuplicateSelectedEntity()
{
	const EntityID source = m_SelectedEntity;
	if (source == g_kINVALID_ENTITY || !Registry::IsAlive(source) || !IsEditableEntity(source) ||
		source == Camera::GetEditorCameraEntity())
	{
		return g_kINVALID_ENTITY;
	}

	EntitySnapshot copy = CaptureEntity(source);
	const EntityID duplicate = World::CreateEntity().GetID();
	if (duplicate == g_kINVALID_ENTITY || !Registry::IsAlive(duplicate))
	{
		AddLog("複製に失敗しました（エンティティ数の上限）");
		return g_kINVALID_ENTITY;
	}

	copy.Entity = duplicate;
	copy.WasAlive = true;
	if (copy.HasName)
	{
		copy.Name.Name = MakeDuplicateName(copy.Name.Name);
	}
	if (copy.HasTransform)
	{
		copy.Transform.HasPreviousWorld = false; // 初回フレームのモーションベクトルを出さない
		copy.Transform.IsDirty = true;
	}
	copy.HasSun = false;
	if (copy.HasCamera)
	{
		copy.Camera.IsMainGameCamera = false;
	}
	if (copy.HasAnimationModel)
	{
		copy.AnimationModel.CurrentTime = 0.0f;
		copy.AnimationModel.NextTime = 0.0f;
	}
	if (copy.HasTimeline)
	{
		copy.Timeline.CurrentTime = 0.0f;
	}
	ApplySnapshot(copy);

	// スナップショットに含まれないコンポーネント
	if (ComponentManager::HasComponent<InstancingComponent>(source))
	{
		const InstancingComponent instancing = ComponentManager::GetComponentUnchecked<InstancingComponent>(source);
		if (!ComponentManager::HasComponent<InstancingComponent>(duplicate))
		{
			ComponentManager::AddComponent(duplicate, ComponentType::INSTANCING);
		}
		ComponentManager::GetComponentUnchecked<InstancingComponent>(duplicate) = instancing;
	}
	GpuSceneChanges::Notify(duplicate);
	MaterialManager::NotifyAssignmentsChanged();

	// 取り消し用：「存在しなかった」状態を積む → Ctrl+Z で複製が消える
	EntitySnapshot undo{};
	undo.Entity = duplicate;
	undo.WasAlive = false;
	PushUndoSnapshot(undo);

	m_SelectedEntity = duplicate;
	AddLog("エンティティ複製: %s", GetEntityDisplayName(duplicate));
	return duplicate;
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
		io.KeyAlt || // Alt+左ドラッグはオービット操作
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
