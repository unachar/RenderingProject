#include "pch.h"
#include "editorscenenavigation.h"
#include "imguimanager.h"
#include <unordered_map>
#include <json/json.h>
#include "daynightcycle.h"
#include "renderingvalidation.h"
#include "projectmanager.h"
#include "physicssystem.h"
#include "systemmanager.h"
#include "timelinesystem.h"
#include "proceduralscene.h"
#include "animator.h"
#include "texturemanager.h"
#include "modelmanager.h"
#include "scenefile.h"
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

	// Unityと同じく、先頭に現在のシーン名を表示（未保存の変更があれば「*」）
	{
		const string sceneLabel = GetSceneDisplayName() + (IsSceneDirty() ? "*" : "");
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
		ImGui::TextUnformatted(sceneLabel.c_str());
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", m_ScenePath.empty() ? "未保存のシーン（Ctrl+S で保存）" : m_ScenePath.generic_string().c_str());
		}
		if (ImGui::BeginPopupContextItem("SceneHeaderMenu"))
		{
			DrawSceneFileMenu();
			ImGui::EndPopup();
		}
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

// ============================================================================
// シーンシステム（Unity風）
//  ・.scene（JSON）への保存 / 読み込み / 新規作成
//  ・変更検知：保存対象と同じ項目からハッシュを作り、保存時のハッシュと比べる（0.25秒ごと）
//  ・未保存のまま切り替える・終了するときは「保存しますか？」を出す
//  ・cfg の scene= に現在のシーンを記録し、次回起動時に開く
// ============================================================================
namespace
{
	template<typename T>
	void VisitLiveComponent(SceneFile::Archive& archive, const char* key, EntityID entity)
	{
		bool present = ComponentManager::HasComponent<T>(entity);
		if (!present)
		{
			if (archive.Hashing())
			{
				const uint8_t none = 0;
				archive.HashBytes(&none, 1);
			}
			return;
		}
		SceneFile::Component(archive, key, present, ComponentManager::GetComponentUnchecked<T>(entity));
	}

	// 書き込み / ハッシュ共通：1エンティティ分のコンポーネントを列挙する
	void VisitLiveEntity(SceneFile::Archive& archive, EntityID entity)
	{
		// 昼夜サイクルが動かしている太陽の位置・光は、変更検知の対象から外す（保存はする）
		bool drivenSun = false;
		if (archive.Hashing() && ComponentManager::HasComponent<SunComponent>(entity))
		{
			const auto& cycle = DayNightCycle::GetSettings();
			drivenSun = cycle.TimeDriven || cycle.AutomaticLighting;
		}
		VisitLiveComponent<NameComponent>(archive, "name", entity);
		if (!drivenSun) VisitLiveComponent<TransformComponent>(archive, "transform", entity);
		VisitLiveComponent<ShaderComponent>(archive, "shader", entity);
		VisitLiveComponent<StaticModelComponent>(archive, "staticModel", entity);
		VisitLiveComponent<AnimationModelComponent>(archive, "animationModel", entity);
		if (!drivenSun) VisitLiveComponent<LightComponent>(archive, "light", entity);
		VisitLiveComponent<SunComponent>(archive, "sun", entity);
		VisitLiveComponent<ProceduralTerrainComponent>(archive, "terrain", entity);
		VisitLiveComponent<ProceduralMotionComponent>(archive, "motion", entity);
		VisitLiveComponent<MaterialComponent>(archive, "material", entity);
		VisitLiveComponent<AABBComponent>(archive, "aabb", entity);
		VisitLiveComponent<SpriteComponent>(archive, "sprite", entity);
		VisitLiveComponent<MeshComponent>(archive, "mesh", entity);
		VisitLiveComponent<CameraComponent>(archive, "camera", entity);
		VisitLiveComponent<PostProcessComponent>(archive, "postProcess", entity);
		VisitLiveComponent<InputComponent>(archive, "input", entity);
		VisitLiveComponent<MoveComponent>(archive, "move", entity);
		VisitLiveComponent<PhysicsComponent>(archive, "physics", entity);
		VisitLiveComponent<TimelineComponent>(archive, "timeline", entity);
		VisitLiveComponent<OBBComponent>(archive, "obb", entity);
		VisitLiveComponent<LODComponent>(archive, "lod", entity);
		VisitLiveComponent<InstancingComponent>(archive, "instancing", entity);

		// プリミティブ・地形以外のメッシュは頂点そのものが形状。ハッシュでは頂点の代わりにジオメトリハッシュを使う
		if (ComponentManager::HasComponent<MeshComponent>(entity) && !ComponentManager::HasComponent<ProceduralTerrainComponent>(entity))
		{
			auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
			if (mesh.PrimitiveType == 0)
			{
				if (archive.Hashing())
				{
					archive.HashBytes(&mesh.GeometryHash, sizeof(mesh.GeometryHash));
				}
				else if (archive.Writing() && mesh.NaniteMesh && !mesh.NaniteMesh->CpuVertices.empty())
				{
					vector<float> packed;
					packed.reserve(mesh.NaniteMesh->CpuVertices.size() * 12);
					for (const auto& vertex : mesh.NaniteMesh->CpuVertices)
					{
						for (float value : { vertex.Position.x, vertex.Position.y, vertex.Position.z,
							vertex.Normal.x, vertex.Normal.y, vertex.Normal.z, vertex.TexCoord.x, vertex.TexCoord.y,
							vertex.Diffuse.x, vertex.Diffuse.y, vertex.Diffuse.z, vertex.Diffuse.w })
						{
							packed.push_back(value);
						}
					}
					string encoded = SceneFile::EncodeBase64(packed.data(), packed.size() * sizeof(float));
					archive.Object("meshVertices", [&](SceneFile::Archive& v)
					{
						int floatsPerVertex = 12;
						v("floatsPerVertex", floatsPerVertex);
						v("data", encoded);
					});
				}
			}
		}
	}

