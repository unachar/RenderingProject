#include "pch.h"
#include "imguimanager.h"
#include "materialmanager.h"
#include "materialopticsui.h"
#include "materialpreview.h"
#include "materialvalidation.h"
#include "graphicslog.h"
#include <imgui_internal.h>
#include "editorwidgets.h"
#include "world.h"
#include "camera.h"
#include <DirectXCollision.h>

namespace
{
    struct MaterialEdit
    {
        AssetHandle Handle = 0;
        MaterialAssetData Before;
    };

    AssetHandle openMaterial = 0;
    AssetHandle requestedMaterial = 0;
    bool closeRequested = false;
    bool promptRequested = false;
    bool editorFocused = false;
    bool focusEditorRequested = false;
    bool pendingEdit = false;
    MaterialEdit editStart;
    unordered_map<AssetHandle, vector<MaterialAssetData>> materialUndo;
    unordered_map<AssetHandle, vector<MaterialAssetData>> materialRedo;
    EntityID dropEntity = g_kINVALID_ENTITY;
    AssetHandle dropMaterial = 0;

    const AssetDragPayload* ReadPayload(const ImGuiPayload* payload, AssetType expected)
    {
        if (!payload || payload->DataSize != sizeof(AssetDragPayload))
        {
            return nullptr;
        }
        const auto* asset = static_cast<const AssetDragPayload*>(payload->Data);
        if (asset->Type != expected || MaterialManager::Registry().TypeOf(asset->Handle) != expected)
        {
            return nullptr;
        }
        return asset;
    }

    string AssetName(AssetHandle handle, const char* empty)
    {
        if (handle == 0)
        {
            return empty;
        }
        const auto path = MaterialManager::Registry().Resolve(handle);
        if (path.empty())
        {
            return "Missing: " + FormatAssetHandle(handle);
        }
        return path.filename().generic_string();
    }

