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
#include "scenehierarchy.h"
#include "prefabformat.h"
#include "graphicslog.h"
#include "primitivegeometry.h"
#include "materialmanager.h"
#include "gpuscene/gpuscenechanges.h"
#include <fstream>
#include <future>
#include <thread>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include <functional>
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
    const char* toolLabels[] = {"ハンド Q", "移動 W", "回転 E", "スケール R"};
    const int toolOperations[] = {3, 0, 1, 2};
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
        else
        {
            rowWidth = 0.0f;
        }
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
        if (rowWidth + toolSpacing + spaceWidth <= toolbarWidth)
        {
            ImGui::SameLine();
            rowWidth += toolSpacing;
        }
        else
        {
            rowWidth = 0.0f;
        }
        if (ImGui::Selectable(spaceLabel, false, 0, ImVec2(spaceWidth, ImGui::GetFrameHeight())))
        {
            m_GizmoLocalSpace = !m_GizmoLocalSpace;
        }
        rowWidth += spaceWidth;
    }
    const float lightToggleWidth =
        ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("ライト可視化").x;
    if (rowWidth + toolSpacing + lightToggleWidth <= toolbarWidth)
    {
        ImGui::SameLine();
    }
    ImGui::Checkbox("ライト可視化", &m_ShowLightDebug);

    ImGui::Separator();
    const bool sceneFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (sceneFocused && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false))
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
    ImGui::SetCursorPos(
        ImVec2(imageCursor.x + (available.x - imageSize.x) * 0.5f, imageCursor.y + (available.y - imageSize.y) * 0.5f));
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    m_SceneViewPos = cursor;
    m_SceneViewSize = imageSize;

    D3D12_GPU_DESCRIPTOR_HANDLE handle = RenderTargets::GetEditorSceneSrvHandle();
    ImGui::Image((ImTextureData*)handle.ptr, imageSize);
    m_IsSceneViewHovered = ImGui::IsItemHovered();
    DrawMaterialDropTarget();
    VirtualGeometryDebug::DrawViewport(ImGui::GetWindowDrawList(), cursor, imageSize, m_SelectedEntity);
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
                PlaceAssetInScene(filesystem::u8path(path), sceneMouse); // ペイロードは UTF-8。
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SetCursorPos(ImVec2(imageCursor.x, imageCursor.y + available.y + ImGui::GetStyle().ItemSpacing.y));
    ImGui::TextDisabled(
        "左: 選択 / 右ドラッグ+WASD: 飛行 / 中ドラッグ: パン / Alt+左: 回転 / ホイール: ズーム / F: フォーカス");
    ImGui::End();
}

void ImGuiManager::DrawTransformGizmo()
{
    if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity) ||
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
    const TransformComponent beforeTransform = transform;

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

    if (!ImGuizmo::Manipulate(&viewMatrix._11, &projectionMatrix._11, operation, mode, &modelMatrix._11))
    {
        return;
    }

    // Reject an invalid gizmo result before it can corrupt the entity or its undo history.
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            if (!std::isfinite(modelMatrix.m[row][column]))
            {
                return;
            }
        }
    }

    float translation[3]{};
    float rotationDeg[3]{};
    float scale[3]{};
    ImGuizmo::DecomposeMatrixToComponents(&modelMatrix._11, translation, rotationDeg, scale);

    for (int axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(translation[axis]) || !std::isfinite(rotationDeg[axis]) || !std::isfinite(scale[axis]))
        {
            return;
        }
    }

    TransformComponent after = beforeTransform;
    after.Position = {translation[0], translation[1], translation[2]};
    after.Rotation = {rotationDeg[0] * kDegToRad, rotationDeg[1] * kDegToRad, rotationDeg[2] * kDegToRad};
    after.Scale = ClampScale(scale, false);
    ApplySelectionTransform(beforeTransform, after);
}