	string ToUtf8(const filesystem::path& path)
	{
		const auto text = path.generic_u8string();
		return string(text.begin(), text.end());
	}

	wstring Utf8ToWide(const string& text)
	{
		if (text.empty()) return {};
		const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
		wstring result(static_cast<size_t>(max(length, 0)), L'\0');
		if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length);
		return result;
	}

	// 作業フォルダからの相対パス（日本語を含むパスも壊さないよう path のまま扱う）
	filesystem::path RelativeToWorkingDirectory(const filesystem::path& path)
	{
		error_code error;
		const filesystem::path relative = filesystem::relative(path, filesystem::current_path(error), error);
		return error || relative.empty() ? path.lexically_normal() : relative;
	}

	const char* kSaveChangesPopup = "シーンの変更を保存##SceneSaveChanges";
	const char* kSaveAsPopup = "名前を付けて保存##SceneSaveAs";
}

bool ImGuiManager::SceneTrackingAllowed()
{
	if (RenderingValidation::IsEnabled()) return false;
	char validationMode[8]{};
	return !(GetEnvironmentVariableA("DX12_VALIDATION_MODE", validationMode, sizeof(validationMode)) > 0 && atoi(validationMode) != 0);
}

bool ImGuiManager::IsSceneFile(const filesystem::path& path)
{
	string extension = path.extension().generic_string();
	transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	return extension == SceneFile::kExtension;
}

string ImGuiManager::GetSceneDisplayName()
{
	return m_ScenePath.empty() ? string("Untitled") : ToUtf8(m_ScenePath.stem());
}

// シーンに属するエンティティ：Transformを持ち、エディターカメラではないもの（EntityID昇順）
vector<EntityID> ImGuiManager::CollectSceneEntities()
{
	const EntityID editorCamera = Camera::GetEditorCameraEntity();
	vector<EntityID> entities;
	for (EntityID entity : World::GetView<TransformComponent>()) // EntityID昇順
	{
		if (entity != editorCamera) entities.push_back(entity);
	}
	return entities;
}