    bool DrawTextureSlot(const char* label, AssetHandle& handle, bool allowNoise = false)
    {
        ImGui::PushID(label);
        const auto name = AssetName(handle, "未設定");
        ImGui::Button(name.c_str(), ImVec2(-40, 0));
        if (allowNoise)
        {
            ImGui::SetItemTooltip("通常のテクスチャ、または .noise をドロップ。ノイズは描画時に計算します。");
        }
        const auto textureMinimum = ImGui::GetItemRectMin();
        const auto textureMaximum = ImGui::GetItemRectMax();
        MaterialValidation::CaptureDropTarget(label, textureMinimum.x, textureMinimum.y, textureMaximum.x,
                                              textureMaximum.y);
        if (MaterialValidation::Enabled() && strcmp(label, "Normal") == 0 && ImGui::GetDragDropPayload())
        {
            WriteGraphicsLog("Material UI trace: Normal rect=%.1f,%.1f..%.1f,%.1f hovered=%d mouse=%.1f,%.1f\n",
                             textureMinimum.x, textureMinimum.y, textureMaximum.x, textureMaximum.y,
                             ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem),
                             ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
        }
        bool changed = false;
        if (ImGui::BeginDragDropTarget())
        {
            const auto* payload = ImGui::AcceptDragDropPayload(AssetPayloadType);
            const auto* asset = ReadPayload(payload, AssetType::Texture);
            if (!asset && allowNoise)
            {
                asset = ReadPayload(payload, AssetType::Noise);
            }
            if (MaterialValidation::Enabled())
            {
                WriteGraphicsLog("Material UI trace: Texture target accepted=%d\n", asset != nullptr);
            }
            if (asset)
            {
                handle = asset->Handle;
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
        {
            handle = 0;
            changed = true;
        }
        ImGui::PopID();
        return changed;
    }

    void FinishMaterialEdit()
    {
        if (pendingEdit)
        {
            if (MaterialManager::GetRuntime(editStart.Handle).Editing != editStart.Before)
            {
                materialUndo[editStart.Handle].push_back(editStart.Before);
                materialRedo[editStart.Handle].clear();
            }
            pendingEdit = false;
        }
    }
}

void ImGuiManager::OpenMaterialAsset(AssetHandle handle)
{
    if (handle == 0 || MaterialManager::Registry().TypeOf(handle) != AssetType::Material)
    {
        return;
    }
    FinishMaterialEdit();
    if (openMaterial != 0 && openMaterial != handle && MaterialManager::GetRuntime(openMaterial).Dirty)
    {
        requestedMaterial = handle;
        closeRequested = false;
        promptRequested = true;
        return;
    }
    openMaterial = handle;
    m_ShowMaterialEditorWindow = true;
    focusEditorRequested = true;
}

bool ImGuiManager::MaterialEditorHasFocus()
{
    return editorFocused && openMaterial != 0 && m_ShowMaterialEditorWindow;
}

void ImGuiManager::MaterialEditorUndo(bool redo)
{
    FinishMaterialEdit();
    if (openMaterial == 0)
    {
        return;
    }
    auto* source = &materialUndo[openMaterial];
    auto* destination = &materialRedo[openMaterial];
    if (redo)
    {
        swap(source, destination);
    }
    if (source->empty())
    {
        return;
    }
    const auto editing = source->back();
    auto& runtime = MaterialManager::GetRuntime(openMaterial);
    const auto current = runtime.Editing;
    if (MaterialManager::SetEditing(openMaterial, editing))
    {
        source->pop_back();
        destination->push_back(current);
    }
}

void ImGuiManager::DrawMaterialEditorWindow()
{
    if (openMaterial == 0)
    {
        DrawLegacyMaterialEditorWindow();
        return;
    }
    auto& runtime = MaterialManager::GetRuntime(openMaterial);
    const auto name = AssetName(openMaterial, "Default");
    string title = "Material: " + name;
    if (runtime.Dirty)
    {
        title += " *";
    }
    title += "###MaterialAssetEditor";
    bool visible = true;
    ImGui::SetNextWindowSize(ImVec2(840, 560), ImGuiCond_FirstUseEver);
    if (focusEditorRequested)
    {
        ImGui::SetNextWindowFocus();
        focusEditorRequested = false;
    }
    const bool expanded = ImGui::Begin(title.c_str(), &visible);
    editorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!visible)
    {
        FinishMaterialEdit();
        closeRequested = true;
        requestedMaterial = 0;
        promptRequested = true;
    }
    if (!expanded)
    {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Save"))
    {
        FinishMaterialEdit();
        MaterialManager::Save(openMaterial);
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
    {
        FinishMaterialEdit();
        const auto before = runtime.Editing;
        if (MaterialManager::Revert(openMaterial) && before != runtime.Editing)
        {
            materialUndo[openMaterial].push_back(before);
            materialRedo[openMaterial].clear();
        }
    }
    const auto revertMinimum = ImGui::GetItemRectMin();
    const auto revertMaximum = ImGui::GetItemRectMax();
    MaterialValidation::CaptureDropTarget("Revert", revertMinimum.x, revertMinimum.y, revertMaximum.x, revertMaximum.y);
    if (editorFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        FinishMaterialEdit();
        MaterialManager::Save(openMaterial);
    }
    ImGui::SameLine();
    MaterialPreview::DrawControls();
    ImGui::Separator();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("MaterialPreview", ImVec2(width * 0.54f, -ImGui::GetTextLineHeightWithSpacing()), true);
    MaterialPreview::DrawImage(openMaterial);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("Properties", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()));
    MaterialAssetData editing = runtime.Editing;
    bool changed = false;
    if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("Shader: PBR");
        int surface = static_cast<int>(editing.Surface);
        if (ImGui::Combo("Surface Type", &surface, "Opaque\0Masked\0Transparent\0"))
        {
            editing.Surface = static_cast<MaterialSurface>(surface);
            if (editing.Surface == MaterialSurface::Transparent)
            {
                editing.DepthWrite = false;
            }
            changed = true;
        }
    }
    if (ImGui::CollapsingHeader("Base Color", ImGuiTreeNodeFlags_DefaultOpen))
    {
        changed |= ImGui::ColorEdit3("Color", editing.BaseColor.data());
        float transparency = 1.0f - editing.BaseColor[3];
        if (ImGui::SliderFloat("Coverage Transparency", &transparency, 0, 1))
        {
            editing.BaseColor[3] = 1.0f - transparency;
            if (transparency > 0.0f)
            {
                editing.Surface = MaterialSurface::Transparent;
                editing.DepthWrite = false;
            }
            changed = true;
        }
        ImGui::SetItemTooltip(
            "Coverage transparency, not optical transmission. For glass use Transmission=1 and Transparency=0.");
        const auto transparencyMinimum = ImGui::GetItemRectMin();
        const auto transparencyMaximum = ImGui::GetItemRectMax();
        MaterialValidation::CaptureDropTarget("Transparency", transparencyMinimum.x, transparencyMinimum.y,
                                              transparencyMaximum.x, transparencyMaximum.y);
        changed |= DrawTextureSlot("Base Color", editing.Textures[0], true);
    }
    if (ImGui::CollapsingHeader("Metallic / Roughness", ImGuiTreeNodeFlags_DefaultOpen))
    {
        changed |= ImGui::SliderFloat("Metallic", &editing.Metallic, 0, 1);
        changed |= ImGui::SliderFloat("Roughness", &editing.Roughness, 0, 1);
        ImGui::TextDisabled("Texture: R = Metallic / G = Roughness");
        changed |= DrawTextureSlot("Metallic Roughness", editing.Textures[2]);
    }
    if (ImGui::CollapsingHeader("Normal", ImGuiTreeNodeFlags_DefaultOpen))
    {
        changed |= DrawTextureSlot("Normal", editing.Textures[1], true);
        changed |= ImGui::SliderFloat("Strength", &editing.NormalScale, 0, 8);
    }
    if (ImGui::CollapsingHeader("Parallax Occlusion Mapping"))
    {
        changed |= DrawTextureSlot("Height (POM)", editing.Textures[5], true);
        changed |= ImGui::SliderFloat("Parallax Depth", &editing.ParallaxDepth, 0.0f, 0.2f, "%.3f");
        changed |= ImGui::SliderFloat("Parallax Steps", &editing.ParallaxSteps, 8, 128, "%.0f");
        ImGui::TextDisabled("Height: white = top, black = recessed. Depth 0 disables POM.");
        ImGui::TextDisabled("Supported on opaque PBR materials.");
        if (editing.Textures[5] != 0)
        {
            if (editing.Surface != MaterialSurface::Opaque || editing.Transmission > 0.0f)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Surface が Opaque 以外のため POM は無効です。");
            }
            else if (editing.ParallaxDepth <= 0.0f)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Parallax Depth が 0 のため POM は無効です。");
            }
            else if (editing.Textures[1] == 0)
            {
                ImGui::TextDisabled("Normal 未設定: 高さマップから法線を自動生成します。");
            }
        }
    }
    if (ImGui::CollapsingHeader("Ambient Occlusion"))
    {
        changed |= DrawTextureSlot("AO", editing.Textures[3]);
        changed |= ImGui::SliderFloat("AO Strength", &editing.AOStrength, 0, 1);
    }
    if (ImGui::CollapsingHeader("Emission"))
    {
        changed |= ImGui::ColorEdit3("Emission Color", editing.EmissiveColor.data(),
                                     ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        changed |= ImGui::SliderFloat("Intensity", &editing.EmissiveIntensity, 0, 30);
        changed |= DrawTextureSlot("Emission", editing.Textures[4]);
    }
    if (ImGui::CollapsingHeader("Shading Model", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int model = static_cast<int>(editing.ShadingModel);
        if (ImGui::Combo("Model", &model, "Standard\0Clear Coat\0Sheen (Cloth)\0Skin\0Hair\0Anisotropic\0"))
        {
            editing.ShadingModel = static_cast<uint32_t>(model);
            changed = true;
        }
        ImGui::BeginDisabled(editing.ShadingModel == 0);
        changed |= ImGui::SliderFloat("Model Strength", &editing.ShadingStrength, 0, 1);
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "ClearCoat=塗装の層 / Sheen=布の光沢 / Skin=肌の散乱 / Hair=髪のハイライト / Anisotropic=ヘアライン金属");
    }
    if (ImGui::CollapsingHeader("Advanced"))
    {
        int cull = static_cast<int>(editing.Cull);
        if (ImGui::Combo("Cull Mode", &cull, "Back\0None\0Front\0"))
        {
            editing.Cull = static_cast<MaterialCull>(cull);
            changed = true;
        }
        changed |= ImGui::Checkbox("Depth Write", &editing.DepthWrite);
        changed |= ImGui::SliderFloat("Alpha Cutoff", &editing.AlphaCutoff, 0, 1);
        int shadowMode = static_cast<int>(editing.ShadowMode);
        if (ImGui::Combo("Shadow Mode", &shadowMode, "None\0Opaque\0AlphaTest\0Transmittance\0Auto\0"))
        {
            editing.ShadowMode = static_cast<MaterialShadowMode>(shadowMode);
            changed = true;
        }
        if (editing.ShadowMode == MaterialShadowMode::AlphaTest)
        {
            changed |= ImGui::SliderFloat("Shadow Cutoff", &editing.ShadowCutoff, 0, 1);
        }
        ImGui::Text("Handle: %s", FormatAssetHandle(openMaterial).c_str());
        ImGui::Text("Runtime revision: %llu", runtime.Revision);
    }
    const bool opticsChanged =
        MaterialOpticsUI::Draw(editing.IOR, editing.Transmission, editing.Roughness, editing.TransmissionRoughness,
                               editing.Thickness, editing.AbsorptionCoefficient.data());
    changed |= opticsChanged;
    if (editing.Transmission > 0.0f)
    {
        // 描画経路だけを変更し、設定済みの色・粗さ・IOR は上書きしない。
        if (editing.Surface != MaterialSurface::Transparent || editing.DepthWrite)
        {
            editing.Surface = MaterialSurface::Transparent;
            editing.DepthWrite = false;
            changed = true;
        }
        ImGui::TextDisabled("Optical transmission: Transparent path, depth write disabled.");
    }
    if (changed)
    {
        if (!pendingEdit)
        {
            editStart = {openMaterial, runtime.Editing};
            pendingEdit = true;
        }
        MaterialManager::SetEditing(openMaterial, editing);
    }
    if (!ImGui::IsAnyItemActive())
    {
        FinishMaterialEdit();
    }
    ImGui::EndChild();
    if (!runtime.SourceError.empty())
    {
        ImGui::TextColored(ImVec4(1, 0.35f, 0.25f, 1), "%s", runtime.SourceError.c_str());
    }
    if (!runtime.Error.empty())
    {
        ImGui::TextColored(ImVec4(1, 0.35f, 0.25f, 1), "%s", runtime.Error.c_str());
    }
    else
    {
        ImGui::TextDisabled("%s", MaterialManager::Registry().Resolve(openMaterial).generic_string().c_str());
    }
    ImGui::End();
}