void ImGuiManager::DrawPrimitiveCreationMenu()
{
    struct PrimitiveEntry
    {
        const char* Label;
        const char* Name;
        ObjectType Type;
    };

    const PrimitiveEntry shapes2D[] = {{"円", "Circle", ObjectType::CIRCLE},
                                       {"四角", "Square", ObjectType::QUAD},
                                       {"三角", "Triangle", ObjectType::TRIANGLE}};
    const PrimitiveEntry shapes3D[] = {{"球", "Sphere", ObjectType::SPHERE},
                                       {"キューブ", "Cube", ObjectType::CUBE},
                                       {"カプセル", "Capsule", ObjectType::CAPSULE},
                                       {"円柱", "Cylinder", ObjectType::CYLINDER},
                                       {"円錐", "Cone", ObjectType::CONE},
                                       {"三角錐", "Tetrahedron", ObjectType::TETRAHEDRON},
                                       {"三角柱", "Triangular Prism", ObjectType::TRIANGULAR_PRISM},
                                       {"平面", "Plane", ObjectType::PLANE}};
    const auto drawEntries = [&](const auto& entries) {
        for (const auto& entry : entries)
        {
            if (!ImGui::MenuItem(entry.Label))
            {
                continue;
            }
            auto entity = World::CreateEntity()
                              .Add<TransformComponent>()
                              .Add<MeshComponent>()
                              .Add<MaterialComponent>()
                              .Add<NameComponent>();
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
            if (entry.Type == ObjectType::CAPSULE)
            {
                settings.Rings = 8;
            }
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
    if (!state.Open)
    {
        return;
    }
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
        const bool rectangular =
            state.Type == ObjectType::CUBE || state.Type == ObjectType::QUAD || state.Type == ObjectType::PLANE;
        bool changed = false;
        if (rectangular || state.Type == ObjectType::TRIANGLE || state.Type == ObjectType::TETRAHEDRON ||
            state.Type == ObjectType::TRIANGULAR_PRISM)
        {
            changed |=
                ImGui::SliderInt("面の分割数", &settings.FaceDivisions, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
        }
        if (rectangular)
        {
            changed |=
                ImGui::DragFloat("幅", &settings.Width, 0.01f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        }
        else
        {
            changed |= ImGui::DragFloat("半径", &settings.Radius, 0.01f, 0.001f, 1000.0f, "%.3f",
                                        ImGuiSliderFlags_AlwaysClamp);
        }
        if (state.Type != ObjectType::CIRCLE && state.Type != ObjectType::TRIANGLE &&
            state.Type != ObjectType::SPHERE && state.Type != ObjectType::PLANE)
        {
            changed |= ImGui::DragFloat(state.Type == ObjectType::CAPSULE ? "円柱部分の高さ" : "高さ", &settings.Height,
                                        0.01f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        }
        if (state.Type == ObjectType::CUBE || state.Type == ObjectType::PLANE)
        {
            changed |= ImGui::DragFloat("奥行き", &settings.Depth, 0.01f, 0.001f, 1000.0f, "%.3f",
                                        ImGuiSliderFlags_AlwaysClamp);
        }
        if (state.Type == ObjectType::CIRCLE || state.Type == ObjectType::SPHERE || state.Type == ObjectType::CAPSULE ||
            state.Type == ObjectType::CYLINDER || state.Type == ObjectType::CONE)
        {
            changed |=
                ImGui::SliderInt("周方向の分割数", &settings.Segments, 3, 128, "%d", ImGuiSliderFlags_AlwaysClamp);
        }
        if (state.Type == ObjectType::SPHERE || state.Type == ObjectType::CAPSULE)
        {
            changed |= ImGui::SliderInt(state.Type == ObjectType::CAPSULE ? "半球の分割数" : "縦方向の分割数",
                                        &settings.Rings, 2, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
        }
        state.PendingUpdate |= changed;
        if (ImGui::Button("完了"))
        {
            state.Open = false;
        }
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
    SyncEntitySelection();
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
            ImGui::SetTooltip("%s", m_ScenePath.empty() ? "未保存のシーン（Ctrl+S で保存）"
                                                        : m_ScenePath.generic_string().c_str());
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
        SelectEntity(g_kINVALID_ENTITY);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ 作成"))
    {
        ImGui::OpenPopup("HierarchyCreatePopup");
    }
    if (ImGui::BeginPopup("HierarchyCreatePopup"))
    {
        if (ImGui::MenuItem("空のオブジェクト"))
        {
            auto group = World::CreateEntity().Add<TransformComponent>().Add<NameComponent>();
            group.SetName("GameObject");
            SelectEntity(group.GetID());
            EntitySnapshot missing{};
            missing.Entity = group.GetID();
            PushUndoSnapshot(missing);
        }
        DrawPrimitiveCreationMenu();
        if (ImGui::MenuItem("GameCamera"))
        {
            int cameraNumber = 1;
            for (EntityID candidate : World::GetView<CameraComponent>())
            {
                if (ComponentManager::GetComponentUnchecked<CameraComponent>(candidate).IsGameCamera)
                {
                    ++cameraNumber;
                }
            }
            m_SelectedEntity = Camera::CreateGameCamera("GameCamera " + to_string(cameraNumber), {5.0f, 4.0f, -10.0f},
                                                        {0.0f, 3.0f, 0.0f}, XM_PIDIV4,
                                                        Camera::GetGameCameraEntity() == g_kINVALID_ENTITY);
            AddLog("GameCamera追加: #%u", m_SelectedEntity);
        }
        if (ImGui::BeginMenu("ライト"))
        {
            if (ImGui::MenuItem("Directional"))
            {
                m_SelectedEntity = CreateLightEntity(LightType::Directional);
            }
            if (ImGui::MenuItem("Point"))
            {
                m_SelectedEntity = CreateLightEntity(LightType::Point);
            }
            if (ImGui::MenuItem("Rect Area"))
            {
                m_SelectedEntity = CreateLightEntity(LightType::Rect);
            }
            if (ImGui::MenuItem("Capsule"))
            {
                m_SelectedEntity = CreateLightEntity(LightType::Capsule);
            }
            if (ImGui::MenuItem("Spot"))
            {
                m_SelectedEntity = CreateLightEntity(LightType::Spot);
            }
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
    vector<EntityID> visible;
    vector<EntityID> all;
    unordered_map<EntityID, vector<EntityID>> children;
    for (EntityID entity : SceneHierarchy::Entities())
    {
        if (IsEditableEntity(entity))
        {
            all.push_back(entity);
        }
    }
    totalCount = static_cast<int>(all.size());
    for (EntityID entity : all)
    {
        EntityID parent = SceneHierarchy::Parent(entity);
        if (find(all.begin(), all.end(), parent) == all.end())
        {
            parent = g_kINVALID_ENTITY;
        }
        children[parent].push_back(entity);
    }
    EntityID clicked = g_kINVALID_ENTITY;
    static EntityID pendingClick = g_kINVALID_ENTITY;
    static bool selectionDragged = false;
    EntityID dropParent = g_kINVALID_ENTITY;
    bool reparentRequested = false;
    unordered_set<EntityID> drawn;
    std::function<void(EntityID)> drawEntity = [&](EntityID entity) {
        if (!drawn.insert(entity).second)
        {
            return;
        }
        const string displayName = GetEntityDisplayName(entity);
        if (entityFilter.IsActive() && !entityFilter.PassFilter(displayName.c_str()) && m_RenamingEntity != entity)
        {
            return;
        }
        visible.push_back(entity);
        ImGui::PushID(static_cast<int>(entity));
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (IsEntitySelected(entity))
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (children[entity].empty() || entityFilter.IsActive())
        {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        const bool prefab = GetPrefabRoot(entity) != g_kINVALID_ENTITY;
        if (prefab)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kPrefabColor);
        }
        const bool open = ImGui::TreeNodeEx("Entity", flags, "%s", displayName.c_str());
        if (prefab)
        {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            clicked = entity;
        }
        if (ImGui::BeginDragDropSource())
        {
            selectionDragged = true;
            if (!IsEntitySelected(entity))
            {
                SelectEntity(entity);
            }
            ImGui::SetDragDropPayload("SCENE_ENTITIES", &entity, sizeof(entity));
            ImGui::Text("%zu 個のオブジェクト", m_SelectedEntities.size());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (ImGui::AcceptDragDropPayload("SCENE_ENTITIES"))
            {
                dropParent = entity;
                reparentRequested = true;
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("EntityActions"))
        {
            if (!IsEntitySelected(entity))
            {
                SelectEntity(entity);
            }
            if (ImGui::MenuItem("名前を変更", "F2", false, m_SelectedEntities.size() == 1))
            {
                BeginRename(entity);
            }
            if (ImGui::MenuItem("複製", "Ctrl+D", false, m_SelectedEntities.size() == 1))
            {
                duplicateRequested = true;
            }
            if (ImGui::MenuItem("親を解除"))
            {
                reparentRequested = true;
            }
            if (ImGui::MenuItem("削除（子も含む）", "Delete"))
            {
                deleteRequested = true;
            }
            ImGui::EndPopup();
        }
        if (m_RenamingEntity == entity)
        {
            DrawRenameInput(entity);
        }
        if (open)
        {
            if (!entityFilter.IsActive())
            {
                for (EntityID child : children[entity])
                {
                    drawEntity(child);
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    if (entityFilter.IsActive())
    {
        for (EntityID entity : all)
        {
            drawEntity(entity);
        }
    }
    else
    {
        for (EntityID entity : children[g_kINVALID_ENTITY])
        {
            drawEntity(entity);
        }
    }
    visibleCount = static_cast<int>(visible.size());
    if (clicked != g_kINVALID_ENTITY)
    {
        if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && IsEntitySelected(clicked) &&
            m_SelectedEntities.size() > 1)
        {
            // Keep a multi-selection intact while the user starts dragging one of its rows.
            pendingClick = clicked;
            selectionDragged = false;
        }
        else if (ImGui::GetIO().KeyShift)
        {
            SelectEntityRange(clicked, visible);
        }
        else
        {
            SelectEntity(clicked, ImGui::GetIO().KeyCtrl);
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if (pendingClick != g_kINVALID_ENTITY && !selectionDragged && Registry::IsAlive(pendingClick))
        {
            SelectEntity(pendingClick);
        }
        pendingClick = g_kINVALID_ENTITY;
        selectionDragged = false;
    }
    if (hierarchyFocused && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_A, false) && !visible.empty())
    {
        m_SelectedEntities = visible;
        m_SelectedEntity = visible.back();
    }
    ImGui::Selectable("ここにドロップして親を解除", false);
    if (ImGui::BeginDragDropTarget())
    {
        if (ImGui::AcceptDragDropPayload("SCENE_ENTITIES"))
        {
            reparentRequested = true;
        }
        ImGui::EndDragDropTarget();
    }
    if (reparentRequested)
    {
        ReparentSelection(dropParent);
    }
    if (visibleCount == 0)
    {
        ImGui::TextWrapped("表示するオブジェクトがありません。検索条件を変更するか、+ 作成から追加してください。");
    }
    if (ImGui::BeginPopupContextWindow("HierarchyBackgroundActions",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::BeginMenu("オブジェクトの作成"))
        {
            DrawPrimitiveCreationMenu();
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    if (duplicateRequested)
    {
        DuplicateSelectedEntity();
    }
    if (deleteRequested)
    {
        DeleteSelectedEntity();
    }
    ImGui::TextDisabled("%d / %d 件 | %zu 選択", visibleCount, totalCount, m_SelectedEntities.size());
    ImGui::End();
}

bool ImGuiManager::IsEntitySelected(EntityID entity)
{
    return find(m_SelectedEntities.begin(), m_SelectedEntities.end(), entity) != m_SelectedEntities.end();
}

void ImGuiManager::SyncEntitySelection()
{
    if (!Registry::IsAlive(m_SelectedEntity))
    {
        m_SelectedEntity = g_kINVALID_ENTITY;
        m_SelectedEntities.clear();
        return;
    }
    // Existing creation/import commands still assign the active entity directly.
    if (!IsEntitySelected(m_SelectedEntity))
    {
        m_SelectedEntities = {m_SelectedEntity};
        m_EntitySelectionAnchor = m_SelectedEntity;
    }
    erase_if(m_SelectedEntities, [](EntityID entity) {
        return !Registry::IsAlive(entity);
    });
}

void ImGuiManager::SelectEntity(EntityID entity, bool toggle)
{
    FinalizeUndoCaptureIfIdle(true);
    SyncEntitySelection();
    if (!toggle)
    {
        m_SelectedEntities.clear();
    }
    if (toggle && IsEntitySelected(entity))
    {
        erase(m_SelectedEntities, entity);
        m_SelectedEntity = g_kINVALID_ENTITY;
        if (!m_SelectedEntities.empty())
        {
            m_SelectedEntity = m_SelectedEntities.back();
        }
    }
    else
    {
        if (Registry::IsAlive(entity))
        {
            m_SelectedEntities.push_back(entity);
        }
        m_SelectedEntity = entity;
    }
    m_EntitySelectionAnchor = entity;
}

void ImGuiManager::SelectEntityRange(EntityID entity, const vector<EntityID>& visible)
{
    const auto anchor = find(visible.begin(), visible.end(), m_EntitySelectionAnchor);
    const auto target = find(visible.begin(), visible.end(), entity);
    if (anchor == visible.end() || target == visible.end())
    {
        SelectEntity(entity, ImGui::GetIO().KeyCtrl);
        return;
    }
    FinalizeUndoCaptureIfIdle(true);
    if (!ImGui::GetIO().KeyCtrl)
    {
        m_SelectedEntities.clear();
    }
    const auto first = min(anchor, target);
    const auto last = max(anchor, target);
    for (auto it = first; it != last + 1; ++it)
    {
        if (!IsEntitySelected(*it))
        {
            m_SelectedEntities.push_back(*it);
        }
    }
    m_SelectedEntity = entity;
}

void ImGuiManager::ReparentSelection(EntityID parent)
{
    SyncEntitySelection();
    const auto roots = EntityHierarchy::SelectionRoots(m_SelectedEntities, SceneHierarchy::Parent);
    for (EntityID entity : roots)
    {
        if (!EntityHierarchy::CanReparent(entity, parent, SceneHierarchy::Parent))
        {
            AddLog("親子関係は設定できません: 自分自身や子孫を親にはできません");
            return;
        }
    }
    FinalizeUndoCaptureIfIdle(true);
    SceneHierarchy::Update();
    vector<EntitySnapshot> before;
    for (EntityID entity : roots)
    {
        if (SceneHierarchy::Parent(entity) != parent)
        {
            before.push_back(CaptureEntity(entity));
            SceneHierarchy::SetParent(entity, parent);
        }
    }
    PushUndoGroup(before);
}

void ImGuiManager::DrawParentSelector()
{
    const EntityID current = SceneHierarchy::Parent(m_SelectedEntity);
    bool mixed = false;
    for (EntityID entity : m_SelectedEntities)
    {
        mixed |= SceneHierarchy::Parent(entity) != current;
    }
    const char* label = "なし（ルート）";
    if (mixed)
    {
        label = "複数の親";
    }
    else if (current != g_kINVALID_ENTITY)
    {
        label = GetEntityDisplayName(current);
    }
    if (ImGui::BeginCombo("親", label))
    {
        if (ImGui::Selectable("なし（ルート）", !mixed && current == g_kINVALID_ENTITY))
        {
            ReparentSelection(g_kINVALID_ENTITY);
        }
        const auto roots = EntityHierarchy::SelectionRoots(m_SelectedEntities, SceneHierarchy::Parent);
        for (EntityID candidate : SceneHierarchy::Entities())
        {
            bool valid = IsEditableEntity(candidate);
            for (EntityID entity : roots)
            {
                valid &= EntityHierarchy::CanReparent(entity, candidate, SceneHierarchy::Parent);
            }
            if (!valid)
            {
                continue;
            }
            ImGui::PushID(static_cast<int>(candidate));
            if (ImGui::Selectable(GetEntityDisplayName(candidate), !mixed && candidate == current))
            {
                ReparentSelection(candidate);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void ImGuiManager::ApplySelectionTransform(const TransformComponent& before, const TransformComponent& after)
{
    SyncEntitySelection();
    // Property edits describe a delta in the displayed TRS frame. Apply that delta
    // to the exact affine matrix so moving a sheared child preserves its shape.
    const XMMATRIX original = TransformMath::World(before.Position, before.Rotation, before.Scale);
    if (std::abs(XMVectorGetX(XMMatrixDeterminant(original))) < 0.0000001f)
    {
        return;
    }
    const XMMATRIX delta =
        XMMatrixInverse(nullptr, original) * TransformMath::World(after.Position, after.Rotation, after.Scale);
    const auto roots = EntityHierarchy::SelectionRoots(m_SelectedEntities, SceneHierarchy::Parent);
    // Capture every root before changing any of them, including their descendants.
    for (EntityID entity : roots)
    {
        BeginUndoCapture(entity, CaptureEntity(entity));
    }
    for (EntityID entity : roots)
    {
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        TransformMath::SetWorld(transform, TransformMath::World(transform) * delta);
    }
    SceneHierarchy::Update();
    for (EntityID entity : EntityHierarchy::WithDescendants(roots, SceneHierarchy::Entities(), SceneHierarchy::Parent))
    {
        if (ComponentManager::HasComponent<SunComponent>(entity))
        {
            Sun::Sync(entity);
        }
        else
        {
            ApplyLightEntityToRuntime(entity);
        }
    }
}

void ImGuiManager::DeleteSelectedEntity()
{
    SyncEntitySelection();
    FinalizeUndoCaptureIfIdle(true);
    const auto entities =
        EntityHierarchy::WithDescendants(m_SelectedEntities, SceneHierarchy::Entities(), SceneHierarchy::Parent);
    vector<EntitySnapshot> before;
    for (EntityID entity : entities)
    {
        before.push_back(CaptureEntity(entity));
    }
    PushUndoGroup(before);
    for (EntityID entity : entities)
    {
        if (m_RenamingEntity == entity)
        {
            CancelRename();
        }
        World::DestroyEntity(Entity(entity));
    }
    SelectEntity(g_kINVALID_ENTITY);
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
                if (!isdigit(static_cast<unsigned char>(baseName[i])))
                {
                    digits = false;
                    break;
                }
            }
            if (digits)
            {
                baseName.erase(open);
            }
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
        if (usedNames.find(candidate) == usedNames.end())
        {
            return candidate;
        }
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

    if (GetPrefabRoot(source) == source)
    {
        const string asset = ComponentManager::GetComponentUnchecked<NameComponent>(source).PrefabAssetPath;
        const auto placement = ComponentManager::GetComponentUnchecked<TransformComponent>(source);
        return LoadPrefabInstance(filesystem::u8path(asset), g_kINVALID_ENTITY, &placement);
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
        copy.Name.PrefabAssetPath.clear();
        copy.Name.PrefabLocalId = g_kINVALID_ENTITY;
        copy.Name.IsPrefabRoot = false;
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
    outOrigin = XMVector3Unproject(XMVectorSet(sceneMouse.x, sceneMouse.y, 1.0f, 1.0f), 0.0f, 0.0f, m_SceneViewSize.x,
                                   m_SceneViewSize.y, 0.0f, 1.0f, proj, view, XMMatrixIdentity());
    XMVECTOR farPoint =
        XMVector3Unproject(XMVectorSet(sceneMouse.x, sceneMouse.y, 0.5f, 1.0f), 0.0f, 0.0f, m_SceneViewSize.x,
                           m_SceneViewSize.y, 0.0f, 1.0f, proj, view, XMMatrixIdentity());
    outDirection = XMVector3Normalize(XMVectorSubtract(farPoint, outOrigin));
    return true;
}

void ImGuiManager::PickEntityFromMouse()
{
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.KeyAlt || // Alt+左ドラッグはオービット操作
        !m_IsSceneViewHovered || ImGuizmo::IsOver() || ImGuizmo::IsUsing())
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

    XMVECTOR nearPoint = XMVector3Unproject(XMVectorSet(mouse.x, mouse.y, 1.0f, 1.0f), 0.0f, 0.0f, width, height, 0.0f,
                                            1.0f, proj, view, XMMatrixIdentity());
    XMVECTOR farPoint = XMVector3Unproject(XMVectorSet(mouse.x, mouse.y, 0.5f, 1.0f), 0.0f, 0.0f, width, height, 0.0f,
                                           1.0f, proj, view, XMMatrixIdentity());
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
            hits.push_back({entity, distance});
        }
    }

    sort(hits.begin(), hits.end(), [](const PickHit& lhs, const PickHit& rhs) {
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
        if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift)
        {
            SelectEntity(g_kINVALID_ENTITY);
        }
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

    SelectEntity(hits[pickIndex].Entity, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
    m_LastPickCandidates = move(candidates);
    m_LastPickMouse = mouse;
}

bool ImGuiManager::IsEditableEntity(EntityID entity)
{
    if (entity == Camera::GetEditorCameraEntity())
    {
        return false;
    }
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

    return ComponentManager::HasComponent<NameComponent>(entity);
}

void ImGuiManager::BeginRename(EntityID entity)
{
    if (entity == g_kINVALID_ENTITY || !Registry::IsAlive(entity) ||
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
    if (m_RenamingEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_RenamingEntity) ||
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

    char buffer[256]{};
    strncpy_s(buffer, m_RenameBuffer.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushID(static_cast<int>(entity));
    if (m_RenameNeedsFocus)
    {
        ImGui::SetKeyboardFocusHere();
        m_RenameNeedsFocus = false;
    }
    const bool submitted = ImGui::InputText("##Rename", buffer, IM_ARRAYSIZE(buffer),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
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
        case LightType::Directional:
            return "Directional";
        case LightType::Point:
            return "Point";
        case LightType::Spot:
            return "Spot";
        case LightType::Volume:
            return "Volume";
        case LightType::Rect:
            return "Rect";
        case LightType::Capsule:
            return "Capsule";
        default:
            return "Unknown";
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
    template <typename T> void VisitLiveComponent(SceneFile::Archive& archive, const char* key, EntityID entity)
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
        if (!drivenSun)
        {
            VisitLiveComponent<TransformComponent>(archive, "transform", entity);
        }
        VisitLiveComponent<ShaderComponent>(archive, "shader", entity);
        VisitLiveComponent<StaticModelComponent>(archive, "staticModel", entity);
        VisitLiveComponent<AnimationModelComponent>(archive, "animationModel", entity);
        if (!drivenSun)
        {
            VisitLiveComponent<LightComponent>(archive, "light", entity);
        }
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
        if (ComponentManager::HasComponent<MeshComponent>(entity) &&
            !ComponentManager::HasComponent<ProceduralTerrainComponent>(entity))
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
                        for (float value : {vertex.Position.x, vertex.Position.y, vertex.Position.z, vertex.Normal.x,
                                            vertex.Normal.y, vertex.Normal.z, vertex.TexCoord.x, vertex.TexCoord.y,
                                            vertex.Diffuse.x, vertex.Diffuse.y, vertex.Diffuse.z, vertex.Diffuse.w})
                        {
                            packed.push_back(value);
                        }
                    }
                    string encoded = SceneFile::EncodeBase64(packed.data(), packed.size() * sizeof(float));
                    archive.Object("meshVertices", [&](SceneFile::Archive& v) {
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
        if (text.empty())
        {
            return {};
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        wstring result(static_cast<size_t>(max(length, 0)), L'\0');
        if (length > 0)
        {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length);
        }
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
    if (RenderingValidation::IsEnabled())
    {
        return false;
    }
    char validationMode[8]{};
    return !(GetEnvironmentVariableA("DX12_VALIDATION_MODE", validationMode, sizeof(validationMode)) > 0 &&
             atoi(validationMode) != 0);
}

bool ImGuiManager::IsSceneFile(const filesystem::path& path)
{
    string extension = path.extension().generic_string();
    transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c));
    });
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
        if (entity != editorCamera)
        {
            entities.push_back(entity);
        }
    }
    return entities;
}

uint64_t ImGuiManager::ComputeSceneHash()
{
    uint64_t hash = 1469598103934665603ull;
    const vector<EntityID> entities = CollectSceneEntities();
    unordered_map<EntityID, EntityID> fileIds;
    fileIds.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i)
    {
        fileIds[entities[i]] = static_cast<EntityID>(i);
    }
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

namespace
{
    // 高速保存：Json::Value の木を作らず、文字列へ直接書き出す（Archive の Stream モード）
    string StreamSceneJson(const vector<EntityID>& entities, const unordered_map<EntityID, EntityID>& fileIds,
                           void (*visit)(SceneFile::Archive&, EntityID))
    {
        string out;
        out.reserve(entities.size() * 640 + 256);
        out.append("{\"format\":\"DX12Scene\",\"version\":");
        out.append(to_string(SceneFile::kVersion));
        out.append(",\"entities\":[");
        for (size_t index = 0; index < entities.size(); ++index)
        {
            if (index)
            {
                out.push_back(',');
            }
            out.append("{\"id\":");
            out.append(to_string(fileIds.at(entities[index])));
            string body;
            SceneFile::Archive archive(body);
            archive.EntityMap = &fileIds;
            visit(archive, entities[index]);
            if (!body.empty())
            {
                out.push_back(',');
                out.append(body);
            }
            out.push_back('}');
            out.push_back('\n'); // 1 エンティティ 1 行（差分ツールで見やすく）
        }
        out.append("]}\n");
        return out;
    }

    // ---- 決定論的な差分保存
    //   出力は「シーンの状態」だけで決まる（同じ状態なら毎回バイト単位で同じファイル。キャッシュの有無・スレッド数に依存しない）
    //   ・エンティティは EntityID 昇順、ファイル内 ID は 0 からの連番、キーは Visit の順、数値は最短往復表現
    //   ・エンティティごとの書き出し結果を、内容のハッシュ（参照先のファイル内 ID も含む）をキーに再利用する
    //   ・並列に作ってもエンティティの順に連結するので、出力は 1 スレッドと同じ
    //   ・昼夜サイクルが動かす太陽はハッシュが位置を見ないので、毎回書き出す
    struct EntityRecordCache
    {
        uint64_t Hash = 0;
        EntityID FileId = 0;
        bool Valid = false;
        string Body; // {"id":N, の後ろから } の手前まで（コンポーネント部分）
    };

    vector<EntityRecordCache> g_RecordCache;
    constexpr uint64_t kRecordFormat = 1; // 書き出し形式を変えたら上げる（古いキャッシュを使わない）

    bool IsDrivenSun(EntityID entity)
    {
        if (!ComponentManager::HasComponent<SunComponent>(entity))
        {
            return false;
        }
        const auto& cycle = DayNightCycle::GetSettings();
        return cycle.TimeDriven || cycle.AutomaticLighting;
    }

    uint64_t HashEntityRecord(EntityID entity, EntityID fileId, const unordered_map<EntityID, EntityID>& fileIds)
    {
        uint64_t hash = 1469598103934665603ull ^ (kRecordFormat * 0x9e3779b97f4a7c15ull);
        SceneFile::Archive archive(hash);
        archive.EntityMap = &fileIds;
        archive.HashBytes(&fileId, sizeof(fileId));
        VisitLiveEntity(archive, entity);
        return hash;
    }

    void StreamEntityBody(EntityID entity, const unordered_map<EntityID, EntityID>& fileIds, string& out)
    {
        out.clear();
        SceneFile::Archive archive(out);
        archive.EntityMap = &fileIds;
        VisitLiveEntity(archive, entity);
    }

    struct DeterministicSaveStats
    {
        size_t Reused = 0;
        size_t Written = 0;
    };

    string StreamSceneDeterministic(const vector<EntityID>& entities, const unordered_map<EntityID, EntityID>& fileIds,
                                    DeterministicSaveStats& stats)
    {
        // キャッシュは EntityID で引く（並列に書くので先に大きさを確保する）
        EntityID maxEntity = 0;
        for (EntityID entity : entities)
        {
            maxEntity = (std::max)(maxEntity, entity);
        }
        if (g_RecordCache.size() <= maxEntity)
        {
            g_RecordCache.resize(static_cast<size_t>(maxEntity) + 1);
        }

        // エンティティを塊に分けて並列に処理（各塊は自分の担当エンティティのキャッシュだけを読み書きする）
        const size_t count = entities.size();
        const size_t workers =
            (std::max)(size_t(1), (std::min)(size_t(std::thread::hardware_concurrency()), count / 256 + 1));
        vector<size_t> reused(workers, 0);
        auto work = [&](size_t worker) {
            const size_t first = count * worker / workers;
            const size_t last = count * (worker + 1) / workers;
            for (size_t index = first; index < last; ++index)
            {
                const EntityID entity = entities[index];
                const EntityID fileId = static_cast<EntityID>(index);
                auto& cache = g_RecordCache[entity];
                if (IsDrivenSun(entity))
                {
                    StreamEntityBody(entity, fileIds, cache.Body);
                    cache.Valid = false;
                    continue;
                }
                const uint64_t hash = HashEntityRecord(entity, fileId, fileIds);
                if (cache.Valid && cache.Hash == hash && cache.FileId == fileId)
                {
                    ++reused[worker];
                    continue;
                }
                StreamEntityBody(entity, fileIds, cache.Body);
                cache.Hash = hash;
                cache.FileId = fileId;
                cache.Valid = true;
            }
        };
        vector<std::future<void>> tasks;
        for (size_t worker = 1; worker < workers; ++worker)
        {
            tasks.push_back(std::async(std::launch::async, work, worker));
        }
        work(0);
        for (auto& task : tasks)
        {
            task.get();
        }
        for (size_t value : reused)
        {
            stats.Reused += value;
        }
        stats.Written = count - stats.Reused;

        // エンティティの順に連結（ここが出力の順序を決める唯一の場所）
        size_t total = 64;
        for (EntityID entity : entities)
        {
            total += g_RecordCache[entity].Body.size() + 24;
        }
        string out;
        out.reserve(total);
        out.append("{\"format\":\"DX12Scene\",\"version\":");
        out.append(to_string(SceneFile::kVersion));
        out.append(",\"entities\":[");
        for (size_t index = 0; index < count; ++index)
        {
            if (index)
            {
                out.push_back(',');
            }
            out.append("{\"id\":");
            out.append(to_string(index));
            const string& body = g_RecordCache[entities[index]].Body;
            if (!body.empty())
            {
                out.push_back(',');
                out.append(body);
            }
            out.push_back('}');
            out.push_back('\n');
        }
        out.append("]}\n");
        return out;
    }

    // ---- 非同期保存（文字列化 + 一時ファイル + 置き換え）
    struct SaveResult
    {
        bool Success = false;
        string Path;
        double Milliseconds = 0.0;
    };

    std::future<SaveResult> g_PendingSave;
    std::mutex g_SaveFileMutex; // 同じファイルへの書き込みを直列化

    // 変更判定のハッシュを複数フレームに分けて計算する（以前は 0.25 秒ごとに全エンティティを 1 フレームで処理し、
    // 数千オブジェクトのシーンで定期的にフレームが引っかかっていた）
    struct IncrementalSceneHash
    {
        vector<EntityID> Entities;
        unordered_map<EntityID, EntityID> FileIds;
        uint64_t Hash = 0;
        size_t Next = 0;
        bool Active = false;
    };

    IncrementalSceneHash g_TrackingHash;
    bool g_SavedHashPending = false;
    uint64_t g_SavedHashRevision = 0;
    constexpr size_t kHashEntitiesPerFrame = 768;

    SaveResult WriteSceneFile(string contents, filesystem::path path, string displayPath)
    {
        const auto begin = std::chrono::steady_clock::now();
        SaveResult result;
        result.Path = std::move(displayPath);
        std::lock_guard<std::mutex> lock(g_SaveFileMutex);
        error_code error;
        filesystem::path temporary = path;
        temporary += L".tmp";
        {
            ofstream file(temporary, ios::binary | ios::trunc);
            file.write(contents.data(), static_cast<streamsize>(contents.size()));
            if (!file)
            {
                return result;
            }
        }
        // 置き換えは原子的（途中で落ちても古いファイルか新しいファイルのどちらかが残る）。
        // WRITE_THROUGH（ディスクへの物理書き出し待ち）は数百 ms〜秒かかるので使わない
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            filesystem::remove(temporary, error);
            return result;
        }
        result.Success = true;
        result.Milliseconds = std::chrono::duration<double, milli>(std::chrono::steady_clock::now() - begin).count();
        return result;
    }

    void VisitLiveEntityThunk(SceneFile::Archive& archive, EntityID entity)
    {
        VisitLiveEntity(archive, entity);
    }
}

string ImGuiManager::SerializeScene()
{
    const vector<EntityID> entities = CollectSceneEntities();
    unordered_map<EntityID, EntityID> fileIds;
    fileIds.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i)
    {
        fileIds[entities[i]] = static_cast<EntityID>(i);
    }
    return StreamSceneJson(entities, fileIds, VisitLiveEntityThunk);
}

// 保存中の非同期書き込みが終わっていればログを出す（毎フレーム）。wait = true なら終わるまで待つ
void ImGuiManager::PollPendingSceneSave(bool wait)
{
    if (!g_PendingSave.valid())
    {
        return;
    }
    if (!wait && g_PendingSave.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return;
    }
    const SaveResult result = g_PendingSave.get();
    if (result.Success)
    {
        AddLog("シーンを保存: %s（書き込み %.0f ms・バックグラウンド）", result.Path.c_str(), result.Milliseconds);
    }
    else
    {
        AddLog("シーンの保存に失敗しました: %s", result.Path.c_str());
        m_SceneSavedHash = 0; // 未保存（*）に戻す
    }
}

bool ImGuiManager::SaveScene(const filesystem::path& path)
{
    if (ProjectManager::IsPlaying())
    {
        AddLog("再生中はシーンを保存できません。停止してから保存してください");
        return false;
    }
    // 負荷削減：メインスレッドでは ECS の読み取り（JSON の木を作る）と変更判定のハッシュだけを行い、
    // 文字列化・ファイル書き込み・ディスクへの書き出し待ちは別スレッドで行う（以前は全部メインスレッドで数秒止まっていた）
    const auto begin = std::chrono::steady_clock::now();
    PollPendingSceneSave(true); // 前の保存が残っていれば先に終わらせる（順序を守る）
    error_code error;
    if (path.has_parent_path())
    {
        filesystem::create_directories(path.parent_path(), error);
    }
    const vector<EntityID> entities = CollectSceneEntities();
    unordered_map<EntityID, EntityID> fileIds;
    fileIds.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i)
    {
        fileIds[entities[i]] = static_cast<EntityID>(i);
    }
    DeterministicSaveStats saveStats;
    string contents = StreamSceneDeterministic(entities, fileIds, saveStats);
    // 検証（環境変数 DX12_VERIFY_SAVE=1）：キャッシュを使わない書き出しとバイト単位で比較
    char verify[8]{};
    if (GetEnvironmentVariableA("DX12_VERIFY_SAVE", verify, sizeof(verify)) > 0 && atoi(verify) != 0)
    {
        const string reference = StreamSceneJson(entities, fileIds, VisitLiveEntityThunk);
        AddLog(reference == contents ? "保存の検証: 一致（%zu bytes）" : "保存の検証: 不一致！（%zu bytes）",
               contents.size());
    }
    m_ScenePath = RelativeToWorkingDirectory(path);
    g_PendingSave = std::async(std::launch::async, WriteSceneFile, std::move(contents), path, ToUtf8(m_ScenePath));
    // 変更判定の基準は、保存直後から始める分割ハッシュの結果を使う（ここで全エンティティをハッシュしない）。
    // 結果が出るまでは「保存済み」と表示し、その間に編集されたら基準を更新しない（= 未保存 * に戻る）
    g_SavedHashPending = true;
    g_SavedHashRevision = m_EditRevision;
    g_TrackingHash.Active = false;
    m_SceneNextHashTime = 0.0;
    m_SceneSavedHash = m_SceneCurrentHash;
    SaveProjectSettings(); // cfg の scene= を更新
    AddLog("シーンを保存中: %s（メインスレッド %.0f ms・再利用 %zu / 書き出し %zu）", ToUtf8(m_ScenePath).c_str(),
           std::chrono::duration<double, milli>(std::chrono::steady_clock::now() - begin).count(), saveStats.Reused,
           saveStats.Written);
    return true;
}

void ImGuiManager::DestroySceneEntities()
{
    PollPendingSceneSave(true);    // 書き込み中の保存を終わらせてから
    g_TrackingHash.Active = false; // 途中の変更判定は捨てる（エンティティが入れ替わるため）
    g_RecordCache.clear();         // 別のシーンの書き出し結果は使わない（ハッシュで弾かれるが、メモリを解放する）
    GraphicsDevice::WaitForGpuIdle();
    if (ProjectManager::IsPlaying())
    {
        ProjectManager::Stop();
    }
    CancelRename();
    m_SelectedEntity = g_kINVALID_ENTITY;
    m_LastPickCandidates.clear();
    m_SelectedEntities.clear();
    m_EntitySelectionAnchor = g_kINVALID_ENTITY;
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

void ImGuiManager::RestoreLoadedEntity(EntityID entity, Json::Value& record,
                                       const unordered_map<EntityID, EntityID>& entityMap)
{
    EntitySnapshot snapshot{};
    snapshot.Entity = entity;
    snapshot.WasAlive = true;
    SceneFile::Archive archive(SceneFile::Mode::Read, record);
    archive.EntityMap = &entityMap;
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
    RebuildLoadedEntity(entity, record);
}

void ImGuiManager::RebuildLoadedEntity(EntityID entity, const Json::Value& record)
{

    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
    {
        auto& model = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
        model.ModelId =
            model.ModelPath.empty() ? -1 : ModelManager::LoadStaticModel(model.ModelPath.c_str(), model.IsConvert);
        if (model.ModelId < 0)
        {
            AddLog("シーン: モデルを読み込めません: %s", model.ModelPath.c_str());
        }
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
        animation.ModelId = animation.ModelPath.empty()
                                ? -1
                                : ModelManager::LoadAnimModel(animation.ModelPath.c_str(), animation.IsConvert);
        if (animation.ModelId < 0)
        {
            AddLog("シーン: アニメーションモデルを読み込めません: %s", animation.ModelPath.c_str());
        }
        else
        {
            const size_t count = min(animation.AnimationPaths.size(), animation.Animations.size());
            for (size_t i = 0; i < count; ++i)
            {
                if (!ModelManager::LoadAnimation(animation.ModelId, animation.AnimationPaths[i].c_str(),
                                                 animation.Animations[i].c_str()))
                {
                    AddLog("シーン: アニメーションを読み込めません: %s", animation.AnimationPaths[i].c_str());
                }
            }
            const bool playing = animation.IsPlaying;
            vector<string> layers;
            for (const auto& layer : animation.ActiveAnimationLayers)
            {
                layers.push_back(layer.AnimationName);
            }
            if (playing && !layers.empty())
            {
                Animator::Play(animation, layers);
            }
            else if (playing && !animation.CurrentAnimation.empty())
            {
                Animator::Play(animation, animation.CurrentAnimation);
            }
        }
    }

    if (ComponentManager::HasComponent<MeshComponent>(entity))
    {
        auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (ComponentManager::HasComponent<ProceduralTerrainComponent>(entity))
        {
            const auto settings = ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(entity).Settings;
            if (ProceduralScene::GenerateTerrain(settings, entity) == g_kINVALID_ENTITY)
            {
                AddLog("シーン: 地形を生成できません");
            }
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
                    vertices[i].Pos = {f[0], f[1], f[2]};
                    vertices[i].Normal = {f[3], f[4], f[5]};
                    vertices[i].Tex = {f[6], f[7]};
                    vertices[i].Color = {f[8], f[9], f[10], f[11]};
                }
                if (!PrimitiveGeometry::UploadVertices(entity, vertices))
                {
                    AddLog("シーン: メッシュを復元できません #%u", entity);
                }
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
        if (!material.TexturePath.empty())
        {
            material.TextureID = TextureManager::LoadTexture(material.TexturePath.c_str());
        }
        else if (material.UseTexture)
        {
            material.TextureID = TextureManager::GetDefaultTextureIndex();
        }
        if (!material.NormalMapPath.empty())
        {
            material.NormalMapID = TextureManager::LoadNormalTexture(material.NormalMapPath.c_str());
        }
        if (!material.MeshShadingOverrides.empty())
        {
            ApplyMeshShadingOverridesToModel(entity);
        }
    }

    if (ComponentManager::HasComponent<CameraComponent>(entity))
    {
        const auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
        if (camera.IsGameCamera && camera.IsMainGameCamera)
        {
            Camera::SetMainGameCamera(entity);
        }
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

    // 現在のシーンを破棄する前にエンティティ表全体を検証する。ファイル ID が重複していたり
    // 不正だったりすると参照の再割り当てが曖昧になり、従来は ModelTest の
    // 実行時リソースが一部だけ再構築された状態で残ることがあった。
    unordered_set<EntityID> fileIds;
    fileIds.reserve(records.size());
    for (Json::ArrayIndex i = 0; i < records.size(); ++i)
    {
        if (!records[i].isObject())
        {
            AddLog("シーンファイルが不正です: entities[%u] がオブジェクトではありません", i);
            return false;
        }
        const Json::Value& id = records[i]["id"];
        if (!id.isNull() && !id.isUInt())
        {
            AddLog("シーンファイルが不正です: entities[%u].id が不正です", i);
            return false;
        }
        const EntityID fileId = id.isUInt() ? id.asUInt() : static_cast<EntityID>(i);
        if (fileId == g_kINVALID_ENTITY || !fileIds.insert(fileId).second)
        {
            AddLog("シーンファイルが不正です: entity id %u が無効または重複しています", fileId);
            return false;
        }
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
        if (entity == g_kINVALID_ENTITY)
        {
            continue;
        }
        try
        {
            Json::Value record = records[i];
            RestoreLoadedEntity(entity, record, idMap);
        }
        catch (const std::exception& error)
        {
            AddLog("シーン: entity #%u の復元を中止しました: %s", entity, error.what());
            World::DestroyEntity(Entity(entity));
            created[i] = g_kINVALID_ENTITY;
        }
        catch (...)
        {
            AddLog("シーン: entity #%u の復元中に不明な例外が発生しました", entity);
            World::DestroyEntity(Entity(entity));
            created[i] = g_kINVALID_ENTITY;
        }
    }

    // アセットブラウザの「シーン作成」で作った空シーン：新規シーンと同じ既定のカメラと太陽を置く
    if (records.size() == 0 && root.get("createDefaults", false).asBool())
    {
        Camera::CreateGameCamera("Main Camera", {0.0f, 1.0f, -10.0f}, {0.0f, 1.0f, 0.0f}, XM_PIDIV4, true);
        Sun::CreateDefault();
    }

    if (auto* physics = SystemManager::GetSystem<PhysicsSystem>())
    {
        physics->ResetSimulation();
    }
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
    Camera::CreateGameCamera("Main Camera", {0.0f, 1.0f, -10.0f}, {0.0f, 1.0f, 0.0f}, XM_PIDIV4, true);
    Sun::CreateDefault();
    if (auto* physics = SystemManager::GetSystem<PhysicsSystem>())
    {
        physics->ResetSimulation();
    }
    MaterialManager::NotifyAssignmentsChanged();
    m_ScenePath.clear();
    BeginSceneBaseline(1.5);
    SaveProjectSettings();
    AddLog("新しいシーンを作成しました");
}

void ImGuiManager::BeginSceneBaseline(double settleSeconds)
{
    m_SceneTracking = SceneTrackingAllowed();
    if (!m_SceneTracking)
    {
        return;
    }
    m_SceneSavedHash = ComputeSceneHash();
    m_SceneCurrentHash = m_SceneSavedHash;
    m_SceneSettleUntil = ImGui::GetTime() + settleSeconds;
    m_SceneSettleRevision = m_EditRevision;
    m_SceneNextHashTime = 0.0;
}

// 毎フレーム（Update の最後）。0.25秒ごとにハッシュを取り直す。

void ImGuiManager::UpdateSceneTracking()
{
    PollPendingSceneSave(false);
    if (m_SceneTracking && !ProjectManager::IsPlaying())
    {
        const double now = ImGui::GetTime();
        auto& tracking = g_TrackingHash;
        if (!tracking.Active && now >= m_SceneNextHashTime)
        {
            // 新しい周回を始める（ComputeSceneHash と同じ順序・同じ内容）
            tracking.Entities = CollectSceneEntities();
            tracking.FileIds.clear();
            tracking.FileIds.reserve(tracking.Entities.size());
            for (size_t i = 0; i < tracking.Entities.size(); ++i)
            {
                tracking.FileIds[tracking.Entities[i]] = static_cast<EntityID>(i);
            }
            tracking.Hash = 1469598103934665603ull;
            SceneFile::Archive start(tracking.Hash);
            const uint64_t count = tracking.Entities.size();
            start.HashBytes(&count, sizeof(count));
            tracking.Next = 0;
            tracking.Active = true;
        }
        bool completed = false;
        if (tracking.Active)
        {
            SceneFile::Archive archive(tracking.Hash);
            archive.EntityMap = &tracking.FileIds;
            const size_t end = (std::min)(tracking.Entities.size(), tracking.Next + kHashEntitiesPerFrame);
            for (; tracking.Next < end; ++tracking.Next)
            {
                const EntityID entity = tracking.Entities[tracking.Next];
                if (Registry::IsAlive(entity))
                {
                    VisitLiveEntity(archive, entity);
                }
            }
            if (tracking.Next >= tracking.Entities.size())
            {
                tracking.Active = false;
                completed = true;
                m_SceneNextHashTime = now + 0.25;
            }
        }
        if (completed)
        {
            m_SceneCurrentHash = tracking.Hash;
            if (g_SavedHashPending)
            {
                // 保存後、編集が無ければこの値が保存した内容のハッシュ
                if (m_EditRevision == g_SavedHashRevision)
                {
                    m_SceneSavedHash = tracking.Hash;
                }
                g_SavedHashPending = false;
            }
            // 読み込み直後はモデルの非同期読み込みや太陽の同期で値が落ち着くまで、ユーザーの編集がなければ基準を更新する
            if (now < m_SceneSettleUntil && m_EditRevision == m_SceneSettleRevision && !ImGui::IsAnyItemActive())
            {
                m_SceneSavedHash = m_SceneCurrentHash;
            }
        }
    }

    // ウィンドウタイトル：「DirectX12Window - シーン名*」
    string title = "DirectX12Window - " + GetSceneDisplayName() + (IsSceneDirty() ? "*" : "");
    if (ProjectManager::IsPlaying())
    {
        title += "  [再生中]";
    }
    const wstring wideTitle = Utf8ToWide(title);
    if (wideTitle != m_LastWindowTitle)
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        HWND hwnd = viewport ? static_cast<HWND>(viewport->PlatformHandleRaw) : nullptr;
        if (!hwnd && viewport)
        {
            hwnd = static_cast<HWND>(viewport->PlatformHandle);
        }
        if (hwnd && SetWindowTextW(hwnd, wideTitle.c_str()))
        {
            m_LastWindowTitle = wideTitle;
        }
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
    if (!m_ExecuteSceneAction)
    {
        return;
    }
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
    if (ImGui::MenuItem("新規シーン", "Ctrl+N"))
    {
        RequestNewScene();
    }
    if (ImGui::BeginMenu("シーンを開く"))
    {
        // asset 以下の .scene を一覧表示（メニューを開いている間だけ走査）
        vector<filesystem::path> scenes;
        error_code error;
        for (filesystem::recursive_directory_iterator
                 it(m_AssetRoot, filesystem::directory_options::skip_permission_denied, error),
             end;
             !error && it != end && scenes.size() < 200; it.increment(error))
        {
            if (it->is_regular_file(error) && IsSceneFile(it->path()))
            {
                scenes.push_back(it->path());
            }
        }
        sort(scenes.begin(), scenes.end());
        if (scenes.empty())
        {
            ImGui::TextDisabled("asset フォルダに .scene がありません");
        }
        for (const auto& scene : scenes)
        {
            const string label = ToUtf8(scene.lexically_relative(m_AssetRoot));
            const bool current = !m_ScenePath.empty() && filesystem::equivalent(scene, m_ScenePath, error);
            if (ImGui::MenuItem(label.c_str(), nullptr, current))
            {
                RequestOpenScene(scene);
            }
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("保存", "Ctrl+S", false, !playing))
    {
        SaveCurrentScene(false);
    }
    if (ImGui::MenuItem("名前を付けて保存...", "Ctrl+Shift+S", false, !playing))
    {
        SaveCurrentScene(true);
    }
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
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        const bool enter =
            ImGui::InputText("ファイル名", m_SaveAsName, sizeof(m_SaveAsName), ImGuiInputTextFlags_EnterReturnsTrue);
        string name = m_SaveAsName;
        while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        {
            name.pop_back();
        }
        filesystem::path target = filesystem::u8path(m_SaveAsDirectory) / filesystem::u8path(name);
        if (!IsSceneFile(target))
        {
            target += SceneFile::kExtension;
        }
        const bool validName = !name.empty() && name.find_first_of("\\/:*?\"<>|") == string::npos;
        error_code error;
        const bool exists = validName && filesystem::exists(target, error);
        ImGui::TextDisabled("保存先: %s", ToUtf8(target).c_str());
        if (!validName)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "ファイル名に使えない文字が含まれています");
        }
        else if (exists)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "同じ名前のシーンがあります。上書きします");
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!validName);
        if (ImGui::Button(exists ? "上書き保存" : "保存", ImVec2(120.0f, 0.0f)) || (enter && validName))
        {
            if (SaveScene(target))
            {
                ImGui::CloseCurrentPopup();
                if (m_ContinueAfterSaveAs)
                {
                    m_ExecuteSceneAction = true;
                }
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

bool ImGuiManager::IsPrefabFile(const filesystem::path& path)
{
    string extension = path.extension().generic_string();
    transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(tolower(character));
    });
    return extension == ".prefab";
}

EntityID ImGuiManager::GetPrefabRoot(EntityID entity)
{
    unordered_set<EntityID> visited;
    while (entity != g_kINVALID_ENTITY && visited.insert(entity).second)
    {
        if (ComponentManager::HasComponent<NameComponent>(entity))
        {
            const auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
            if (name.IsPrefabRoot && !name.PrefabAssetPath.empty())
            {
                return entity;
            }
        }
        entity = SceneHierarchy::Parent(entity);
    }
    return g_kINVALID_ENTITY;
}

bool ImGuiManager::SavePrefabAsset(EntityID root, const filesystem::path& path)
{
    if (!SceneHierarchy::HasTransform(root) || !IsInsideAssetRoot(path))
    {
        return false;
    }
    FinalizeUndoCaptureIfIdle(true);
    SceneHierarchy::Update();
    const auto entities = EntityHierarchy::WithDescendants({root}, SceneHierarchy::Entities(), SceneHierarchy::Parent);
    const string source = MakeRelativeAssetPath(path);
    unordered_map<EntityID, EntityID> fileIds{{root, 0}};
    unordered_set<EntityID> used{0};
    for (EntityID entity : entities)
    {
        if (entity == root || !ComponentManager::HasComponent<NameComponent>(entity))
        {
            continue;
        }
        const auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
        if (name.PrefabAssetPath == source && name.PrefabLocalId != g_kINVALID_ENTITY &&
            used.insert(name.PrefabLocalId).second)
        {
            fileIds[entity] = name.PrefabLocalId;
        }
    }
    EntityID next = 1;
    for (EntityID entity : entities)
    {
        if (!fileIds.count(entity))
        {
            while (used.count(next))
            {
                ++next;
            }
            fileIds[entity] = next;
            used.insert(next++);
        }
    }
    Json::Value prefab;
    Json::CharReaderBuilder reader;
    string error;
    istringstream contents(StreamSceneJson(entities, fileIds, VisitLiveEntityThunk));
    if (!Json::parseFromStream(reader, contents, &prefab, &error))
    {
        AddLog("プレハブ作成失敗: %s", error.c_str());
        return false;
    }
    prefab["format"] = "DX12Prefab";
    prefab["root"] = 0;
    for (auto& record : prefab["entities"])
    {
        record["name"]["prefabAsset"] = "";
        record["name"]["prefabLocalId"] = Json::UInt(g_kINVALID_ENTITY);
        record["name"]["prefabRoot"] = false;
    }
    if (!PrefabFormat::Validate(prefab, error))
    {
        AddLog("プレハブ作成失敗: %s", error.c_str());
        return false;
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const auto result = WriteSceneFile(Json::writeString(writer, prefab), path, source);
    if (!result.Success)
    {
        AddLog("プレハブを書き込めません: %s", source.c_str());
        return false;
    }
    vector<EntitySnapshot> before;
    for (EntityID entity : entities)
    {
        before.push_back(CaptureEntity(entity));
        if (!ComponentManager::HasComponent<NameComponent>(entity))
        {
            ComponentManager::AddComponent(entity, ComponentType::NAME);
        }
        auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
        name.PrefabAssetPath = source;
        name.PrefabLocalId = fileIds.at(entity);
        name.IsPrefabRoot = entity == root;
    }
    // Apply to other linked instances while retaining their placement in the scene.
    vector<EntityID> instances;
    for (EntityID entity : SceneHierarchy::Entities())
    {
        if (entity != root && GetPrefabRoot(entity) == entity &&
            ComponentManager::GetComponentUnchecked<NameComponent>(entity).PrefabAssetPath == source)
        {
            instances.push_back(entity);
        }
    }
    for (EntityID instance : instances)
    {
        LoadPrefabInstance(path, instance, nullptr, nullptr, &before);
    }
    PushUndoGroup(before);
    SelectEntity(root);
    m_AssetBrowserRescan = true;
    AddLog("プレハブ保存: %s", source.c_str());
    return true;
}

EntityID ImGuiManager::LoadPrefabInstance(const filesystem::path& path, EntityID existing,
                                          const TransformComponent* placement, const XMFLOAT3* position,
                                          vector<EntitySnapshot>* history)
{
    Json::Value prefab;
    Json::CharReaderBuilder reader;
    string error;
    ifstream stream(path, ios::binary);
    if (!stream || !Json::parseFromStream(reader, stream, &prefab, &error) || !PrefabFormat::Validate(prefab, error))
    {
        AddLog("プレハブを読み込めません: %s %s", ToUtf8(path).c_str(), error.c_str());
        return g_kINVALID_ENTITY;
    }
    const auto& records = prefab["entities"];
    if (SceneHierarchy::Entities().size() + records.size() > g_kMAX_ENTITIES)
    {
        AddLog("プレハブ配置失敗: オブジェクト数が上限を超えます");
        return g_kINVALID_ENTITY;
    }
    if (!GraphicsDevice::WaitForGpuIdle())
    {
        return g_kINVALID_ENTITY;
    }
    FinalizeUndoCaptureIfIdle(true);
    SceneHierarchy::Update();
    const string source = MakeRelativeAssetPath(path);
    const EntityID rootFileId = prefab["root"].asUInt();
    vector<EntityID> oldEntities;
    vector<EntitySnapshot> before;
    unordered_map<EntityID, EntityID> entityMap;
    TransformComponent desired;
    bool keepPlacement = placement != nullptr;
    if (placement)
    {
        desired = *placement;
    }
    if (SceneHierarchy::HasTransform(existing))
    {
        desired = ComponentManager::GetComponentUnchecked<TransformComponent>(existing);
        keepPlacement = true;
        oldEntities = EntityHierarchy::WithDescendants({existing}, SceneHierarchy::Entities(), SceneHierarchy::Parent);
        entityMap[rootFileId] = existing;
        for (EntityID entity : oldEntities)
        {
            before.push_back(CaptureEntity(entity));
            if (entity != existing && ComponentManager::HasComponent<NameComponent>(entity))
            {
                const auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
                if (name.PrefabAssetPath == source && name.PrefabLocalId != g_kINVALID_ENTITY)
                {
                    entityMap.emplace(name.PrefabLocalId, entity);
                }
            }
        }
    }
    vector<EntityID> loaded;
    for (const auto& record : records)
    {
        const EntityID fileId = record["id"].asUInt();
        if (!entityMap.count(fileId))
        {
            const EntityID created = World::CreateEntity().GetID();
            if (created == g_kINVALID_ENTITY)
            {
                for (const auto& snapshot : before)
                {
                    if (!snapshot.WasAlive)
                    {
                        World::DestroyEntity(Entity(snapshot.Entity));
                    }
                }
                return g_kINVALID_ENTITY;
            }
            entityMap[fileId] = created;
            EntitySnapshot missing{};
            missing.Entity = created;
            before.push_back(missing);
        }
        loaded.push_back(entityMap.at(fileId));
    }
    for (EntityID entity : oldEntities)
    {
        World::DestroyEntity(Entity(entity));
    }
    try
    {
        for (auto& record : prefab["entities"])
        {
            const EntityID fileId = record["id"].asUInt();
            const EntityID entity = entityMap.at(fileId);
            RestoreLoadedEntity(entity, record, entityMap);
            if (!ComponentManager::HasComponent<NameComponent>(entity))
            {
                ComponentManager::AddComponent(entity, ComponentType::NAME);
            }
            auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
            name.PrefabAssetPath = source;
            name.PrefabLocalId = fileId;
            name.IsPrefabRoot = fileId == rootFileId;
        }
        const EntityID root = entityMap.at(rootFileId);
        if (position)
        {
            desired = ComponentManager::GetComponentUnchecked<TransformComponent>(root);
            desired.Position = *position;
            keepPlacement = true;
        }
        if (keepPlacement)
        {
            const auto original = ComponentManager::GetComponentUnchecked<TransformComponent>(root);
            const XMMATRIX originalWorld = TransformMath::World(original);
            if (std::abs(XMVectorGetX(XMMatrixDeterminant(originalWorld))) < 0.0000001f)
            {
                throw std::runtime_error("プレハブのルートのスケールが 0 です");
            }
            const XMMATRIX delta = XMMatrixInverse(nullptr, originalWorld) * TransformMath::World(desired);
            for (EntityID entity : loaded)
            {
                auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
                if (!TransformMath::SetWorld(transform, TransformMath::World(transform) * delta))
                {
                    throw std::runtime_error("プレハブの変形を復元できません");
                }
            }
            SceneHierarchy::SetParent(root, desired.Parent);
        }
        for (EntityID entity : loaded)
        {
            SceneHierarchy::ResetParentFrame(entity);
        }
        SceneHierarchy::Update();
        if (history)
        {
            history->insert(history->end(), before.begin(), before.end());
        }
        else
        {
            PushUndoGroup(before);
        }
        SelectEntity(root);
        MaterialManager::NotifyAssignmentsChanged();
        AddLog("プレハブ配置 / 更新: %s", source.c_str());
        return root;
    }
    catch (const std::exception& exception)
    {
        for (EntityID entity : loaded)
        {
            World::DestroyEntity(Entity(entity));
        }
        RestoreHistoryGroup(before);
        AddLog("プレハブ復元失敗: %s", exception.what());
        return g_kINVALID_ENTITY;
    }
}

bool ImGuiManager::DrawPrefabInspector()
{
    const EntityID root = GetPrefabRoot(m_SelectedEntity);
    if (root != g_kINVALID_ENTITY)
    {
        const string source = ComponentManager::GetComponentUnchecked<NameComponent>(root).PrefabAssetPath;
        ImGui::TextColored(kPrefabColor, "プレハブ: %s", source.c_str());
        ImGui::TextWrapped("適用すると、同じプレハブの全インスタンスを更新します。各インスタンスの配置は保持します。");
        if (ImGui::Button("プレハブに適用"))
        {
            SavePrefabAsset(root, filesystem::u8path(source));
            return true;
        }
        ImGui::SameLine();
        if (ImGui::Button("元に戻す"))
        {
            LoadPrefabInstance(filesystem::u8path(source), root);
            return true;
        }
        if (ImGui::Button("プレハブのリンクを解除"))
        {
            FinalizeUndoCaptureIfIdle(true);
            vector<EntitySnapshot> before;
            for (EntityID entity :
                 EntityHierarchy::WithDescendants({root}, SceneHierarchy::Entities(), SceneHierarchy::Parent))
            {
                if (ComponentManager::HasComponent<NameComponent>(entity))
                {
                    before.push_back(CaptureEntity(entity));
                    auto& name = ComponentManager::GetComponentUnchecked<NameComponent>(entity);
                    name.PrefabAssetPath.clear();
                    name.PrefabLocalId = g_kINVALID_ENTITY;
                    name.IsPrefabRoot = false;
                }
            }
            PushUndoGroup(before);
        }
    }
    const auto roots = EntityHierarchy::SelectionRoots(m_SelectedEntities, SceneHierarchy::Parent);
    ImGui::BeginDisabled(roots.size() != 1);
    if (ImGui::Button("選択からプレハブを作成"))
    {
        string name = GetEntityDisplayName(roots.front());
        for (char& character : name)
        {
            if (static_cast<unsigned char>(character) < 32 || string("<>:\"/\\|?*").find(character) != string::npos)
            {
                character = '_';
            }
        }
        auto directory = m_CurrentAssetDirectory;
        error_code error;
        if (!filesystem::is_directory(directory, error) || !IsInsideAssetRoot(directory))
        {
            directory = m_AssetRoot;
        }
        const auto target = MakeUniqueAssetPath(directory, filesystem::u8path(name), ".prefab");
        ImGui::EndDisabled();
        SavePrefabAsset(roots.front(), target);
        return true;
    }
    ImGui::EndDisabled();
    return false;
}

void ImGuiManager::ValidateEditorObjects()
{
    static bool checked = false;
    char enabled[8]{};
    if (checked || !GetEnvironmentVariableA("DX12_EDITOR_OBJECT_TEST", enabled, sizeof(enabled)) || atoi(enabled) == 0)
    {
        return;
    }
    checked = true;
    const auto previousSelection = m_SelectedEntities;
    const auto previousActive = m_SelectedEntity;
    const auto previousAnchor = m_EntitySelectionAnchor;
    const auto previousUndo = m_UndoStack;
    const auto previousRedo = m_RedoStack;
    const auto previousPending = m_PendingUndo;
    const bool hadPending = m_HasPendingUndo;
    const auto revision = m_EditRevision;
    const auto originalEntities = SceneHierarchy::Entities();
    const unordered_set<EntityID> originalSet(originalEntities.begin(), originalEntities.end());
    const auto path = MakeUniqueAssetPath(m_AssetRoot, "__editor_object_validation", ".prefab");
    int failures = 0;
    auto check = [&](bool condition, const char* description) {
        WriteGraphicsLog("Editor object validation: %s %s\n", description, condition ? "PASS" : "FAIL");
        if (!condition)
        {
            throw std::runtime_error(description);
        }
    };
    auto position = [](EntityID entity) -> float {
        return ComponentManager::GetComponentUnchecked<TransformComponent>(entity).Position.x;
    };
    try
    {
        check(!IsEditableEntity(Camera::GetEditorCameraEntity()), "editor camera excluded from batch selection");
        TransformComponent affine;
        affine.Rotation.z = 0.5f;
        const XMMATRIX sheared = TransformMath::World(affine) * XMMatrixScaling(2, 1, 1);
        check(TransformMath::SetWorld(affine, sheared), "affine hierarchy transform");
        Json::Value serialized;
        SceneFile::Archive write(SceneFile::Mode::Write, serialized);
        SceneFile::Visit(write, affine);
        TransformComponent restored;
        SceneFile::Archive read(SceneFile::Mode::Read, serialized);
        SceneFile::Visit(read, restored);
        XMFLOAT4X4 restoredMatrix;
        XMFLOAT4X4 expectedMatrix;
        XMStoreFloat4x4(&restoredMatrix, TransformMath::World(restored));
        XMStoreFloat4x4(&expectedMatrix, sheared);
        check(std::memcmp(&restoredMatrix, &expectedMatrix, sizeof(expectedMatrix)) == 0, "affine scene serialization");
        m_UndoStack.clear();
        m_RedoStack.clear();
        m_PendingUndo.clear();
        m_HasPendingUndo = false;
        auto affineEntity = World::CreateEntity().Add<TransformComponent>().Add<NameComponent>();
        affineEntity.Get<TransformComponent>() = affine;
        SelectEntity(affineEntity.GetID());
        auto movedAffine = affine;
        movedAffine.Position.x += 3;
        ApplySelectionTransform(affine, movedAffine);
        FinalizeUndoCaptureIfIdle(true);
        const auto& movedMatrix = affineEntity.Get<TransformComponent>().WorldMatrix;
        bool shapePreserved = true;
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                shapePreserved &= std::abs(movedMatrix.m[row][column] - expectedMatrix.m[row][column]) < 0.0001f;
            }
        }
        check(shapePreserved && std::abs(movedMatrix._41 - 3) < 0.0001f, "affine property translation preserves shape");
        auto parent = World::CreateEntity().Add<TransformComponent>().Add<NameComponent>();
        auto child = World::CreateEntity().Add<TransformComponent>().Add<NameComponent>();
        parent.SetName("Prefab validation root");
        child.SetName("Prefab validation child");
        parent.Get<TransformComponent>().Position.x = 1;
        child.Get<TransformComponent>().Position.x = 3;
        const EntityID root = parent.GetID();
        const EntityID childId = child.GetID();
        check(SceneHierarchy::SetParent(childId, root), "parent assignment");
        check(!SceneHierarchy::SetParent(root, childId), "cycle rejection");
        SelectEntity(root);
        SelectEntity(childId, true);
        const auto before = child.Get<TransformComponent>();
        auto after = before;
        after.Position.x += 5;
        ApplySelectionTransform(before, after);
        FinalizeUndoCaptureIfIdle(true);
        check(std::abs(position(root) - 6) < 0.0001f && std::abs(position(childId) - 8) < 0.0001f,
              "parent and child selection transforms once");
        Undo();
        check(std::abs(position(root) - 1) < 0.0001f && std::abs(position(childId) - 3) < 0.0001f, "group undo");
        Redo();
        check(std::abs(position(childId) - 8) < 0.0001f, "group redo");
        DrawInspectorWindow();
        DrawHierarchyWindow();
        SelectEntity(root);
        check(SavePrefabAsset(root, path), "prefab creation");
        const XMFLOAT3 dropPosition{20, 0, 0};
        const EntityID instance = LoadPrefabInstance(path, g_kINVALID_ENTITY, nullptr, &dropPosition);
        check(instance != g_kINVALID_ENTITY, "prefab placement");
        EntityID instanceChild = g_kINVALID_ENTITY;
        for (EntityID entity : SceneHierarchy::Entities())
        {
            if (SceneHierarchy::Parent(entity) == instance)
            {
                instanceChild = entity;
            }
        }
        check(instanceChild != g_kINVALID_ENTITY && std::abs(position(instanceChild) - 22) < 0.0001f,
              "prefab hierarchy and placement restored");
        const XMFLOAT3 secondPosition{30, 0, 0};
        const EntityID secondInstance = LoadPrefabInstance(path, g_kINVALID_ENTITY, nullptr, &secondPosition);
        EntityID secondChild = g_kINVALID_ENTITY;
        for (EntityID entity : SceneHierarchy::Entities())
        {
            if (SceneHierarchy::Parent(entity) == secondInstance)
            {
                secondChild = entity;
            }
        }
        check(secondChild != g_kINVALID_ENTITY, "second prefab instance");
        child.Get<TransformComponent>().Position.x += 2;
        child.Get<TransformComponent>().IsDirty = true;
        check(SavePrefabAsset(root, path), "apply prefab");
        check(Registry::IsAlive(instanceChild) && SceneHierarchy::Parent(instanceChild) == instance &&
                  std::abs(position(instance) - 20) < 0.0001f && std::abs(position(instanceChild) - 24) < 0.0001f,
              "apply keeps instance IDs and placement");
        Undo();
        check(std::abs(position(instanceChild) - 22) < 0.0001f && std::abs(position(secondChild) - 32) < 0.0001f,
              "prefab apply undo groups all instances");
        Redo();
        check(std::abs(position(instanceChild) - 24) < 0.0001f && std::abs(position(secondChild) - 34) < 0.0001f,
              "prefab apply redo groups all instances");
        ComponentManager::GetComponentUnchecked<TransformComponent>(instanceChild).Position.x = 99;
        ComponentManager::GetComponentUnchecked<TransformComponent>(instanceChild).IsDirty = true;
        check(LoadPrefabInstance(path, instance) == instance && std::abs(position(instanceChild) - 24) < 0.0001f,
              "revert prefab instance");
        DrawInspectorWindow();
        DrawHierarchyWindow();
        SelectEntity(root);
        SelectEntity(instance, true);
        DeleteSelectedEntity();
        check(!Registry::IsAlive(root) && !Registry::IsAlive(childId) && !Registry::IsAlive(instanceChild),
              "batch deletion includes children");
        Undo();
        check(Registry::IsAlive(root) && Registry::IsAlive(childId) && Registry::IsAlive(instanceChild) &&
                  SceneHierarchy::Parent(childId) == root && GetPrefabRoot(instanceChild) == instance,
              "deletion undo restores hierarchy and prefab links");
    }
    catch (const std::exception& exception)
    {
        ++failures;
        WriteGraphicsLog("Editor object validation exception: %s\n", exception.what());
    }
    for (EntityID entity : SceneHierarchy::Entities())
    {
        if (!originalSet.count(entity))
        {
            World::DestroyEntity(Entity(entity));
        }
    }
    error_code error;
    filesystem::remove(path, error);
    auto temporary = path;
    temporary += L".tmp";
    filesystem::remove(temporary, error);
    m_SelectedEntities = previousSelection;
    m_SelectedEntity = previousActive;
    m_EntitySelectionAnchor = previousAnchor;
    m_UndoStack = previousUndo;
    m_RedoStack = previousRedo;
    m_PendingUndo = previousPending;
    m_HasPendingUndo = hadPending;
    m_EditRevision = revision;
    WriteGraphicsLog("Editor object validation failures = %d\n", failures);
}