uint64_t ImGuiManager::ComputeSceneHash()
{
	uint64_t hash = 1469598103934665603ull;
	const vector<EntityID> entities = CollectSceneEntities();
	unordered_map<EntityID, EntityID> fileIds;
	fileIds.reserve(entities.size());
	for (size_t i = 0; i < entities.size(); ++i) fileIds[entities[i]] = static_cast<EntityID>(i);
	SceneFile::Archive archive(hash);
	archive.EntityMap = &fileIds;
	const uint64_t count = entities.size();
	archive.HashBytes(&count, sizeof(count));
	for (EntityID entity : entities)
	{
		VisitLiveEntity(archive, entity);
	}
	return hash;
}

string ImGuiManager::SerializeScene()
{
	const vector<EntityID> entities = CollectSceneEntities();
	unordered_map<EntityID, EntityID> fileIds;
	fileIds.reserve(entities.size());
	for (size_t i = 0; i < entities.size(); ++i) fileIds[entities[i]] = static_cast<EntityID>(i);

	Json::Value root(Json::objectValue);
	root["format"] = "DX12Scene";
	root["version"] = SceneFile::kVersion;
	root["entities"] = Json::Value(Json::arrayValue);
	for (EntityID entity : entities)
	{
		Json::Value record(Json::objectValue);
		record["id"] = fileIds[entity];
		SceneFile::Archive archive(SceneFile::Mode::Write, record);
		archive.EntityMap = &fileIds;
		VisitLiveEntity(archive, entity);
		root["entities"].append(record);
	}
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "  ";
	writer["precision"] = 9; // float を往復で同じ値に戻せる桁数
	return Json::writeString(writer, root);
}

bool ImGuiManager::SaveScene(const filesystem::path& path)
{
	if (ProjectManager::IsPlaying())
	{
		AddLog("再生中はシーンを保存できません。停止してから保存してください");
		return false;
	}
	error_code error;
	if (path.has_parent_path()) filesystem::create_directories(path.parent_path(), error);
	const string contents = SerializeScene();
	filesystem::path temporary = path;
	temporary += L".tmp";
	{
		ofstream file(temporary, ios::binary | ios::trunc);
		file << contents;
		if (!file)
		{
			AddLog("シーンの保存に失敗しました: %s", ToUtf8(path).c_str());
			return false;
		}
	}
	if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		filesystem::remove(temporary, error);
		AddLog("シーンの保存に失敗しました: %s", ToUtf8(path).c_str());
		return false;
	}
	m_ScenePath = RelativeToWorkingDirectory(path);
	m_SceneSavedHash = ComputeSceneHash();
	m_SceneCurrentHash = m_SceneSavedHash;
	SaveProjectSettings(); // cfg の scene= を更新
	AddLog("シーンを保存: %s", ToUtf8(m_ScenePath).c_str());
	return true;
}

void ImGuiManager::DestroySceneEntities()
{
	GraphicsDevice::WaitForGpuIdle();
	if (ProjectManager::IsPlaying()) ProjectManager::Stop();
	CancelRename();
	m_SelectedEntity = g_kINVALID_ENTITY;
	m_LastPickCandidates.clear();
	m_UndoStack.clear();
	m_RedoStack.clear();
	m_HasPendingUndo = false;
	m_PendingUndo = {};
	primitiveCreation = {};
	// コードで作られたタイムライン（Director）は破棄するエンティティを参照しているため、一緒に破棄する
	TimeLineSystem::Clear();
	for (EntityID entity : CollectSceneEntities())
	{
		World::DestroyEntity(Entity(entity));
	}
}