void ImGuiManager::DrawMaterialAssetClosePrompt()
{
    if (openMaterial != 0 && !m_ShowMaterialEditorWindow)
    {
        closeRequested = true;
        requestedMaterial = 0;
        promptRequested = true;
        m_ShowMaterialEditorWindow = true;
    }
    if (promptRequested)
    {
        promptRequested = false;
        if (openMaterial != 0 && MaterialManager::GetRuntime(openMaterial).Dirty)
        {
            ImGui::OpenPopup("Material has unsaved changes");
        }
        else
        {
            openMaterial = requestedMaterial;
            m_ShowMaterialEditorWindow = !closeRequested;
        }
    }
    if (ImGui::BeginPopupModal("Material has unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Material has unsaved changes.");
        if (!MaterialManager::GetRuntime(openMaterial).Error.empty())
        {
            ImGui::TextWrapped("%s", MaterialManager::GetRuntime(openMaterial).Error.c_str());
        }
        bool finish = false;
        if (ImGui::Button("Save"))
        {
            finish = MaterialManager::Save(openMaterial);
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard"))
        {
            MaterialManager::Discard(openMaterial);
            finish = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            closeRequested = false;
            requestedMaterial = 0;
            ImGui::CloseCurrentPopup();
        }
        if (finish)
        {
            openMaterial = requestedMaterial;
            m_ShowMaterialEditorWindow = !closeRequested;
            focusEditorRequested = !closeRequested;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ImGuiManager::AssignMaterialAsset(EntityID entity, size_t slot, AssetHandle handle)
{
    const auto before = CaptureEntity(entity);
    if (MaterialManager::Assign(entity, slot, handle))
    {
        PushUndoSnapshot(before);
        AddLog("Material assigned: %s / Slot %zu", AssetName(handle, "Legacy").c_str(), slot);
    }
}

void ImGuiManager::DrawMaterialAssetSlots(EntityID entity)
{
    if (!MaterialManager::IsRenderable(entity) || !ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }
    const auto& component = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    for (size_t slot = 0; slot < MaterialManager::SlotCount(entity); ++slot)
    {
        ImGui::PushID(static_cast<int>(slot));
        const auto handle = MaterialManager::SlotHandle(component, slot);
        ImGui::Text("Element %zu", slot);
        if (ImGui::Button(AssetName(handle, "Legacy / Imported").c_str(), ImVec2(-40, 0)) && handle != 0)
        {
            OpenMaterialAsset(handle);
        }
        const auto slotMinimum = ImGui::GetItemRectMin();
        const auto slotMaximum = ImGui::GetItemRectMax();
        MaterialValidation::CaptureDropTarget("Inspector", slotMinimum.x, slotMinimum.y, slotMaximum.x, slotMaximum.y);
        if (ImGui::BeginDragDropTarget())
        {
            const auto* asset = ReadPayload(ImGui::AcceptDragDropPayload(AssetPayloadType), AssetType::Material);
            if (asset)
            {
                AssignMaterialAsset(entity, slot, asset->Handle);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
        {
            AssignMaterialAsset(entity, slot, 0);
        }
        ImGui::PopID();
    }
}

void ImGuiManager::DrawMaterialDropTarget()
{
    if (MaterialValidation::Enabled() && ImGui::GetDragDropPayload())
    {
        WriteGraphicsLog("Material UI trace: Scene hovered=%d mouse=%.1f,%.1f\n",
                         ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem),
                         ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
        const auto& context = *ImGui::GetCurrentContext();
        WriteGraphicsLog("Material UI trace: hoveredWindow=%s sceneRect=%.1f,%.1f..%.1f,%.1f\n",
                         context.HoveredWindow ? context.HoveredWindow->Name : "none", context.LastItemData.Rect.Min.x,
                         context.LastItemData.Rect.Min.y, context.LastItemData.Rect.Max.x,
                         context.LastItemData.Rect.Max.y);
    }
    if (ImGui::BeginDragDropTarget())
    {
        const auto* payload = ImGui::AcceptDragDropPayload(AssetPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery);
        const auto* asset = ReadPayload(payload, AssetType::Material);
        const auto* texture = ReadPayload(payload, AssetType::Texture);
        if (texture && payload->IsDelivery())
        {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            mouse.x -= m_SceneViewPos.x;
            mouse.y -= m_SceneViewPos.y;
            PlaceAssetInScene(MaterialManager::Registry().Resolve(texture->Handle), mouse);
        }

        if (asset)
        {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            mouse.x -= m_SceneViewPos.x;
            mouse.y -= m_SceneViewPos.y;
            XMVECTOR origin;
            XMVECTOR direction;
            EntityID target = g_kINVALID_ENTITY;
            float nearest = FLT_MAX;
            if (GetSceneViewRay(mouse, origin, direction))
            {
                for (EntityID entity : World::GetView<TransformComponent>())
                {
                    XMFLOAT3 center;
                    XMFLOAT3 extents;
                    if (!EditorWidgets::GetLocalAabb(entity, center, extents))
                    {
                        continue;
                    }
                    BoundingOrientedBox local(center, extents, XMFLOAT4(0, 0, 0, 1));
                    BoundingOrientedBox world;
                    local.Transform(world, EditorWidgets::BuildWorldMatrix(
                                               ComponentManager::GetComponentUnchecked<TransformComponent>(entity)));
                    float distance;
                    if (world.Intersects(origin, direction, distance) && distance < nearest)
                    {
                        target = entity;
                        nearest = distance;
                    }
                }
            }
            if (MaterialValidation::Enabled())
            {
                WriteGraphicsLog("Material UI trace: picked=%u renderable=%d delivery=%d\n", target,
                                 MaterialManager::IsRenderable(target), payload->IsDelivery());
            }
            if (MaterialManager::IsRenderable(target))
            {
                ImGui::SetTooltip("%s: Materialを適用", GetEntityDisplayName(target));
                // 対象の実際の境界を投影し、ホバー中のエンティティにハイライトを追従させる。
                XMFLOAT3 center;
                XMFLOAT3 extents;
                if (EditorWidgets::GetLocalAabb(target, center, extents))
                {
                    XMMATRIX view;
                    XMMATRIX projection;
                    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
                    const auto world = EditorWidgets::BuildWorldMatrix(
                        ComponentManager::GetComponentUnchecked<TransformComponent>(target));
                    ImVec2 minimum(FLT_MAX, FLT_MAX);
                    ImVec2 maximum(-FLT_MAX, -FLT_MAX);
                    for (int corner = 0; corner < 8; ++corner)
                    {
                        const float x = center.x + ((corner & 1) ? extents.x : -extents.x);
                        const float y = center.y + ((corner & 2) ? extents.y : -extents.y);
                        const float z = center.z + ((corner & 4) ? extents.z : -extents.z);
                        XMFLOAT3 screen;
                        XMStoreFloat3(&screen, XMVector3Project(XMVectorSet(x, y, z, 1), m_SceneViewPos.x,
                                                                m_SceneViewPos.y, m_SceneViewSize.x, m_SceneViewSize.y,
                                                                0, 1, projection, view, world));
                        minimum.x = min(minimum.x, screen.x);
                        minimum.y = min(minimum.y, screen.y);
                        maximum.x = max(maximum.x, screen.x);
                        maximum.y = max(maximum.y, screen.y);
                    }
                    ImGui::GetWindowDrawList()->AddRect(minimum, maximum, IM_COL32(70, 210, 255, 255), 3, 0, 2);
                }
                if (payload->IsDelivery())
                {
                    if (MaterialManager::SlotCount(target) == 1)
                    {
                        AssignMaterialAsset(target, 0, asset->Handle);
                    }
                    else
                    {
                        dropEntity = target;
                        dropMaterial = asset->Handle;
                        ImGui::OpenPopup("Material Slotを選択");
                    }
                }
            }
            else
            {
                ImGui::SetTooltip("Material cannot be assigned to this entity.");
                if (payload->IsDelivery())
                {
                    AddLog("Material cannot be assigned to this entity.");
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("Material Slotを選択"))
    {
        for (size_t slot = 0; slot < MaterialManager::SlotCount(dropEntity); ++slot)
        {
            if (ImGui::Selectable(("Element " + to_string(slot)).c_str()))
            {
                AssignMaterialAsset(dropEntity, slot, dropMaterial);
            }
        }
        ImGui::EndPopup();
    }
}