void ImGuiManager::RebuildLoadedEntity(EntityID entity, const void* recordPointer)
{
	const Json::Value& record = *static_cast<const Json::Value*>(recordPointer);

	if (ComponentManager::HasComponent<StaticModelComponent>(entity))
	{
		auto& model = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
		model.ModelId = model.ModelPath.empty() ? -1 : ModelManager::LoadStaticModel(model.ModelPath.c_str(), model.IsConvert);
		if (model.ModelId < 0) AddLog("シーン: モデルを読み込めません: %s", model.ModelPath.c_str());
		else if (!record.isMember("aabb") && ComponentManager::HasComponent<AABBComponent>(entity))
		{
			if (auto* resource = ModelManager::GetStaticModel(model.ModelId))
			{
				auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
				bounds.Center = resource->GetAabbCenter();
				bounds.Extents = resource->GetAabbExtents();
			}
		}
	}

	if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
	{
		auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
		animation.ModelId = animation.ModelPath.empty() ? -1 : ModelManager::LoadAnimModel(animation.ModelPath.c_str(), animation.IsConvert);
		if (animation.ModelId < 0)
		{
			AddLog("シーン: アニメーションモデルを読み込めません: %s", animation.ModelPath.c_str());
		}
		else
		{
			const size_t count = min(animation.AnimationPaths.size(), animation.Animations.size());
			for (size_t i = 0; i < count; ++i)
			{
				if (!ModelManager::LoadAnimation(animation.ModelId, animation.AnimationPaths[i].c_str(), animation.Animations[i].c_str()))
				{
					AddLog("シーン: アニメーションを読み込めません: %s", animation.AnimationPaths[i].c_str());
				}
			}
			const bool playing = animation.IsPlaying;
			vector<string> layers;
			for (const auto& layer : animation.ActiveAnimationLayers) layers.push_back(layer.AnimationName);
			if (playing && !layers.empty()) Animator::Play(animation, layers);
			else if (playing && !animation.CurrentAnimation.empty()) Animator::Play(animation, animation.CurrentAnimation);
		}
	}

	if (ComponentManager::HasComponent<MeshComponent>(entity))
	{
		auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
		if (ComponentManager::HasComponent<ProceduralTerrainComponent>(entity))
		{
			const auto settings = ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(entity).Settings;
			if (ProceduralScene::GenerateTerrain(settings, entity) == g_kINVALID_ENTITY) AddLog("シーン: 地形を生成できません");
		}
		else if (mesh.PrimitiveType != 0)
		{
			VertexResource resource{};
			resource.entityid = entity;
			resource.objectType = static_cast<ObjectType>(mesh.PrimitiveType);
			resource.color = static_cast<Color>(mesh.PrimitiveColor);
			PrimitiveSettings settings;
			settings.Radius = mesh.PrimitiveRadius;
			settings.Height = mesh.PrimitiveHeight;
			settings.Width = mesh.PrimitiveWidth;
			settings.Depth = mesh.PrimitiveDepth;
			settings.Segments = mesh.PrimitiveSegments;
			settings.Rings = mesh.PrimitiveRings;
			settings.FaceDivisions = mesh.PrimitiveFaceDivisions;
			PrimitiveGeometry::CreateObjectVertex(resource, settings);
		}
		else if (record.isMember("meshVertices") && record["meshVertices"]["data"].isString())
		{
			vector<uint8_t> bytes;
			const int floatsPerVertex = record["meshVertices"].get("floatsPerVertex", 12).asInt();
			if (floatsPerVertex == 12 && SceneFile::DecodeBase64(record["meshVertices"]["data"].asString(), bytes) &&
				bytes.size() % (12 * sizeof(float)) == 0)
			{
				vector<Vertex> vertices(bytes.size() / (12 * sizeof(float)));
				const float* f = reinterpret_cast<const float*>(bytes.data());
				for (size_t i = 0; i < vertices.size(); ++i, f += 12)
				{
					vertices[i].Pos = { f[0], f[1], f[2] };
					vertices[i].Normal = { f[3], f[4], f[5] };
					vertices[i].Tex = { f[6], f[7] };
					vertices[i].Color = { f[8], f[9], f[10], f[11] };
				}
				if (!PrimitiveGeometry::UploadVertices(entity, vertices)) AddLog("シーン: メッシュを復元できません #%u", entity);
			}
		}
	}

	if (ComponentManager::HasComponent<SpriteComponent>(entity))
	{
		const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
		if (sprite.SourceShape != 0)
		{
			VertexResource resource{};
			resource.entityid = entity;
			resource.shapetype = static_cast<ShapeType>(sprite.SourceShape);
			resource.radius = sprite.SourceRadius;
			resource.color = static_cast<Color>(sprite.SourceColor);
			PrimitiveGeometry::CreateSpriteVertex(resource);
		}
	}

	if (ComponentManager::HasComponent<MaterialComponent>(entity))
	{
		auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
		if (!material.TexturePath.empty()) material.TextureID = TextureManager::LoadTexture(material.TexturePath.c_str());
		else if (material.UseTexture) material.TextureID = TextureManager::GetDefaultTextureIndex();
		if (!material.NormalMapPath.empty()) material.NormalMapID = TextureManager::LoadNormalTexture(material.NormalMapPath.c_str());
		if (!material.MeshShadingOverrides.empty()) ApplyMeshShadingOverridesToModel(entity);
	}

	if (ComponentManager::HasComponent<CameraComponent>(entity))
	{
		const auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
		if (camera.IsGameCamera && camera.IsMainGameCamera) Camera::SetMainGameCamera(entity);
	}
	GpuSceneChanges::Notify(entity);
}

bool ImGuiManager::LoadScene(const filesystem::path& path)
{
	ifstream stream(path, ios::binary);
	if (!stream)
	{
		AddLog("シーンを開けません: %s", ToUtf8(path).c_str());
		return false;
	}
	Json::Value root;
	Json::CharReaderBuilder reader;
	string parseError;
	if (!Json::parseFromStream(reader, stream, &root, &parseError) || !root.isObject() ||
		root.get("format", "").asString() != "DX12Scene" || !root["entities"].isArray())
	{
		AddLog("シーンファイルが不正です: %s %s", ToUtf8(path).c_str(), parseError.c_str());
		return false;
	}
	if (root.get("version", 0).asInt() > SceneFile::kVersion)
	{
		AddLog("新しい形式のシーンです（version %d）。読める項目だけ読み込みます", root["version"].asInt());
	}
	const Json::Value& records = root["entities"];
	if (records.size() + CollectSceneEntities().size() > g_kMAX_ENTITIES)
	{
		AddLog("シーンのエンティティ数が上限を超えています");
		return false;
	}

	DestroySceneEntities();

	// 1回目：ID を確保（カメラの追従対象やタイムラインの参照を付け替えるため）
	unordered_map<EntityID, EntityID> idMap;
	vector<EntityID> created(records.size(), g_kINVALID_ENTITY);
	for (Json::ArrayIndex i = 0; i < records.size(); ++i)
	{
		const EntityID entity = World::CreateEntity().GetID();
		created[i] = entity;
		const EntityID fileId = records[i].get("id", i).asUInt();
		idMap[fileId] = entity;
	}

	// 2回目：コンポーネントを復元 → 実行時リソースを作り直す
	for (Json::ArrayIndex i = 0; i < records.size(); ++i)
	{
		const EntityID entity = created[i];
		if (entity == g_kINVALID_ENTITY) continue;
		Json::Value record = records[i];
		EntitySnapshot snapshot{};
		snapshot.Entity = entity;
		snapshot.WasAlive = true;
		SceneFile::Archive archive(SceneFile::Mode::Read, record);
		archive.EntityMap = &idMap;
		SceneFile::Component(archive, "name", snapshot.HasName, snapshot.Name);
		SceneFile::Component(archive, "transform", snapshot.HasTransform, snapshot.Transform);
		SceneFile::Component(archive, "shader", snapshot.HasShader, snapshot.Shader);
		SceneFile::Component(archive, "staticModel", snapshot.HasStaticModel, snapshot.StaticModel);
		SceneFile::Component(archive, "animationModel", snapshot.HasAnimationModel, snapshot.AnimationModel);
		SceneFile::Component(archive, "light", snapshot.HasLight, snapshot.Light);
		SceneFile::Component(archive, "sun", snapshot.HasSun, snapshot.Sun);
		SceneFile::Component(archive, "terrain", snapshot.HasProceduralTerrain, snapshot.ProceduralTerrain);
		SceneFile::Component(archive, "motion", snapshot.HasProceduralMotion, snapshot.ProceduralMotion);
		SceneFile::Component(archive, "material", snapshot.HasMaterial, snapshot.Material);
		SceneFile::Component(archive, "aabb", snapshot.HasAabb, snapshot.Aabb);
		SceneFile::Component(archive, "sprite", snapshot.HasSprite, snapshot.Sprite);
		SceneFile::Component(archive, "mesh", snapshot.HasMesh, snapshot.Mesh);
		SceneFile::Component(archive, "camera", snapshot.HasCamera, snapshot.Camera);
		SceneFile::Component(archive, "postProcess", snapshot.HasPostProcess, snapshot.PostProcess);
		SceneFile::Component(archive, "input", snapshot.HasInput, snapshot.Input);
		SceneFile::Component(archive, "move", snapshot.HasMove, snapshot.Move);
		SceneFile::Component(archive, "physics", snapshot.HasPhysics, snapshot.Physics);
		SceneFile::Component(archive, "timeline", snapshot.HasTimeline, snapshot.Timeline);
		SceneFile::Component(archive, "obb", snapshot.HasObb, snapshot.Obb);
		SceneFile::Component(archive, "lod", snapshot.HasLod, snapshot.Lod);
		if (!snapshot.HasTransform)
		{
			snapshot.HasTransform = true; // シーンのエンティティは必ずTransformを持つ
		}
		snapshot.Transform.IsDirty = true;
		snapshot.Transform.HasPreviousWorld = false;
		ApplySnapshot(snapshot);

		bool hasInstancing = false;
		InstancingComponent instancing{};
		SceneFile::Component(archive, "instancing", hasInstancing, instancing);
		if (hasInstancing)
		{
			ComponentManager::AddComponent(entity, ComponentType::INSTANCING);
			ComponentManager::GetComponentUnchecked<InstancingComponent>(entity) = instancing;
		}
		RebuildLoadedEntity(entity, &record);
	}

	if (auto* physics = SystemManager::GetSystem<PhysicsSystem>()) physics->ResetSimulation();
	MaterialManager::NotifyAssignmentsChanged();
	m_ScenePath = RelativeToWorkingDirectory(path);
	BeginSceneBaseline(3.0);
	SaveProjectSettings();
	AddLog("シーンを開きました: %s（%u エンティティ）", ToUtf8(m_ScenePath).c_str(), records.size());
	return true;
}

// 新規シーン：Unityと同じく「Main Camera」と平行光源（太陽）だけを置く
void ImGuiManager::CreateNewScene()
{
	DestroySceneEntities();
	Camera::CreateGameCamera("Main Camera", { 0.0f, 1.0f, -10.0f }, { 0.0f, 1.0f, 0.0f }, XM_PIDIV4, true);
	Sun::CreateDefault();
	if (auto* physics = SystemManager::GetSystem<PhysicsSystem>()) physics->ResetSimulation();
	MaterialManager::NotifyAssignmentsChanged();
	m_ScenePath.clear();
	BeginSceneBaseline(1.5);
	SaveProjectSettings();
	AddLog("新しいシーンを作成しました");
}

void ImGuiManager::BeginSceneBaseline(double settleSeconds)
{
	m_SceneTracking = SceneTrackingAllowed();
	if (!m_SceneTracking) return;
	m_SceneSavedHash = ComputeSceneHash();
	m_SceneCurrentHash = m_SceneSavedHash;
	m_SceneSettleUntil = ImGui::GetTime() + settleSeconds;
	m_SceneSettleRevision = m_EditRevision;
	m_SceneNextHashTime = 0.0;
}

// 毎フレーム（Update の最後）。0.25秒ごとにハッシュを取り直す。
void ImGuiManager::UpdateSceneTracking()
{
	if (m_SceneTracking && !ProjectManager::IsPlaying())
	{
		const double now = ImGui::GetTime();
		if (now >= m_SceneNextHashTime)
		{
			m_SceneNextHashTime = now + 0.25;
			m_SceneCurrentHash = ComputeSceneHash();
			// 読み込み直後はモデルの非同期読み込みや太陽の同期で値が落ち着くまで、ユーザーの編集がなければ基準を更新する
			if (now < m_SceneSettleUntil && m_EditRevision == m_SceneSettleRevision && !ImGui::IsAnyItemActive())
			{
				m_SceneSavedHash = m_SceneCurrentHash;
			}
		}
	}

	// ウィンドウタイトル：「DirectX12Window - シーン名*」
	string title = "DirectX12Window - " + GetSceneDisplayName() + (IsSceneDirty() ? "*" : "");
	if (ProjectManager::IsPlaying()) title += "  [再生中]";
	const wstring wideTitle = Utf8ToWide(title);
	if (wideTitle != m_LastWindowTitle)
	{
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		HWND hwnd = viewport ? static_cast<HWND>(viewport->PlatformHandleRaw) : nullptr;
		if (!hwnd && viewport) hwnd = static_cast<HWND>(viewport->PlatformHandle);
		if (hwnd && SetWindowTextW(hwnd, wideTitle.c_str())) m_LastWindowTitle = wideTitle;
	}
}

void ImGuiManager::RequestSceneAction(SceneAction action, const filesystem::path& path)
{
	if (m_PendingSceneAction != SceneAction::None && !m_ExecuteSceneAction)
	{
		return; // 確認中
	}
	if (ProjectManager::IsPlaying())
	{
		ProjectManager::Stop(); // Unityと同じく、シーン操作の前に再生を止める（編集時の状態に戻る）
	}
	m_PendingSceneAction = action;
	m_PendingScenePath = path;
	if (IsSceneDirty())
	{
		m_RequestSaveChangesPopup = true;
	}
	else
	{
		m_ExecuteSceneAction = true;
	}
}

void ImGuiManager::RequestQuit()
{
	if (!IsSceneDirty())
	{
		PostQuitMessage(0);
		return;
	}
	RequestSceneAction(SceneAction::Quit);
}

// Update の先頭（ImGui::NewFrame の前）で呼ぶ。描画中ではないのでエンティティを安全に作り直せる。
void ImGuiManager::ProcessSceneAction()
{
	if (!m_ExecuteSceneAction) return;
	const SceneAction action = m_PendingSceneAction;
	const filesystem::path path = m_PendingScenePath;
	m_ExecuteSceneAction = false;
	m_PendingSceneAction = SceneAction::None;
	m_PendingScenePath.clear();
	switch (action)
	{
	case SceneAction::Open:
		LoadScene(path);
		break;
	case SceneAction::New:
		CreateNewScene();
		break;
	case SceneAction::Quit:
		m_SceneTracking = false;
		PostQuitMessage(0);
		break;
	default:
		break;
	}
}

void ImGuiManager::SaveCurrentScene(bool saveAs)
{
	if (ProjectManager::IsPlaying())
	{
		AddLog("再生中はシーンを保存できません。停止してから保存してください");
		return;
	}
	if (saveAs || m_ScenePath.empty())
	{
		const string name = m_ScenePath.empty() ? string("NewScene") : ToUtf8(m_ScenePath.stem());
		snprintf(m_SaveAsName, sizeof(m_SaveAsName), "%s", name.c_str());
		const string directory = m_ScenePath.empty() ? string("asset/Scenes") : ToUtf8(m_ScenePath.parent_path());
		snprintf(m_SaveAsDirectory, sizeof(m_SaveAsDirectory), "%s", directory.c_str());
		m_RequestSaveAsPopup = true;
		return;
	}
	SaveScene(m_ScenePath);
}

void ImGuiManager::DrawSceneFileMenu()
{
	const bool playing = ProjectManager::IsPlaying();
	if (ImGui::MenuItem("新規シーン", "Ctrl+N")) RequestNewScene();
	if (ImGui::BeginMenu("シーンを開く"))
	{
		// asset 以下の .scene を一覧表示（メニューを開いている間だけ走査）
		vector<filesystem::path> scenes;
		error_code error;
		for (filesystem::recursive_directory_iterator it(m_AssetRoot, filesystem::directory_options::skip_permission_denied, error), end;
			!error && it != end && scenes.size() < 200; it.increment(error))
		{
			if (it->is_regular_file(error) && IsSceneFile(it->path())) scenes.push_back(it->path());
		}
		sort(scenes.begin(), scenes.end());
		if (scenes.empty()) ImGui::TextDisabled("asset フォルダに .scene がありません");
		for (const auto& scene : scenes)
		{
			const string label = ToUtf8(scene.lexically_relative(m_AssetRoot));
			const bool current = !m_ScenePath.empty() && filesystem::equivalent(scene, m_ScenePath, error);
			if (ImGui::MenuItem(label.c_str(), nullptr, current)) RequestOpenScene(scene);
		}
		ImGui::EndMenu();
	}
	ImGui::Separator();
	if (ImGui::MenuItem("保存", "Ctrl+S", false, !playing)) SaveCurrentScene(false);
	if (ImGui::MenuItem("名前を付けて保存...", "Ctrl+Shift+S", false, !playing)) SaveCurrentScene(true);
}

void ImGuiManager::DrawScenePopups()
{
	if (m_RequestSaveChangesPopup)
	{
		ImGui::OpenPopup(kSaveChangesPopup);
		m_RequestSaveChangesPopup = false;
	}
	if (m_RequestSaveAsPopup)
	{
		ImGui::OpenPopup(kSaveAsPopup);
		m_RequestSaveAsPopup = false;
	}

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal(kSaveChangesPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("シーン「%s」に保存されていない変更があります。", GetSceneDisplayName().c_str());
		ImGui::Text("変更を保存しますか？");
		ImGui::TextDisabled("保存しないと、変更は失われます。");
		ImGui::Spacing();
		if (ImGui::Button("保存", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
		{
			ImGui::CloseCurrentPopup();
			if (m_ScenePath.empty())
			{
				m_ContinueAfterSaveAs = true;
				SaveCurrentScene(true);
			}
			else if (SaveScene(m_ScenePath))
			{
				m_ExecuteSceneAction = true;
			}
			else
			{
				m_PendingSceneAction = SceneAction::None;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("保存しない", ImVec2(110.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			m_ExecuteSceneAction = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("キャンセル", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
			m_PendingSceneAction = SceneAction::None;
			m_PendingScenePath.clear();
		}
		ImGui::EndPopup();
	}

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal(kSaveAsPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::SetNextItemWidth(360.0f);
		ImGui::InputText("フォルダ", m_SaveAsDirectory, sizeof(m_SaveAsDirectory));
		ImGui::SetNextItemWidth(360.0f);
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		const bool enter = ImGui::InputText("ファイル名", m_SaveAsName, sizeof(m_SaveAsName), ImGuiInputTextFlags_EnterReturnsTrue);
		string name = m_SaveAsName;
		while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
		filesystem::path target = filesystem::u8path(m_SaveAsDirectory) / filesystem::u8path(name);
		if (!IsSceneFile(target)) target += SceneFile::kExtension;
		const bool validName = !name.empty() && name.find_first_of("\\/:*?\"<>|") == string::npos;
		error_code error;
		const bool exists = validName && filesystem::exists(target, error);
		ImGui::TextDisabled("保存先: %s", ToUtf8(target).c_str());
		if (!validName) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "ファイル名に使えない文字が含まれています");
		else if (exists) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "同じ名前のシーンがあります。上書きします");
		ImGui::Spacing();
		ImGui::BeginDisabled(!validName);
		if (ImGui::Button(exists ? "上書き保存" : "保存", ImVec2(120.0f, 0.0f)) || (enter && validName))
		{
			if (SaveScene(target))
			{
				ImGui::CloseCurrentPopup();
				if (m_ContinueAfterSaveAs) m_ExecuteSceneAction = true;
				m_ContinueAfterSaveAs = false;
			}
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
			if (m_ContinueAfterSaveAs)
			{
				m_PendingSceneAction = SceneAction::None;
				m_PendingScenePath.clear();
			}
			m_ContinueAfterSaveAs = false;
		}
		ImGui::EndPopup();
	}
}

