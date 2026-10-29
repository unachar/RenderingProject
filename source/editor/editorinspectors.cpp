#include "pch.h"
#include "materialmanager.h"
#include "modelmeshtable.h"
#include "materialopticsui.h"
#include "proceduralio.h"
#include "imguimanager.h"
#include "world.h"
#include "editorwidgets.h"
#include "camera.h"
#include "frameconstants.h"
#include "graphicsdevice.h"
#include "materialsystem.h"
#include "modelmanager.h"
#include "physicssystem.h"
#include "projectmanager.h"
#include "sun.h"
#include "systemmanager.h"
#include "texturemanager.h"
#include "timelinesystem.h"
#include "gpuscene/gpuscenechanges.h"
#include "impostorrenderer.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <DirectXCollision.h>
#include <ImGuizmo.h>

using namespace EditorWidgets;

void ImGuiManager::DrawInspectorWindow()
{
    SyncEntitySelection();
    ImGui::SetNextWindowSize(ImVec2(312.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("インスペクター", &m_ShowInspectorWindow))
    {
        ImGui::End();
        return;
    }

    if (ImGui::Button("選択対象を変更"))
    {
        ImGui::OpenPopup("InspectorSelection");
    }
    if (ImGui::BeginPopup("InspectorSelection"))
    {
        if (ImGui::Button("すべて選択"))
        {
            FinalizeUndoCaptureIfIdle(true);
            m_SelectedEntities.clear();
            for (EntityID entity : World::GetView<TransformComponent>())
            {
                if (IsEditableEntity(entity))
                {
                    m_SelectedEntities.push_back(entity);
                    m_SelectedEntity = entity;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("選択解除"))
        {
            SelectEntity(g_kINVALID_ENTITY);
        }
        ImGui::BeginChild("SelectionList", ImVec2(280.0f, 260.0f));
        for (EntityID entity : World::GetView<TransformComponent>())
        {
            if (!IsEditableEntity(entity))
            {
                continue;
            }
            ImGui::PushID(static_cast<int>(entity));
            bool selected = IsEntitySelected(entity);
            if (ImGui::Checkbox(GetEntityDisplayName(entity), &selected))
            {
                SelectEntity(entity, true);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
    {
        ImGui::SeparatorText("オブジェクト未選択");
        ImGui::TextWrapped(
            "ヒエラルキーまたはシーンビューでオブジェクトを選択すると、ここでプロパティを編集できます。");
        ImGui::End();
        return;
    }

    if (m_SelectedEntities.size() > 1)
    {
        ImGui::Text("%zu 個のオブジェクトを選択中", m_SelectedEntities.size());
        ImGui::TextWrapped("位置・回転・スケールは、最後に選んだオブジェクトを基準にまとめて変更します。");
        if (ImGui::TreeNode("選択オブジェクト"))
        {
            for (EntityID entity : m_SelectedEntities)
            {
                ImGui::BulletText("%s (#%u)", GetEntityDisplayName(entity), entity);
            }
            ImGui::TreePop();
        }
    }
    if (m_RenamingEntity == m_SelectedEntity)
    {
        DrawRenameInput(m_SelectedEntity);
    }
    else
    {
        if (GetPrefabRoot(m_SelectedEntity) != g_kINVALID_ENTITY)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kPrefabColor);
        }
        ImGui::TextWrapped("%s", GetEntityDisplayName(m_SelectedEntity));
        if (GetPrefabRoot(m_SelectedEntity) != g_kINVALID_ENTITY)
        {
            ImGui::PopStyleColor();
        }
        ImGui::TextDisabled("Entity #%u", m_SelectedEntity);
    }
    ImGui::Separator();

    if (ImGui::BeginPopupContextWindow("InspectorEntityContext", ImGuiPopupFlags_MouseButtonRight))
    {
        if (ImGui::MenuItem("名前を変更", "F2", false,
                            m_SelectedEntities.size() == 1 &&
                                ComponentManager::HasComponent<NameComponent>(m_SelectedEntity)))
        {
            BeginRename(m_SelectedEntity);
        }
        if (ImGui::MenuItem("削除"))
        {
            DeleteSelectedEntity();
            ImGui::EndPopup();
            ImGui::End();
            return;
        }
        ImGui::EndPopup();
    }

    if (m_SelectedEntities.size() == 1 && DrawPrefabInspector())
    {
        ImGui::End();
        return;
    }
    DrawParentSelector();
    if (ImGui::Button("選択を削除（子も含む）"))
    {
        DeleteSelectedEntity();
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        DeleteSelectedEntity();
        ImGui::End();
        return;
    }
    if (ComponentManager::HasComponent<TransformComponent>(m_SelectedEntity) &&
        ImGui::CollapsingHeader("トランスフォーム", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const auto before = ComponentManager::GetComponentUnchecked<TransformComponent>(m_SelectedEntity);
        auto transform = before;
        ImGui::TextDisabled("ワールド座標 / 最後に選択したオブジェクトが基準");
        float rotationDeg[3] = {transform.Rotation.x * kRadToDeg, transform.Rotation.y * kRadToDeg,
                                transform.Rotation.z * kRadToDeg};
        bool changed = DrawAxisFloat3("位置", transform.Position, 0.01f);
        if (DrawAxisFloat3("回転", rotationDeg, 0.1f))
        {
            transform.Rotation = {rotationDeg[0] * kDegToRad, rotationDeg[1] * kDegToRad, rotationDeg[2] * kDegToRad};
            changed = true;
        }
        changed |= DrawScaleAxisFloat3("スケール", transform.Scale, 0.01f, 0.001f, 100.0f);
        if (changed)
        {
            ApplySelectionTransform(before, transform);
        }
    }
    if (m_SelectedEntities.size() > 1)
    {
        ImGui::End();
        return;
    }

    if (ComponentManager::HasComponent<AABBComponent>(m_SelectedEntity) &&
        ImGui::CollapsingHeader("AABB", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(m_SelectedEntity);
        EntitySnapshot before = CaptureEntity(m_SelectedEntity);
        bool changed = false;
        changed |= ImGui::Checkbox("デバッグ描画", &aabb.DrawDebug);
        changed |= ImGui::DragFloat3("AABB中心", &aabb.Center.x, 0.01f);
        changed |= ImGui::DragFloat3("AABBサイズ", &aabb.Extents.x, 0.01f, 0.001f, 100.0f);
        if (changed)
        {
            BeginUndoCapture(m_SelectedEntity, before);
        }
    }

    if (ComponentManager::HasComponent<LightComponent>(m_SelectedEntity))
    {
        DrawLightInspector(m_SelectedEntity);
    }
    DrawMaterialAssetSlots(m_SelectedEntity);
    DrawComponentInspector(m_SelectedEntity);

    ImGui::TextUnformatted("右クリック: カメラ / 左クリック: 選択");
    ImGui::End();
}

void ImGuiManager::DrawLegacyMaterialEditorWindow()
{
    ImGui::SetNextWindowSize(ImVec2(380.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("マテリアルエディター", &m_ShowMaterialEditorWindow))
    {
        ImGui::End();
        return;
    }

    if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity))
    {
        ImGui::TextUnformatted("マテリアルを持つオブジェクトを選択してください");
        ImGui::End();
        return;
    }

    ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
    ImGui::Separator();

    if (!ComponentManager::HasComponent<MaterialComponent>(m_SelectedEntity))
    {
        ImGui::TextUnformatted("選択中のオブジェクトにマテリアルがありません");
        ImGui::End();
        return;
    }

    DrawMaterialInspector(m_SelectedEntity);
    ImGui::End();
}

void ImGuiManager::DrawRimSettingsWindow()
{
    ImGui::SetNextWindowSize(ImVec2(420.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("リム設定", &m_ShowRimSettingsWindow))
    {
        ImGui::End();
        return;
    }

    if (m_SelectedEntity == g_kINVALID_ENTITY || !Registry::IsAlive(m_SelectedEntity) ||
        !ComponentManager::HasComponent<MaterialComponent>(m_SelectedEntity))
    {
        ImGui::TextUnformatted("リムを設定するマテリアル付きオブジェクトを選択してください");
        ImGui::End();
        return;
    }

    auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(m_SelectedEntity);
    EntitySnapshot before = CaptureEntity(m_SelectedEntity);
    bool changed = false;

    ImGui::Text("選択中: %s", GetEntityDisplayName(m_SelectedEntity));
    ImGui::Separator();

    if (ImGui::BeginTabBar("RimSettingsTabs"))
    {
        if (ImGui::BeginTabItem("全体"))
        {
            changed |= ImGui::SliderFloat("リム強度", &material.RimStrength, 0.0f, 3.0f);
            changed |= ImGui::SliderFloat("リムしきい値", &material.RimThreshold, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("リムぼかし", &material.RimSoftness, 0.001f, 0.50f, "%.3f");
            changed |= ImGui::SliderFloat("リムカーブ", &material.RimPower, 0.05f, 6.0f, "%.2f");
            changed |= ImGui::ColorEdit3("リム色", &material.RimColor.x, ImGuiColorEditFlags_Float);
            changed |= ImGui::SliderFloat("ベース色混合", &material.RimAlbedoBlend, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("ライト色混合", &material.RimLightBlend, 0.0f, 1.0f);
            if (ImGui::Button("リムを標準値に戻す"))
            {
                material.RimStrength = 0.45f;
                material.RimThreshold = 0.70f;
                material.RimSoftness = 0.055f;
                material.RimPower = 1.0f;
                material.RimColor = {0.38f, 0.48f, 0.80f};
                material.RimAlbedoBlend = 0.20f;
                material.RimLightBlend = 0.35f;
                changed = true;
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("部位別"))
        {
            const char* partNames[] = {"透明",
                                       "髪",
                                       "服",
                                       "肌",
                                       "トゥーン",
                                       "影",
                                       "メタリック",
                                       "セルフシャドウ",
                                       "ライティング",
                                       "目",
                                       "非ライティング",
                                       "PBR",
                                       "BRDF",
                                       "BTDF",
                                       "BSDF"};
            const int partValues[] = {
                static_cast<int>(ShaderClass::Transparent), static_cast<int>(ShaderClass::Hair),
                static_cast<int>(ShaderClass::Cloth),       static_cast<int>(ShaderClass::Skin),
                static_cast<int>(ShaderClass::Toon),        static_cast<int>(ShaderClass::Shadow),
                static_cast<int>(ShaderClass::Metallic),    static_cast<int>(ShaderClass::SelfShadow),
                static_cast<int>(ShaderClass::Lit),         static_cast<int>(ShaderClass::Eye),
                static_cast<int>(ShaderClass::Unlit),       static_cast<int>(ShaderClass::PBR),
                static_cast<int>(ShaderClass::BRDF),        static_cast<int>(ShaderClass::BTDF),
                static_cast<int>(ShaderClass::BSDF),
            };

            for (int i = 0; i < IM_ARRAYSIZE(partValues); ++i)
            {
                const int part = partValues[i];
                if (part < 0 || part >= kMaterialPartParamCount)
                {
                    continue;
                }

                ImGui::PushID(part);
                if (ImGui::TreeNode(partNames[i]))
                {
                    auto& params = material.PartParams[part];
                    changed |= ImGui::SliderFloat("リム強度", &params.RimStrength, 0.0f, 3.0f);
                    changed |= ImGui::SliderFloat("リムしきい値", &params.RimThreshold, 0.0f, 1.0f);
                    changed |= ImGui::SliderFloat("リムぼかし", &params.RimSoftness, 0.001f, 0.50f, "%.3f");
                    changed |= ImGui::SliderFloat("リムカーブ", &params.RimPower, 0.05f, 6.0f, "%.2f");
                    changed |= ImGui::ColorEdit3("リム色", &params.RimColor.x, ImGuiColorEditFlags_Float);
                    changed |= ImGui::SliderFloat("ベース色混合", &params.RimAlbedoBlend, 0.0f, 1.0f);
                    changed |= ImGui::SliderFloat("ライト色混合", &params.RimLightBlend, 0.0f, 1.0f);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (changed)
    {
        BeginUndoCapture(m_SelectedEntity, before);
    }
    ImGui::End();
}

void ImGuiManager::DrawMaterialInspector(EntityID entity)
{
    if (ComponentManager::HasComponent<MaterialComponent>(entity) &&
        !ComponentManager::GetComponentUnchecked<MaterialComponent>(entity).MaterialSlots.empty())
    {
        ImGui::TextWrapped("共有マテリアルはMaterials欄からアセットを開いて編集してください。");
        return;
    }

    if (!ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return;
    }

    if (!ImGui::CollapsingHeader("マテリアル", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    EntitySnapshot before = CaptureEntity(entity);
    bool changed = false;

    const char* shaderModes[] = {"自動", "手動"};
    int shaderClassMode = static_cast<int>(material.ShaderClassMode);
    if (ImGui::Combo("シェーダークラスモード", &shaderClassMode, shaderModes, IM_ARRAYSIZE(shaderModes)))
    {
        material.ShaderClassMode = static_cast<MaterialMode>(shaderClassMode);
        changed = true;
    }
    const bool isManualMode = material.ShaderClassMode == MaterialMode::Manual;

    const char* shaderClasses[] = {"Transparent", "Hair", "Cloth", "Skin", "Toon", "Unlit", "Metallic",
                                   "Lit",         "Eye",  "PBR",   "BRDF", "BTDF", "BSDF"};
    const int shaderClassValues[] = {static_cast<int>(ShaderClass::Transparent), static_cast<int>(ShaderClass::Hair),
                                     static_cast<int>(ShaderClass::Cloth),       static_cast<int>(ShaderClass::Skin),
                                     static_cast<int>(ShaderClass::Toon),        static_cast<int>(ShaderClass::Unlit),
                                     static_cast<int>(ShaderClass::Metallic),    static_cast<int>(ShaderClass::Lit),
                                     static_cast<int>(ShaderClass::Eye),         static_cast<int>(ShaderClass::PBR),
                                     static_cast<int>(ShaderClass::BRDF),        static_cast<int>(ShaderClass::BTDF),
                                     static_cast<int>(ShaderClass::BSDF)};
    int shaderClassIndex = 5;
    for (int idx = 0; idx < IM_ARRAYSIZE(shaderClassValues); ++idx)
    {
        if (static_cast<int>(material.ShaderClass) == shaderClassValues[idx])
        {
            shaderClassIndex = idx;
            break;
        }
    }
    if (isManualMode)
    {
        if (ImGui::Combo("シェーダークラス", &shaderClassIndex, shaderClasses, IM_ARRAYSIZE(shaderClasses)))
        {
            const ShaderClass newShaderClass = static_cast<ShaderClass>(shaderClassValues[shaderClassIndex]);
            material.ShaderClass = newShaderClass;
            if (material.Alpha >= 0.999f)
            {
                if (newShaderClass == ShaderClass::BTDF)
                {
                    material.Alpha = 0.45f;
                }
                else if (newShaderClass == ShaderClass::BSDF)
                {
                    material.Alpha = 0.65f;
                }
                else if (newShaderClass == ShaderClass::Transparent)
                {
                    material.Alpha = 0.5f;
                }
            }
            changed = true;
        }
        changed |= ImGui::SliderFloat("Metallic", &material.Metallic, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Roughness", &material.Roughness, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Fresnel", &material.Fresnel, 0.0f, 1.0f);
    }
    changed |= ImGui::SliderFloat("Alpha", &material.Alpha, 0.0f, 1.0f);
    int shadowMode = static_cast<int>(material.ShadowMode);
    if (ImGui::Combo("Shadow Mode", &shadowMode, "None\0Opaque\0AlphaTest\0Transmittance\0Auto\0"))
    {
        material.ShadowMode = static_cast<MaterialShadowMode>(shadowMode);
        changed = true;
    }
    if (material.ShadowMode == MaterialShadowMode::AlphaTest)
    {
        changed |= ImGui::SliderFloat("Shadow Cutoff", &material.ShadowCutoff, 0.0f, 1.0f);
    }
    const bool opticsChanged =
        MaterialOpticsUI::Draw(material.IOR, material.Transmission, material.Roughness, material.TransmissionRoughness,
                               material.Thickness, &material.AbsorptionCoefficient.x);
    changed |= opticsChanged;
    if (opticsChanged)
    {
        const float ratio = (material.IOR - 1.0f) / (material.IOR + 1.0f);
        material.Fresnel = ratio * ratio;
    }
    if (opticsChanged && material.Transmission > 0.0f)
    {
        material.IsTransparent = true;
        material.AssetSurface = MaterialSurface::Transparent;
        material.AssetDepthWrite = false;
        material.ShaderClassMode = MaterialMode::Manual;
        material.ShaderClass = ShaderClass::PBR;
    }

    ImGui::SeparatorText("セクション");
    const char* outlineModes[] = {"押し出し", "TEO", "MIX"};
    int outlineMode = static_cast<int>(material.ToonOutlineRenderMode);
    if (ImGui::Combo("アウトラインモード", &outlineMode, outlineModes, IM_ARRAYSIZE(outlineModes)))
    {
        material.ToonOutlineRenderMode = static_cast<ToonOutlineMode>(outlineMode);
        changed = true;
    }
    const char* teoModes[] = {"バランス", "境界", "ハードエッジ", "クリーン"};
    int teoMode = static_cast<int>(material.ToonTeoRenderMode);
    if (ImGui::Combo("TEOモード", &teoMode, teoModes, IM_ARRAYSIZE(teoModes)))
    {
        material.ToonTeoRenderMode = static_cast<ToonTeoMode>(teoMode);
        changed = true;
    }
    const char* outlineWidthModes[] = {"ワールド単位", "スクリーンピクセル"};
    int outlineWidthMode = static_cast<int>(material.ToonOutlineWidthModeSetting);
    if (ImGui::Combo("幅モード", &outlineWidthMode, outlineWidthModes, IM_ARRAYSIZE(outlineWidthModes)))
    {
        material.ToonOutlineWidthModeSetting = static_cast<ToonOutlineWidthMode>(outlineWidthMode);
        changed = true;
    }
    changed |= ImGui::SliderFloat("アウトライン幅", &material.ToonOutlineWidth, 0.0f, 0.20f);
    changed |= ImGui::SliderFloat("スクリーン幅px", &material.ToonOutlineScreenWidth, 0.0f, 24.0f, "%.1f");
    changed |= ImGui::SliderFloat("TEO幅スケール", &material.ToonOutlineTeoWidthScale, 0.0f, 3.0f);

    if (changed)
    {
        BeginUndoCapture(entity, before);
    }

    if (!isManualMode && IsModelMaterialEntity(entity))
    {
        if (ImGui::CollapsingHeader("シェーディングパラメータ", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool partChanged = false;
            for (int i = 0; i < IM_ARRAYSIZE(shaderClassValues); ++i)
            {
                const int classId = shaderClassValues[i];
                if (classId >= 0 && classId < kMaterialPartParamCount)
                {
                    partChanged |= DrawMaterialPartParams(shaderClasses[i], material.PartParams[classId]);
                }
            }
            if (partChanged)
            {
                BeginUndoCapture(entity, before);
            }
        }
    }

    if (isManualMode && IsModelMaterialEntity(entity))
    {
        if (ImGui::CollapsingHeader("ディファード Toon / PBR", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool toonChanged = false;
            toonChanged |= ImGui::SliderFloat("法線ブレンド", &material.NormalBlend, 0.0f, 1.0f);
            toonChanged |= ImGui::SliderFloat("法線バイアス", &material.NormalBias, -1.0f, 1.0f);
            toonChanged |= ImGui::SliderFloat("ベース彩度", &material.BaseSaturation, 0.0f, 3.0f);
            toonChanged |= ImGui::SliderFloat("ベース明度", &material.BaseBrightness, 0.0f, 3.0f);
            toonChanged |= ImGui::SliderFloat("かわいいブレンド", &material.KawaiiBlend, 0.0f, 1.0f);
            if (toonChanged)
            {
                BeginUndoCapture(entity, before);
            }
        }

        if (ImGui::CollapsingHeader("影", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool shadowChanged = false;
            shadowChanged |= ImGui::SliderFloat("影しきい値", &material.ShadowThreshold, 0.0f, 1.0f);
            shadowChanged |= ImGui::SliderFloat("影ぼかし", &material.ShadowSoftness, 0.0f, 0.5f);
            shadowChanged |= ImGui::SliderFloat("影の強さ", &material.ShadowStrength, 0.0f, 2.0f);
            shadowChanged |= ImGui::SliderFloat("中間色の強さ", &material.MidStrength, 0.0f, 2.0f);
            shadowChanged |= ImGui::SliderFloat("明部の強さ", &material.LitStrength, 0.0f, 2.0f);
            shadowChanged |= ImGui::SliderFloat("影しきい値", &material.CastShadowThreshold, 0.0f, 1.0f);
            shadowChanged |= ImGui::SliderFloat("影ぼかし", &material.CastShadowSoftness, 0.0f, 0.5f);
            if (shadowChanged)
            {
                BeginUndoCapture(entity, before);
            }
        }

        if (ImGui::CollapsingHeader("ライティング", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool lightingChanged = false;
            lightingChanged |= ImGui::SliderFloat("リム強度", &material.RimStrength, 0.0f, 2.0f);
            lightingChanged |= ImGui::SliderFloat("リムしきい値", &material.RimThreshold, 0.0f, 1.0f);
            lightingChanged |= ImGui::SliderFloat("リムぼかし", &material.RimSoftness, 0.001f, 0.50f, "%.3f");
            lightingChanged |= ImGui::SliderFloat("リムカーブ", &material.RimPower, 0.05f, 6.0f, "%.2f");
            lightingChanged |= ImGui::ColorEdit3("リム色", &material.RimColor.x, ImGuiColorEditFlags_Float);
            lightingChanged |= ImGui::SliderFloat("ベース色混合", &material.RimAlbedoBlend, 0.0f, 1.0f);
            lightingChanged |= ImGui::SliderFloat("ライト色混合", &material.RimLightBlend, 0.0f, 1.0f);
            lightingChanged |= ImGui::SliderFloat("スペキュラ強度", &material.SpecularStrength, 0.0f, 2.0f);
            lightingChanged |= ImGui::SliderFloat("スペキュラしきい値", &material.SpecularThreshold, 0.0f, 1.0f);
            if (lightingChanged)
            {
                BeginUndoCapture(entity, before);
            }
        }

        if (ImGui::CollapsingHeader("肌", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool skinChanged = false;
            skinChanged |= ImGui::SliderFloat("肌散乱強度", &material.SkinScatterStrength, 0.0f, 3.0f);
            skinChanged |= ImGui::SliderFloat("肌散乱ラップ", &material.SkinScatterWrap, 0.0f, 1.0f);
            skinChanged |= ImGui::SliderFloat("肌逆光強度", &material.SkinBacklightStrength, 0.0f, 3.0f);
            skinChanged |= ImGui::SliderFloat("肌リム散乱強度", &material.SkinRimScatterStrength, 0.0f, 3.0f);
            skinChanged |= ImGui::SliderFloat("スペキュラ強度", &material.SkinOilSpecularStrength, 0.0f, 3.0f);
            skinChanged |= ImGui::SliderFloat("肌影散乱", &material.SkinShadowScatter, 0.0f, 1.0f);
            if (skinChanged)
            {
                BeginUndoCapture(entity, before);
            }
        }
    }

    bool useTexture = material.UseTexture != 0;
    if (ImGui::Checkbox("テクスチャ使用", &useTexture))
    {
        BeginUndoCapture(entity, before);
        material.UseTexture = static_cast<int>(useTexture);
    }
    if (ImGui::Checkbox("透明", &material.IsTransparent))
    {
        BeginUndoCapture(entity, before);
    }
    if (ImGui::Checkbox("両面 (Mesh Shader)", &material.DoubleSided))
    {
        BeginUndoCapture(entity, before);
    }
    if (ImGui::Checkbox("ポストプロセスを受ける", &material.ReceivingPostProcess))
    {
        BeginUndoCapture(entity, before);
    }
    ImGui::Text("テクスチャID: %d", material.TextureID);
    const char* statusText;
    if (material.TexturePath.empty())
    {
        statusText = "(なし)";
    }
    else
    {
        statusText = material.TexturePath.c_str();
    }
    ImGui::Text("テクスチャパス: %s", statusText);

    static EntityID editingEntity = g_kINVALID_ENTITY;
    static char texturePath[260]{};
    if (editingEntity != entity)
    {
        editingEntity = entity;
        strncpy_s(texturePath, material.TexturePath.c_str(), _TRUNCATE);
    }

    ImGui::InputText("テクスチャパス", texturePath, IM_ARRAYSIZE(texturePath));
    if (ImGui::Button("テクスチャ適用"))
    {
        BeginUndoCapture(entity, before);
        MaterialSystem::SetTexture(entity, texturePath);
    }
    ImGui::SameLine();
    if (ImGui::Button("テクスチャ解除"))
    {
        texturePath[0] = '\0';
        BeginUndoCapture(entity, before);
        MaterialSystem::SetTexture(entity, "");
    }

    if (ImGui::BeginDragDropTarget())
    {
        if (const auto* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
        {
            if (payload->DataSize == sizeof(AssetDragPayload))
            {
                const auto* asset = static_cast<const AssetDragPayload*>(payload->Data);
                if (asset->Type == AssetType::Texture)
                {
                    const auto path = MaterialManager::Registry().Resolve(asset->Handle).generic_string();
                    if (!path.empty())
                    {
                        BeginUndoCapture(entity, before);
                        MaterialSystem::SetTexture(entity, path.c_str());
                    }
                }
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            const char* path = static_cast<const char*>(payload->Data);
            if (path && IsTextureFile(path))
            {
                strncpy_s(texturePath, path, _TRUNCATE);
                BeginUndoCapture(entity, before);
                MaterialSystem::SetTexture(entity, path);
            }
        }
        ImGui::EndDragDropTarget();
    }

    UINT width = 0;
    UINT height = 0;
    if (TextureManager::GetTextureSize(material.TextureID, width, height))
    {
        ImGui::Text("サイズ: %u x %u", width, height);
        D3D12_GPU_DESCRIPTOR_HANDLE handle = FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(material.TextureID) * FrameConstants::GetCbvIncrementSize();
        ImGui::Image((ImTextureID)handle.ptr, ImVec2(96.0f, 96.0f));
    }
}

void ImGuiManager::DrawToonMeshOutlineInspector(EntityID entity, bool embeddedInInspector)
{
    if (!ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return;
    }

    auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);

    // Resolve components/resources once for this draw, rather than once per table row.
    const bool hasStaticModel = ComponentManager::HasComponent<StaticModelComponent>(entity);
    const auto* staticModel = hasStaticModel ? ModelManager::GetStaticModel(
        ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId) : nullptr;
    const auto* animationModel = ComponentManager::HasComponent<AnimationModelComponent>(entity) ?
        ModelManager::GetAnimModel(ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity).ModelId) : nullptr;
    const UINT meshCount = hasStaticModel ? (staticModel ? staticModel->GetMeshCount() : 0) :
                                           (animationModel ? animationModel->GetMeshCount() : 0);
    const auto* resourceMap = staticModel ? &staticModel->GetResourceMap() :
                              (animationModel ? &animationModel->GetResourceMap() : nullptr);
    auto getStaticMesh = [&](UINT index) -> const StaticMeshData* {
        return staticModel && index < staticModel->GetMeshCount() ? &staticModel->GetMeshData(index) : nullptr;
    };
    auto getAnimMesh = [&](UINT index) -> const MeshData* {
        return animationModel && index < animationModel->GetMeshCount() ? &animationModel->GetMeshData(index) : nullptr;
    };

    if (meshCount == 0)
    {
        if (!embeddedInInspector)
        {
            ImGui::TextUnformatted("メッシュがありません");
        }
        return;
    }

    bool opened = true;
    if (embeddedInInspector)
    {
        opened = ImGui::TreeNode("メッシュアウトライン上書き");
    }
    if (!opened)
    {
        return;
    }

    EntitySnapshot before = CaptureEntity(entity);
    bool changed = false;
    auto ensureOverrideSize = [&]() {
        if (material.ToonMeshOutlineOverrides.size() < meshCount)
        {
            material.ToonMeshOutlineOverrides.resize(meshCount, MeshOutlineOverride::Auto);
        }
    };
    auto ensureWidthScaleSize = [&]() {
        if (material.ToonMeshOutlineWidthScales.size() < meshCount)
        {
            material.ToonMeshOutlineWidthScales.resize(meshCount, 1.0f);
        }
    };

    if (ImGui::Button("すべて自動"))
    {
        material.ToonMeshOutlineOverrides.assign(meshCount, MeshOutlineOverride::Auto);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("すべてオン"))
    {
        material.ToonMeshOutlineOverrides.assign(meshCount, MeshOutlineOverride::ForceOn);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("すべてオフ"))
    {
        material.ToonMeshOutlineOverrides.assign(meshCount, MeshOutlineOverride::ForceOff);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("幅リセット"))
    {
        material.ToonMeshOutlineWidthScales.assign(meshCount, 1.0f);
        changed = true;
    }

    const char* overrideItems[] = {"自動", "オン", "オフ"};
    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

    float tableHeight;
    if (embeddedInInspector)
    {
        tableHeight = 260.0f;
    }
    else
    {
        tableHeight = max(180.0f, ImGui::GetContentRegionAvail().y);
    }
    if (ImGui::BeginTable("MeshOutlineOverrideTable", 7, tableFlags, ImVec2(0.0f, tableHeight)))
    {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupColumn("メッシュ");
        ImGui::TableSetupColumn("マテリアル");
        ImGui::TableSetupColumn("部位", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("自動", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("上書き", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("幅倍率", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableHeadersRow();

        ForEachVisibleModelMesh(resourceMap, meshCount, [&](uint32_t i) {
            const char* meshName = "(unnamed)";
            const char* materialName = "(none)";
            float materialPartId = 10.0f;
            bool defaultOutline = true;
            if (const auto* mesh = getStaticMesh(i))
            {
                meshName = mesh->MeshName.empty() ? "(unnamed)" : mesh->MeshName.c_str();
                materialName = mesh->MaterialName.empty() ? "(none)" : mesh->MaterialName.c_str();
                materialPartId = mesh->MaterialPartId;
                defaultOutline = mesh->DefaultToonOutlineEnabled;
            }
            else if (const auto* mesh = getAnimMesh(i))
            {
                meshName = mesh->MeshName.empty() ? "(unnamed)" : mesh->MeshName.c_str();
                materialName = mesh->MaterialName.empty() ? "(none)" : mesh->MaterialName.c_str();
                materialPartId = mesh->MaterialPartId;
                defaultOutline = mesh->DefaultToonOutlineEnabled;
            }

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(meshName);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(materialName);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.0f", materialPartId);
            ImGui::TableSetColumnIndex(4);
            const char* statusTextValue;
            if (defaultOutline)
            {
                statusTextValue = "オン";
            }
            else
            {
                statusTextValue = "オフ";
            }
            ImGui::TextUnformatted(statusTextValue);
            ImGui::TableSetColumnIndex(5);

            MeshOutlineOverride overrideValue = MeshOutlineOverride::Auto;
            if (i < material.ToonMeshOutlineOverrides.size())
            {
                overrideValue = material.ToonMeshOutlineOverrides[i];
            }
            int overrideIndex = static_cast<int>(overrideValue);
            if (ImGui::SetNextItemWidth(-FLT_MIN),
                ImGui::Combo("##Override", &overrideIndex, overrideItems, IM_ARRAYSIZE(overrideItems)))
            {
                ensureOverrideSize();
                material.ToonMeshOutlineOverrides[i] = static_cast<MeshOutlineOverride>(overrideIndex);
                changed = true;
            }
            ImGui::TableSetColumnIndex(6);
            float widthScale = 1.0f;
            if (i < material.ToonMeshOutlineWidthScales.size())
            {
                widthScale = material.ToonMeshOutlineWidthScales[i];
            }
            if (ImGui::SetNextItemWidth(-FLT_MIN), ImGui::SliderFloat("##WidthScale", &widthScale, 0.0f, 3.0f, "%.2f"))
            {
                ensureWidthScaleSize();
                material.ToonMeshOutlineWidthScales[i] = widthScale;
                changed = true;
            }
            ImGui::PopID();
        });
        ImGui::EndTable();
    }

    if (changed)
    {
        BeginUndoCapture(entity, before);
    }
    if (embeddedInInspector)
    {
        ImGui::TreePop();
    }
}

void ImGuiManager::ApplyMeshShadingOverridesToModel(EntityID entity)
{
    if (!Registry::IsAlive(entity) || !ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return;
    }

    auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    vector<int> overridePartIds;
    overridePartIds.reserve(material.MeshShadingOverrides.size());
    for (MeshShadingOverride value : material.MeshShadingOverrides)
    {
        overridePartIds.push_back(static_cast<int>(value));
    }
    if (!overridePartIds.empty() && IsModelMaterialEntity(entity))
    {
        material.ShaderClassMode = MaterialMode::Auto;
    }

    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
    {
        const auto& comp = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
        if (auto* model = ModelManager::GetStaticModel(comp.ModelId))
        {
            model->ApplyMeshShadingOverridePartIds(GraphicsDevice::GetDevice(), overridePartIds);
        }
    }
    else if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
    {
        const auto& comp = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        if (auto* model = ModelManager::GetAnimModel(comp.ModelId))
        {
            model->ApplyMeshShadingOverridePartIds(overridePartIds);
        }
    }
}

void ImGuiManager::DrawMeshShadingInspector(EntityID entity, bool embeddedInInspector)
{
    if (!ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return;
    }

    auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);

    // Resolve components/resources once for this draw, rather than once per table row.
    const bool hasStaticModel = ComponentManager::HasComponent<StaticModelComponent>(entity);
    const auto* staticModel = hasStaticModel ? ModelManager::GetStaticModel(
        ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId) : nullptr;
    const auto* animationModel = ComponentManager::HasComponent<AnimationModelComponent>(entity) ?
        ModelManager::GetAnimModel(ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity).ModelId) : nullptr;
    const UINT meshCount = hasStaticModel ? (staticModel ? staticModel->GetMeshCount() : 0) :
                                           (animationModel ? animationModel->GetMeshCount() : 0);
    const auto* resourceMap = staticModel ? &staticModel->GetResourceMap() :
                              (animationModel ? &animationModel->GetResourceMap() : nullptr);
    auto getStaticMesh = [&](UINT index) -> const StaticMeshData* {
        return staticModel && index < staticModel->GetMeshCount() ? &staticModel->GetMeshData(index) : nullptr;
    };
    auto getAnimMesh = [&](UINT index) -> const MeshData* {
        return animationModel && index < animationModel->GetMeshCount() ? &animationModel->GetMeshData(index) : nullptr;
    };

    if (meshCount == 0)
    {
        if (!embeddedInInspector)
        {
            ImGui::TextUnformatted("メッシュがありません");
        }
        return;
    }

    bool opened = true;
    if (embeddedInInspector)
    {
        opened = ImGui::TreeNode("メッシュシェーディング上書き");
    }
    if (!opened)
    {
        return;
    }

    EntitySnapshot before = CaptureEntity(entity);
    bool changed = false;
    auto ensureOverrideSize = [&]() {
        if (material.MeshShadingOverrides.size() < meshCount)
        {
            material.MeshShadingOverrides.resize(meshCount, MeshShadingOverride::Auto);
        }
    };

    if (ImGui::Button("すべて自動"))
    {
        material.MeshShadingOverrides.assign(meshCount, MeshShadingOverride::Auto);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("すべて髪##MeshShading"))
    {
        material.MeshShadingOverrides.assign(meshCount, MeshShadingOverride::Hair);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("すべて肌##MeshShading"))
    {
        material.MeshShadingOverrides.assign(meshCount, MeshShadingOverride::Skin);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("すべてPBR##MeshShading"))
    {
        material.MeshShadingOverrides.assign(meshCount, MeshShadingOverride::PBR);
        changed = true;
    }

    const char* shadingItems[] = {"自動",         "透明",       "髪",
                                  "服",           "肌",         "トゥーン",
                                  "影",           "メタリック", "セルフシャドウ",
                                  "ライティング", "目",         "非ライティング",
                                  "PBR",          "BRDF",       "BTDF",
                                  "BSDF"};
    const int shadingValues[] = {
        static_cast<int>(MeshShadingOverride::Auto),       static_cast<int>(MeshShadingOverride::Transparent),
        static_cast<int>(MeshShadingOverride::Hair),       static_cast<int>(MeshShadingOverride::Cloth),
        static_cast<int>(MeshShadingOverride::Skin),       static_cast<int>(MeshShadingOverride::Toon),
        static_cast<int>(MeshShadingOverride::Shadow),     static_cast<int>(MeshShadingOverride::Metallic),
        static_cast<int>(MeshShadingOverride::SelfShadow), static_cast<int>(MeshShadingOverride::Lit),
        static_cast<int>(MeshShadingOverride::Eye),        static_cast<int>(MeshShadingOverride::Unlit),
        static_cast<int>(MeshShadingOverride::PBR),        static_cast<int>(MeshShadingOverride::BRDF),
        static_cast<int>(MeshShadingOverride::BTDF),       static_cast<int>(MeshShadingOverride::BSDF),
    };

    auto getAutoLabel = [](float materialPartId) -> const char* {
        const int part = static_cast<int>(materialPartId + 0.5f);
        switch (part)
        {
            case static_cast<int>(ShaderClass::Hair):
                return "髪";
            case static_cast<int>(ShaderClass::Cloth):
                return "服";
            case static_cast<int>(ShaderClass::Skin):
                return "肌";
            case static_cast<int>(ShaderClass::Toon):
                return "トゥーン";
            case static_cast<int>(ShaderClass::PBR):
                return "PBR";
            case static_cast<int>(ShaderClass::BRDF):
                return "BRDF";
            case static_cast<int>(ShaderClass::BTDF):
                return "BTDF";
            case static_cast<int>(ShaderClass::BSDF):
                return "BSDF";
            default:
                return "自動";
        }
    };

    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

    float tableHeight;
    if (embeddedInInspector)
    {
        tableHeight = 260.0f;
    }
    else
    {
        tableHeight = max(180.0f, ImGui::GetContentRegionAvail().y);
    }
    if (ImGui::BeginTable("MeshShadingOverrideTable", 5, tableFlags, ImVec2(0.0f, tableHeight)))
    {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupColumn("メッシュ");
        ImGui::TableSetupColumn("マテリアル");
        ImGui::TableSetupColumn("部位", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("シェーディング", ImGuiTableColumnFlags_WidthFixed, 128.0f);
        ImGui::TableHeadersRow();

        ForEachVisibleModelMesh(resourceMap, meshCount, [&](uint32_t i) {
            const char* meshName = "(unnamed)";
            const char* materialName = "(none)";
            float materialPartId = 10.0f;
            if (const auto* mesh = getStaticMesh(i))
            {
                meshName = mesh->MeshName.empty() ? "(unnamed)" : mesh->MeshName.c_str();
                materialName = mesh->MaterialName.empty() ? "(none)" : mesh->MaterialName.c_str();
                materialPartId = mesh->MaterialPartId;
            }
            else if (const auto* mesh = getAnimMesh(i))
            {
                meshName = mesh->MeshName.empty() ? "(unnamed)" : mesh->MeshName.c_str();
                materialName = mesh->MaterialName.empty() ? "(none)" : mesh->MaterialName.c_str();
                materialPartId = mesh->MaterialPartId;
            }

            MeshShadingOverride overrideValue = MeshShadingOverride::Auto;
            if (i < material.MeshShadingOverrides.size())
            {
                overrideValue = material.MeshShadingOverrides[i];
            }
            int comboIndex = 0;
            for (int valueIndex = 0; valueIndex < IM_ARRAYSIZE(shadingValues); ++valueIndex)
            {
                if (static_cast<int>(overrideValue) == shadingValues[valueIndex])
                {
                    comboIndex = valueIndex;
                    break;
                }
            }

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(meshName);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(materialName);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.0f", materialPartId);
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##ShadingOverride", &comboIndex, shadingItems, IM_ARRAYSIZE(shadingItems)))
            {
                ensureOverrideSize();
                material.MeshShadingOverrides[i] = static_cast<MeshShadingOverride>(shadingValues[comboIndex]);
                changed = true;
            }
            ImGui::PopID();
        });
        ImGui::EndTable();
    }

    if (changed)
    {
        material.ShaderClassMode = MaterialMode::Auto;
        ApplyMeshShadingOverridesToModel(entity);
        BeginUndoCapture(entity, before);
    }
    if (embeddedInInspector)
    {
        ImGui::TreePop();
    }
}

void ImGuiManager::DrawLightInspector(EntityID entity)
{
    if (!ComponentManager::HasComponent<LightComponent>(entity))
    {
        return;
    }

    if (!ImGui::CollapsingHeader("ライト", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
    EntitySnapshot before = CaptureEntity(entity);
    bool changed = false;
    const char* lightTypes[] = {"Directional", "Point", "Spot", "Volume", "Rect (エリア)"};
    int typeIndex = static_cast<int>(light.Type);
    if (ImGui::Combo("ライトタイプ", &typeIndex, lightTypes, IM_ARRAYSIZE(lightTypes)))
    {
        changed = true;
        light.Type = static_cast<LightType>(typeIndex);
        RefreshLightEntityName(entity, light.Type);
        ApplyLightEntityToRuntime(entity);
    }

    changed |= ImGui::Checkbox("有効", &light.IsActive);
    changed |= ImGui::Checkbox("デバッグ描画", &light.DrawDebug);
    const char* renderModes[] = {"Physical", "Decal (軽量)", "Emission Only"};
    int renderMode = static_cast<int>(light.RenderMode);
    if (ImGui::Combo("照明経路", &renderMode, renderModes, IM_ARRAYSIZE(renderModes)))
    {
        light.RenderMode = static_cast<LightRenderMode>(renderMode);
        if (light.RenderMode != LightRenderMode::Physical)
        {
            light.CastShadow = false;
            light.AffectsVolumetrics = false;
            light.VolumeDensity = 0.0f;
            light.AffectsForward = false;
        }
        if (light.RenderMode == LightRenderMode::EmissionOnly)
        {
            light.AffectsOpaque = false;
        }
        changed = true;
    }
    changed |= ImGui::Checkbox("不透明に影響", &light.AffectsOpaque);
    changed |= ImGui::Checkbox("Forwardに影響", &light.AffectsForward);
    changed |= ImGui::Checkbox("大気・ボリュームに影響", &light.AffectsVolumetrics);
    changed |= ImGui::DragFloat("ライト優先度", &light.Priority, 0.05f, -10.0f, 10.0f);
    ImGui::BeginDisabled(light.RenderMode != LightRenderMode::Physical);
    changed |= ImGui::Checkbox("影を描画", &light.CastShadow);
    ImGui::EndDisabled();
    if (light.Type == LightType::Point || light.Type == LightType::Spot || light.Type == LightType::Rect)
    {
        ImGui::BeginDisabled(light.RenderMode != LightRenderMode::Physical);
        changed |= ImGui::Checkbox("MegaLights を許可", &light.AllowMegaLights);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("環境設定で MegaLights が有効なとき、不透明面をクラスターライトグリッドで評価します。\n"
                              "シャドウマップを割り当てられたライトは従来の経路のままです。");
        }
        if (light.Type != LightType::Rect)
        {
            changed |= ImGui::DragFloat("ソース半径", &light.SourceRadius, 0.005f, 0.0f, 5.0f, "%.3f m");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "MegaLights（M2 / リファレンス）の影レイで、発光球の大きさに応じたソフトシャドウになります。");
            }
        }
    }
    if (light.Type == LightType::Point || light.Type == LightType::Spot || light.Type == LightType::Rect)
    {
        char iesPath[1024]{};
        strncpy_s(iesPath, light.IesProfilePath.c_str(), _TRUNCATE);
        if (ImGui::InputText("IESプロファイル", iesPath, sizeof(iesPath), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            light.IesProfilePath = iesPath;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("参照##IES"))
        {
            filesystem::path path;
            if (ProceduralIO::OpenFileDialog(L"IESプロファイル (*.ies)\0*.ies\0", path))
            {
                light.IesProfilePath = MakeRelativeAssetPath(path);
                changed = true;
            }
        }
        if (!light.IesProfilePath.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("解除##IES"))
            {
                light.IesProfilePath.clear();
                changed = true;
            }
            changed |= ImGui::SliderFloat("IES回転", &light.IesRotationDegrees, -180.0f, 180.0f, "%.1f deg");
        }
        ImGui::TextDisabled("Type C / TILT=NONE。ライト方向を配光の正面として使用。");
    }
    changed |= ImGui::ColorEdit3("色", &light.Color.x);
    changed |= ImGui::DragFloat("ライト強度", &light.Intensity, 0.01f, 0.0f, 20.0f);
    changed |= ImGui::DragFloat("範囲", &light.Range, 0.05f, 0.1f, 100.0f);
    changed |= ImGui::DragFloat3("ライト方向", &light.Direction.x, 0.01f, -1.0f, 1.0f);

    if (light.Type == LightType::Spot)
    {
        changed |= ImGui::DragFloat("内角", &light.InnerAngle, 0.2f, 0.1f, 89.0f);
        changed |= ImGui::DragFloat("外角", &light.OuterAngle, 0.2f, light.InnerAngle + 0.1f, 89.5f);
    }

    if (light.Type == LightType::Rect)
    {
        // 発光面は「ライト方向」を法線とする片面の矩形（LTC で評価）。
        changed |= ImGui::DragFloat("幅", &light.AreaWidth, 0.01f, 0.01f, 50.0f, "%.2f m");
        changed |= ImGui::DragFloat("高さ", &light.AreaHeight, 0.01f, 0.01f, 50.0f, "%.2f m");
        // MegaLights M7：発光面のテクスチャ（アセットブラウザからテクスチャをドロップ）
        char areaTexture[260]{};
        strncpy_s(areaTexture, light.AreaTexturePath.c_str(), _TRUNCATE);
        if (ImGui::InputText("発光テクスチャ", areaTexture, sizeof(areaTexture), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            light.AreaTexturePath = areaTexture;
            changed = true;
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetPayloadType))
            {
                AssetDragPayload asset{};
                if (payload->DataSize == sizeof(asset))
                {
                    memcpy(&asset, payload->Data, sizeof(asset));
                    const filesystem::path resolved = MaterialManager::Registry().Resolve(asset.Handle);
                    if (asset.Type == AssetType::Texture && !resolved.empty())
                    {
                        light.AreaTexturePath = MakeRelativeAssetPath(resolved);
                        changed = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("MegaLights のグリッドで評価するとき、矩形ライトの発光面にテクスチャを貼ります（M7）。");
        }
        if (!light.AreaTexturePath.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("X##AreaTexture"))
            {
                light.AreaTexturePath.clear();
                changed = true;
            }
        }
    }
    if (light.Type == LightType::Volume)
    {
        const char* volumeShapes[] = {"円錐", "円柱"};
        int volumeShape = light.VolumeShape;
        if (ImGui::Combo("ボリューム形状", &volumeShape, volumeShapes, IM_ARRAYSIZE(volumeShapes)))
        {
            light.VolumeShape = volumeShape;
            changed = true;
        }
        changed |= ImGui::DragFloat("内角", &light.InnerAngle, 0.2f, 0.1f, 89.0f);
        changed |= ImGui::DragFloat("外角", &light.OuterAngle, 0.2f, light.InnerAngle + 0.1f, 89.5f);
        changed |= ImGui::DragFloat("ボリューム密度", &light.VolumeDensity, 0.01f, 0.0f, 30.0f);
    }
    else if (light.AffectsVolumetrics)
    {
        changed |= ImGui::DragFloat("ボリューム密度", &light.VolumeDensity, 0.01f, 0.0f, 3.0f);
    }

    if (ImGui::Button("メインライトに設定"))
    {
        changed = true;
        light.IsActive = true;
    }

    if (changed)
    {
        BeginUndoCapture(entity, before);
        ApplyLightEntityToRuntime(entity);
    }

    if (ComponentManager::HasComponent<SunComponent>(entity) &&
        ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& sun = ComponentManager::GetComponentUnchecked<SunComponent>(entity);
        bool sunChanged = false;
        sunChanged |= DrawAxisFloat3("注視点", sun.Target, 0.05f);
        ImGui::TextWrapped(
            "仮想の太陽です。位置からターゲットへ向かう平行光を放ちます。空の太陽は大気設定で描画します。");
        sunChanged |= ImGui::Checkbox("Directional同期", &sun.SyncDirectionalLight);
        if (sunChanged)
        {
            Sun::Sync(entity);
        }
    }
}

void ImGuiManager::DrawComponentInspector(EntityID entity)
{
    const bool isRenderable3D = ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
                                ComponentManager::HasComponent<StaticModelComponent>(entity) ||
                                ComponentManager::HasComponent<MeshComponent>(entity) ||
                                (ComponentManager::HasComponent<SpriteComponent>(entity) &&
                                 ComponentManager::GetComponentUnchecked<SpriteComponent>(entity).Is3D);

    if (ImGui::CollapsingHeader("コンポーネント"))
    {
        ImGui::Text("エンティティID: %u", entity);
        const char* statusText3;
        if (ComponentManager::HasComponent<NameComponent>(entity))
        {
            statusText3 = ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name.c_str();
        }
        else
        {
            statusText3 = "(none)";
        }
        ImGui::Text("名前: %s", statusText3);
        if (!ComponentManager::HasComponent<AABBComponent>(entity) && ImGui::Button("AABB追加"))
        {
            XMFLOAT3 center{};
            XMFLOAT3 extents{};
            const bool hasModelAabb = GetLocalAabb(entity, center, extents);
            ComponentManager::AddComponent(entity, ComponentType::AABB);
            if (hasModelAabb)
            {
                auto& aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
                aabb.Center = center;
                aabb.Extents = extents;
            }
        }
        const bool isLightEntity = ComponentManager::HasComponent<LightComponent>(entity);
        if (!isLightEntity && !ComponentManager::HasComponent<MaterialComponent>(entity) &&
            ImGui::Button("マテリアル追加"))
        {
            ComponentManager::AddComponent(entity, ComponentType::MATERIAL);
            auto& mat = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
            mat.TextureID = TextureManager::GetDefaultTextureIndex();
            mat.UseTexture = false;
        }
        if (isRenderable3D && !ComponentManager::HasComponent<LODComponent>(entity) && ImGui::Button("LOD追加"))
        {
            PushUndoSnapshot(CaptureEntity(entity));
            ComponentManager::AddComponent(entity, ComponentType::LOD);
        }
        const char* statusText4;
        if (ComponentManager::HasComponent<MeshComponent>(entity))
        {
            statusText4 = "あり";
        }
        else
        {
            statusText4 = "なし";
        }
        ImGui::Text("メッシュ: %s", statusText4);
        if (ComponentManager::HasComponent<StaticModelComponent>(entity))
        {
            auto& staticModel = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
            ImGui::Text("静的モデル: あり");
            ImGui::Checkbox("座標変換", &staticModel.IsConvert);
        }
        else
        {
            ImGui::Text("静的モデル: なし");
        }
        if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
        {
            auto& animModel = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
            ImGui::Text("アニメーションモデル: あり");
            ImGui::Checkbox("座標変換", &animModel.IsConvert);
        }
        else
        {
            ImGui::Text("アニメーションモデル: なし");
        }
        const char* statusText5;
        if (ComponentManager::HasComponent<SpriteComponent>(entity))
        {
            statusText5 = "あり";
        }
        else
        {
            statusText5 = "なし";
        }
        ImGui::Text("スプライト: %s", statusText5);
    }

    if (ComponentManager::HasComponent<CameraComponent>(entity) &&
        ImGui::CollapsingHeader("カメラ", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const EntitySnapshot before = CaptureEntity(entity);
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(entity);
        bool changed = false;
        const bool isEditorCamera = entity == Camera::GetEditorCameraEntity();
        const bool isActive = entity == Camera::GetCameraEntity();
        ImVec4 statusColor;
        if (isActive)
        {
            statusColor = ImVec4(0.25f, 0.9f, 0.4f, 1.0f);
        }
        else
        {
            statusColor = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
        }
        const char* statusText6;
        if (isActive)
        {
            statusText6 = "現在の描画カメラ";
        }
        else
        {
            statusText6 = "非アクティブ";
        }
        ImGui::TextColored(statusColor, statusText6);
        const char* statusText7;
        if (isEditorCamera)
        {
            statusText7 = "種類: EditorCamera";
        }
        else
        {
            statusText7 = "種類: GameCamera";
        }
        ImGui::TextUnformatted(statusText7);

        if (!isEditorCamera)
        {
            if (!camera.IsMainGameCamera)
            {
                if (ImGui::Button("メインGameCameraに設定"))
                {
                    Camera::SetMainGameCamera(entity);
                    changed = true;
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.25f, 0.9f, 0.4f, 1.0f), "Main GameCamera");
            }
            changed |= ImGui::InputInt("優先度", &camera.Priority);

            ImGui::SeparatorText("VMDカメラアニメーション");
            ImGui::BeginDisabled(ProjectManager::IsPlaying());
            char vmdPath[1024]{};
            strncpy_s(vmdPath, camera.Animation.Path.c_str(), _TRUNCATE);
            if (ImGui::InputText("カメラVMD", vmdPath, sizeof(vmdPath), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                camera.Animation.Path = vmdPath;
                camera.Animation.Error.clear();
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("参照##CameraVmd"))
            {
                filesystem::path path;
                if (ProceduralIO::OpenFileDialog(L"カメラモーション (*.vmd)\0*.vmd\0", path))
                {
                    camera.Animation.Path = MakeRelativeAssetPath(path);
                    camera.Animation.Error.clear();
                    changed = true;
                }
            }
            if (!camera.Animation.Path.empty())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("解除##CameraVmd"))
                {
                    camera.Animation.Path.clear();
                    camera.Animation.Error.clear();
                    changed = true;
                }
            }
            changed |= ImGui::Checkbox("再生開始時にVMDを再生", &camera.Animation.PlayOnStart);
            changed |= ImGui::Checkbox("カメラVMDをループ", &camera.Animation.Loop);
            changed |= ImGui::DragFloat("カメラ再生速度", &camera.Animation.Speed, 0.01f, 0.0f, 10.0f);
            changed |= ImGui::DragFloat("VMD座標の倍率", &camera.Animation.Scale, 0.01f, 0.001f, 1000.0f);
            changed |= ImGui::DragFloat3("VMD位置補正", &camera.Animation.Offset.x, 0.01f);
            camera.Animation.Speed = max(0.0f, camera.Animation.Speed);
            camera.Animation.Scale = max(0.001f, camera.Animation.Scale);
            ImGui::EndDisabled();
            ImGui::TextDisabled("再生開始時のメインGameCameraに適用します。");
            if (!camera.Animation.Error.empty())
            {
                ImGui::TextWrapped("%s", camera.Animation.Error.c_str());
            }
        }

        changed |= ImGui::Checkbox("ユーザー操作を許可", &camera.AllowUserControl);
        changed |= ImGui::DragFloat3("注視点", &camera.Target.x, 0.01f);
        changed |= ImGui::Checkbox("平行投影", &camera.IsOrthographic);
        if (camera.IsOrthographic)
        {
            changed |= ImGui::DragFloat("表示範囲の高さ", &camera.OrthographicHeight, 0.01f, 0.001f, 100000.0f);
            camera.OrthographicHeight = max(0.001f, camera.OrthographicHeight);
        }
        float fovDegrees = XMConvertToDegrees(camera.Fov);
        if (ImGui::SliderFloat("視野角 (度)", &fovDegrees, 1.0f, 179.0f))
        {
            camera.Fov = XMConvertToRadians(fovDegrees);
            changed = true;
        }
        changed |= ImGui::DragFloat("Near Clip", &camera.NearClip, 0.01f, 0.001f, 1000.0f);
        changed |= ImGui::DragFloat(camera.IsOrthographic ? "Far Clip" : "Effect Range", &camera.FarClip, 1.0f, 0.01f, 1000000.0f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Perspective projection uses an infinite far plane (Reverse-Z).");
        }
        camera.NearClip = max(0.001f, camera.NearClip);
        camera.FarClip = max(camera.NearClip + 0.001f, camera.FarClip);

        int lockTarget;
        if (camera.LockOnTarget == g_kINVALID_ENTITY)
        {
            lockTarget = -1;
        }
        else
        {
            lockTarget = static_cast<int>(camera.LockOnTarget);
        }
        if (ImGui::InputInt("追従Entity ID (-1=なし)", &lockTarget))
        {
            if (lockTarget < 0)
            {
                camera.LockOnTarget = g_kINVALID_ENTITY;
            }
            else
            {
                camera.LockOnTarget = static_cast<EntityID>(lockTarget);
            }
            changed = true;
        }
        changed |= ImGui::DragFloat3("追従オフセット", &camera.LockOnOffset.x, 0.01f);
        changed |= ImGui::Checkbox("ポストプロセス有効", &camera.EnablePostProcess);

        if (ComponentManager::HasComponent<PostProcessComponent>(entity))
        {
            auto& post = ComponentManager::GetComponentUnchecked<PostProcessComponent>(entity);
            const char* postNames[] = {"なし", "ブラー", "セピア", "グレースケール", "反転", "ブルーム"};
            int type = static_cast<int>(post.Type);
            if (ImGui::Combo("ポストプロセス", &type, postNames, IM_ARRAYSIZE(postNames)))
            {
                post.Type = static_cast<PostProcessType>(clamp(type, 0, static_cast<int>(PostProcessType::COUNT) - 1));
                changed = true;
            }
            float maxPostProcessIntensity;
            if (post.Type == PostProcessType::BLOOM)
            {
                maxPostProcessIntensity = 5.0f;
            }
            else
            {
                maxPostProcessIntensity = 1.0f;
            }
            changed |= ImGui::SliderFloat("エフェクト強度", &post.Intensity, 0.0f, maxPostProcessIntensity);
            if (post.Type == PostProcessType::BLOOM)
            {
                changed |= ImGui::SliderFloat("Bloom しきい値", &post.BloomThreshold, 0.0f, 16.0f);
                changed |= ImGui::SliderFloat("Bloom Soft Knee", &post.BloomSoftKnee, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Bloom 半径", &post.BloomRadius, 0.25f, 4.0f);
            }
        }

        if (ComponentManager::HasComponent<MoveComponent>(entity))
        {
            auto& move = ComponentManager::GetComponentUnchecked<MoveComponent>(entity);
            changed |= ImGui::Checkbox("移動可能", &move.CanMove);
            changed |= ImGui::DragFloat("移動速度", &move.Speed, 0.01f, 0.0f, 1000.0f);
            changed |= ImGui::DragFloat("回転速度", &move.RotationSpeed, 0.001f, 0.0f, 100.0f);
        }
        ImGui::TextWrapped("EditorCamera: 右ドラッグ + WASDQE、Shiftで高速移動");
        if (changed)
        {
            BeginUndoCapture(entity, before);
        }
    }

    if (!ComponentManager::HasComponent<TimelineComponent>(entity))
    {
        ImGui::SeparatorText("Timeline");
        ImGui::TextWrapped("TimelineComponentが設定されていません。追加しますか?");
        if (ImGui::Button("TimelineComponentを追加"))
        {
            PushUndoSnapshot(CaptureEntity(entity));
            ComponentManager::AddComponent(entity, ComponentType::TIMELINE);
        }
    }

    if (ComponentManager::HasComponent<TimelineComponent>(entity) &&
        ImGui::CollapsingHeader("Timeline", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const EntitySnapshot before = CaptureEntity(entity);
        auto& timeline = ComponentManager::GetComponentUnchecked<TimelineComponent>(entity);
        bool changed = false;
        bool evaluate = false;

        changed |= ImGui::DragFloat("長さ (秒)", &timeline.Duration, 0.1f, 0.01f, 36000.0f);
        changed |= ImGui::DragFloat("再生速度", &timeline.Speed, 0.01f, -100.0f, 100.0f);
        changed |= ImGui::Checkbox("Play On Awake", &timeline.PlayOnAwake);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Loop", &timeline.Loop);
        timeline.Duration = max(0.01f, timeline.Duration);

        const char* statusText8;
        if (timeline.IsPlaying)
        {
            statusText8 = "一時停止##Timeline";
        }
        else
        {
            statusText8 = "再生##Timeline";
        }
        if (ImGui::Button(statusText8))
        {
            timeline.IsPlaying = !timeline.IsPlaying;
        }
        ImGui::SameLine();
        if (ImGui::Button("停止##Timeline"))
        {
            timeline.IsPlaying = false;
            timeline.CurrentTime = 0.0f;
            evaluate = true;
        }
        if (ImGui::SliderFloat("再生ヘッド", &timeline.CurrentTime, 0.0f, timeline.Duration, "%.3f 秒"))
        {
            timeline.IsPlaying = false;
            evaluate = true;
        }

        const char* propertyNames[] = {
            "Transform / Position",   "Transform / Rotation", "Transform / Scale", "Camera / Target",
            "Camera / FOV",           "Camera / Near Clip",   "Camera / Far Clip", "Camera / Lock-on Offset",
            "PostProcess / Intensity"};
        if (ImGui::Button("+ Track"))
        {
            TimelineTrackData track{};
            track.Target = entity;
            track.Name = string("Track ") + to_string(timeline.Tracks.size() + 1);
            bool valid = false;
            track.DefaultValue = TimeLineSystem::ReadProperty(track.Target, track.Property, &valid);
            track.HasDefaultValue = valid;
            timeline.Tracks.push_back(move(track));
            changed = true;
        }

        for (size_t trackIndex = 0; trackIndex < timeline.Tracks.size();)
        {
            auto& track = timeline.Tracks[trackIndex];
            ImGui::PushID(static_cast<int>(trackIndex));
            bool removeTrack = false;
            if (ImGui::TreeNodeEx("TrackNode", ImGuiTreeNodeFlags_DefaultOpen, "%s  (%s)", track.Name.c_str(),
                                  propertyNames[static_cast<int>(track.Property)]))
            {
                changed |= ImGui::Checkbox("有効##Track", &track.Enabled);
                char trackName[128]{};
                strncpy_s(trackName, track.Name.c_str(), _TRUNCATE);
                if (ImGui::InputText("Track名", trackName, IM_ARRAYSIZE(trackName)))
                {
                    track.Name = trackName;
                    changed = true;
                }
                int targetId;
                if (track.Target == g_kINVALID_ENTITY)
                {
                    targetId = -1;
                }
                else
                {
                    targetId = static_cast<int>(track.Target);
                }
                if (ImGui::InputInt("対象Entity ID", &targetId))
                {
                    if (targetId < 0)
                    {
                        track.Target = g_kINVALID_ENTITY;
                    }
                    else
                    {
                        track.Target = static_cast<EntityID>(targetId);
                    }
                    changed = true;
                }
                int property = static_cast<int>(track.Property);
                if (ImGui::Combo("パラメーター", &property, propertyNames, IM_ARRAYSIZE(propertyNames)))
                {
                    track.Property = static_cast<TimelineProperty>(clamp(property, 0, IM_ARRAYSIZE(propertyNames) - 1));
                    bool valid = false;
                    track.DefaultValue = TimeLineSystem::ReadProperty(track.Target, track.Property, &valid);
                    track.HasDefaultValue = valid;
                    changed = true;
                }
                if (ImGui::Button("+ Clip"))
                {
                    TimelineClipData clip{};
                    clip.Name = string("Clip ") + to_string(track.Clips.size() + 1);
                    clip.StartTime = timeline.CurrentTime;
                    clip.Duration = min(1.0f, max(0.01f, timeline.Duration - clip.StartTime));
                    bool valid = false;
                    const XMFLOAT4 value = TimeLineSystem::ReadProperty(track.Target, track.Property, &valid);
                    if (valid)
                    {
                        clip.Keys.push_back({0.0f, value, TimelineInterpolation::Linear});
                        clip.Keys.push_back({clip.Duration, value, TimelineInterpolation::Linear});
                    }
                    track.Clips.push_back(move(clip));
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Track削除"))
                {
                    removeTrack = true;
                }

                for (size_t clipIndex = 0; !removeTrack && clipIndex < track.Clips.size();)
                {
                    auto& clip = track.Clips[clipIndex];
                    ImGui::PushID(static_cast<int>(clipIndex));
                    bool removeClip = false;
                    if (ImGui::TreeNodeEx("ClipNode", ImGuiTreeNodeFlags_DefaultOpen, "%s  %.2f - %.2f 秒",
                                          clip.Name.c_str(), clip.StartTime, clip.StartTime + clip.Duration))
                    {
                        changed |= ImGui::Checkbox("有効##Clip", &clip.Enabled);
                        char clipName[128]{};
                        strncpy_s(clipName, clip.Name.c_str(), _TRUNCATE);
                        if (ImGui::InputText("Clip名", clipName, IM_ARRAYSIZE(clipName)))
                        {
                            clip.Name = clipName;
                            changed = true;
                        }
                        changed |= ImGui::DragFloat("開始 (秒)", &clip.StartTime, 0.01f, 0.0f, timeline.Duration);
                        float endTime = clip.StartTime + clip.Duration;
                        if (ImGui::DragFloat("終了 (秒)", &endTime, 0.01f, clip.StartTime + 0.001f, timeline.Duration))
                        {
                            clip.Duration = endTime - clip.StartTime;
                            changed = true;
                        }
                        clip.StartTime = clamp(clip.StartTime, 0.0f, timeline.Duration);
                        clip.Duration = clamp(clip.Duration, 0.001f, max(0.001f, timeline.Duration - clip.StartTime));
                        changed |= ImGui::DragFloat("Blend In", &clip.BlendIn, 0.01f, 0.0f, clip.Duration);
                        changed |= ImGui::DragFloat("Blend Out", &clip.BlendOut, 0.01f, 0.0f, clip.Duration);
                        clip.BlendIn = clamp(clip.BlendIn, 0.0f, clip.Duration);
                        clip.BlendOut = clamp(clip.BlendOut, 0.0f, clip.Duration);

                        if (ImGui::Button("+ Key (現在値)"))
                        {
                            bool valid = false;
                            const XMFLOAT4 value = TimeLineSystem::ReadProperty(track.Target, track.Property, &valid);
                            if (valid)
                            {
                                clip.Keys.push_back({clamp(timeline.CurrentTime - clip.StartTime, 0.0f, clip.Duration),
                                                     value, TimelineInterpolation::Linear});
                                sort(clip.Keys.begin(), clip.Keys.end(), [](const auto& a, const auto& b) {
                                    return a.Time < b.Time;
                                });
                                changed = true;
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Clip削除"))
                        {
                            removeClip = true;
                        }

                        for (size_t keyIndex = 0; !removeClip && keyIndex < clip.Keys.size();)
                        {
                            auto& key = clip.Keys[keyIndex];
                            ImGui::PushID(static_cast<int>(keyIndex));
                            bool removeKey = false;
                            if (ImGui::TreeNode("KeyNode", "Key %.3f 秒", key.Time))
                            {
                                changed |= ImGui::DragFloat("時刻", &key.Time, 0.01f, 0.0f, clip.Duration);
                                key.Time = clamp(key.Time, 0.0f, clip.Duration);
                                const bool vectorProperty = track.Property == TimelineProperty::TransformPosition ||
                                                            track.Property == TimelineProperty::TransformRotation ||
                                                            track.Property == TimelineProperty::TransformScale ||
                                                            track.Property == TimelineProperty::CameraTarget ||
                                                            track.Property == TimelineProperty::CameraLockOnOffset;
                                if (vectorProperty)
                                {
                                    changed |= ImGui::DragFloat3("値", &key.Value.x, 0.01f);
                                }
                                else
                                {
                                    changed |= ImGui::DragFloat("値", &key.Value.x, 0.01f);
                                }
                                const char* easing[] = {"Step", "Linear", "Ease In", "Ease Out", "Ease In Out"};
                                int interpolation = static_cast<int>(key.Interpolation);
                                if (ImGui::Combo("補間", &interpolation, easing, IM_ARRAYSIZE(easing)))
                                {
                                    key.Interpolation = static_cast<TimelineInterpolation>(clamp(interpolation, 0, 4));
                                    changed = true;
                                }
                                if (ImGui::Button("Key削除"))
                                {
                                    removeKey = true;
                                }
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                            if (removeKey)
                            {
                                clip.Keys.erase(clip.Keys.begin() + keyIndex);
                                changed = true;
                            }
                            else
                            {
                                ++keyIndex;
                            }
                        }
                        if (changed)
                        {
                            sort(clip.Keys.begin(), clip.Keys.end(), [](const auto& a, const auto& b) {
                                return a.Time < b.Time;
                            });
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                    if (removeClip)
                    {
                        track.Clips.erase(track.Clips.begin() + clipIndex);
                        changed = true;
                    }
                    else
                    {
                        ++clipIndex;
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
            if (removeTrack)
            {
                timeline.Tracks.erase(timeline.Tracks.begin() + trackIndex);
                changed = true;
            }
            else
            {
                ++trackIndex;
            }
        }

        if (evaluate || (!ProjectManager::IsSimulationRunning() && changed))
        {
            TimeLineSystem::EvaluateComponent(timeline);
        }
        if (changed)
        {
            BeginUndoCapture(entity, before);
        }
    }

    if (!ComponentManager::HasComponent<PhysicsComponent>(entity))
    {
        ImGui::SeparatorText("物理");
        ImGui::TextWrapped("PhysicsComponentが設定されていません。追加しますか?");
        if (ImGui::Button("PhysicsComponentを追加"))
        {
            PushUndoSnapshot(CaptureEntity(entity));
            ComponentManager::AddComponent(entity, ComponentType::PHYSICS);
        }
    }

    if (ComponentManager::HasComponent<PhysicsComponent>(entity) &&
        ImGui::CollapsingHeader("物理設定", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const EntitySnapshot before = CaptureEntity(entity);
        auto& physics = ComponentManager::GetComponentUnchecked<PhysicsComponent>(entity);
        bool changed = false;

        changed |= ImGui::Checkbox("物理を使用", &physics.UsePhysics);
        const bool hasAnimationModel = ComponentManager::HasComponent<AnimationModelComponent>(entity);
        ImGui::BeginDisabled(!hasAnimationModel);
        changed |= ImGui::Checkbox("PMXボーン物理を使用", &physics.UsePhysicsBone);
        ImGui::EndDisabled();
        if (!hasAnimationModel && physics.UsePhysicsBone)
        {
            physics.UsePhysicsBone = false;
            changed = true;
        }

        const char* engineNames[] = {"Bullet Physics", "Jolt Physics", "NVIDIA PhysX"};
        int engine = static_cast<int>(physics.UsePhysicsEngine);
        if (ImGui::Combo("物理エンジン", &engine, engineNames, IM_ARRAYSIZE(engineNames)))
        {
            physics.UsePhysicsEngine = static_cast<PhysicsEngine>(clamp(engine, 0, 2));
            changed = true;
        }
        if (PhysicsSystem* physicsSystem = SystemManager::GetSystem<PhysicsSystem>())
        {
            const bool available = physicsSystem->IsBackendAvailable(physics.UsePhysicsEngine);
            ImGui::SameLine();
            ImVec4 statusColorValue;
            if (available)
            {
                statusColorValue = ImVec4(0.30f, 0.85f, 0.45f, 1.0f);
            }
            else
            {
                statusColorValue = ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
            }
            const char* statusText9;
            if (available)
            {
                statusText9 = "利用可能";
            }
            else
            {
                statusText9 = "初期化失敗";
            }
            ImGui::TextColored(statusColorValue, statusText9);
        }

        ImGui::SeparatorText("剛体");
        ImGui::TextUnformatted("当たり判定プリセット");
        auto applyColliderPreset = [&](PhysicsColliderRole role) {
            physics.UsePhysics = true;
            physics.UsePhysicsBone = false;
            physics.BodyType = PhysicsBodyType::Static;
            physics.ColliderRole = role;
            physics.UseGravity = false;
            XMFLOAT3 center{};
            XMFLOAT3 extents{};
            if (GetLocalAabb(entity, center, extents))
            {
                physics.ColliderCenter = center;
                physics.ColliderSize = {extents.x * 2.0f, extents.y * 2.0f, extents.z * 2.0f};
            }
            changed = true;
        };
        if (ImGui::Button("床"))
        {
            applyColliderPreset(PhysicsColliderRole::Floor);
            physics.Shape = PhysicsShape::Box;
        }
        ImGui::SameLine();
        if (ImGui::Button("壁"))
        {
            applyColliderPreset(PhysicsColliderRole::Wall);
            physics.Shape = PhysicsShape::Box;
        }
        ImGui::SameLine();
        if (ImGui::Button("障害物"))
        {
            applyColliderPreset(PhysicsColliderRole::Obstacle);
            physics.Shape = PhysicsShape::Box;
        }
        ImGui::SameLine();
        if (ImGui::Button("Mesh障害物"))
        {
            applyColliderPreset(PhysicsColliderRole::Obstacle);
            physics.Shape = PhysicsShape::Mesh;
        }
        const char* colliderRoles[] = {"Default", "Floor", "Wall", "Obstacle"};
        int colliderRole = static_cast<int>(physics.ColliderRole);
        if (ImGui::Combo("Collider Role", &colliderRole, colliderRoles, IM_ARRAYSIZE(colliderRoles)))
        {
            physics.ColliderRole = static_cast<PhysicsColliderRole>(clamp(colliderRole, 0, 3));
            changed = true;
        }
        const char* bodyTypes[] = {"Static", "Dynamic", "Kinematic"};
        int bodyType = static_cast<int>(physics.BodyType);
        if (ImGui::Combo("Body Type", &bodyType, bodyTypes, IM_ARRAYSIZE(bodyTypes)))
        {
            physics.BodyType = static_cast<PhysicsBodyType>(clamp(bodyType, 0, 2));
            changed = true;
        }
        const char* shapes[] = {"Box", "Sphere", "Capsule", "Mesh (Convex)"};
        int shape = static_cast<int>(physics.Shape);
        if (ImGui::Combo("Collider Shape", &shape, shapes, IM_ARRAYSIZE(shapes)))
        {
            physics.Shape = static_cast<PhysicsShape>(clamp(shape, 0, 3));
            changed = true;
        }
        if (physics.Shape == PhysicsShape::Mesh)
        {
            const bool hasStaticGeometry = ComponentManager::HasComponent<StaticModelComponent>(entity);
            const char* statusText10;
            if (hasStaticGeometry)
            {
                statusText10 = "StaticModelの頂点から凸メッシュコライダーを生成します。";
            }
            else
            {
                statusText10 = "CPUメッシュ頂点がないためCollider SizeのBoxにフォールバックします。";
            }
            ImGui::TextWrapped(statusText10);
            if (physics.BodyType == PhysicsBodyType::Dynamic)
            {
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "動的Meshは凸形状として扱われます。");
            }
        }
        changed |= ImGui::DragFloat3("Collider Center", &physics.ColliderCenter.x, 0.01f);
        changed |= ImGui::DragFloat3("Collider Size", &physics.ColliderSize.x, 0.01f, 0.001f, 10000.0f);
        if (physics.Shape == PhysicsShape::Sphere)
        {
            changed |= ImGui::DragFloat("Sphere Radius", &physics.SphereRadius, 0.01f, 0.001f, 10000.0f);
        }
        if (physics.Shape == PhysicsShape::Capsule)
        {
            changed |= ImGui::DragFloat("Capsule Radius", &physics.CapsuleRadius, 0.01f, 0.001f, 10000.0f);
            changed |= ImGui::DragFloat("Capsule Height", &physics.CapsuleHeight, 0.01f, 0.001f, 10000.0f);
        }
        changed |= ImGui::DragFloat3("初速", &physics.Velocity.x, 0.01f);
        changed |= ImGui::DragFloat3("初期角速度", &physics.AngularVelocity.x, 0.01f);
        changed |= ImGui::DragFloat("質量", &physics.Mass, 0.01f, 0.0001f, 100000.0f);
        changed |= ImGui::SliderFloat("摩擦", &physics.Friction, 0.0f, 2.0f);
        changed |= ImGui::SliderFloat("反発", &physics.Restitution, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("線形減衰", &physics.LinearDamping, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("角減衰", &physics.AngularDamping, 0.0f, 1.0f);
        changed |= ImGui::Checkbox("重力を使用", &physics.UseGravity);
        ImGui::BeginDisabled(!physics.UseGravity);
        changed |= ImGui::DragFloat("重力倍率", &physics.GravityFactor, 0.01f, -10.0f, 10.0f);
        ImGui::EndDisabled();
        changed |= ImGui::Checkbox("Continuous Collision Detection", &physics.EnableContinuousCollision);
        changed |= ImGui::Checkbox("Sleepを許可", &physics.AllowSleeping);

        if (hasAnimationModel)
        {
            ImGui::SeparatorText("PMXボーン");
            changed |= ImGui::DragFloat("PMX 質量倍率", &physics.PmxMassScale, 0.01f, 0.001f, 100.0f);
            changed |= ImGui::DragFloat("PMX 減衰倍率", &physics.PmxDampingScale, 0.01f, 0.0f, 100.0f);
            changed |= ImGui::DragFloat("PMX バネ倍率", &physics.PmxStiffnessScale, 0.01f, 0.0f, 100.0f);
            changed |= ImGui::DragFloat("PMX Collider倍率", &physics.PmxColliderScale, 0.01f, 0.001f, 100.0f);
        }

        int collisionLayer = physics.CollisionLayer;
        int collisionMask = physics.CollisionMask;
        ImGui::SeparatorText("Collision Filter");
        if (ImGui::InputInt("Collision Layer", &collisionLayer))
        {
            physics.CollisionLayer = static_cast<uint16_t>(clamp(collisionLayer, 0, 15));
            changed = true;
        }
        if (ImGui::InputInt("Collision Mask", &collisionMask))
        {
            physics.CollisionMask = static_cast<uint16_t>(clamp(collisionMask, 0, 65535));
            changed = true;
        }

        if (changed)
        {
            ++physics.SettingsRevision;
            BeginUndoCapture(entity, before);
        }
    }

    if (ComponentManager::HasComponent<LODComponent>(entity) &&
        ImGui::CollapsingHeader("LOD", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const EntitySnapshot before = CaptureEntity(entity);
        auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
        bool changed = ImGui::Checkbox("GPU LODを使用", &lod.UseLOD);
        changed |= ImGui::DragFloat("LOD1距離", &lod.Lod1Distance, 0.25f, 0.0f, 100000.0f, "%.2f");
        changed |= ImGui::DragFloat("LOD2距離", &lod.Lod2Distance, 0.25f, 0.0f, 100000.0f, "%.2f");
        lod.Lod1Distance = max(0.0f, lod.Lod1Distance);
        lod.Lod2Distance = max(lod.Lod1Distance, lod.Lod2Distance);
        if (ComponentManager::HasComponent<StaticModelComponent>(entity))
        {
            ImGui::SeparatorText("インポスター");
            changed |= ImGui::Checkbox("インポスターを使用", &lod.UseImpostor);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("切替距離より遠いとき、多視点で焼いた板1枚で描きます。\n"
                                  "影・レイトレーシングは実メッシュのままです。\n"
                                  "有効にすると GPU Driven ではなく CPU インスタンス描画経路になります。");
            }
            ImGui::BeginDisabled(!lod.UseImpostor);
            changed |= ImGui::DragFloat("切替距離", &lod.ImpostorDistance, 0.5f, 0.0f, 100000.0f, "%.1f m");
            lod.ImpostorDistance = max(0.0f, lod.ImpostorDistance);
            int framesIndex = lod.ImpostorFrames <= 4 ? 0 : (lod.ImpostorFrames <= 8 ? 1 : 2);
            if (ImGui::Combo("視点数", &framesIndex,
                             "4 x 4 (軽量)\0"
                             "8 x 8 (標準)\0"
                             "16 x 16 (高品質)\0"))
            {
                lod.ImpostorFrames = framesIndex == 0 ? 4u : (framesIndex == 1 ? 8u : 16u);
                changed = true;
            }
            int resolutionIndex = lod.ImpostorResolution <= 512 ? 0 : (lod.ImpostorResolution <= 1024 ? 1 : 2);
            if (ImGui::Combo("アトラス解像度", &resolutionIndex,
                             "512 (約 2.7 MB)\0"
                             "1024 (約 11 MB)\0"
                             "2048 (約 43 MB)\0"))
            {
                lod.ImpostorResolution = resolutionIndex == 0 ? 512u : (resolutionIndex == 1 ? 1024u : 2048u);
                changed = true;
            }
            changed |= ImGui::Checkbox("上半球のみ (地面に立つ物)", &lod.ImpostorHemisphere);
            const char* stateText = "未生成（切替距離を超えると生成）";
            switch (ImpostorRenderer::QueryState(entity))
            {
                case ImpostorRenderer::AtlasState::Pending:
                    stateText = "生成待ち";
                    break;
                case ImpostorRenderer::AtlasState::Ready:
                    stateText = "生成済み";
                    break;
                case ImpostorRenderer::AtlasState::Failed:
                    stateText = "生成失敗（メッシュで描画）";
                    break;
                default:
                    break;
            }
            ImGui::Text("状態: %s", stateText);
            if (ImGui::Button("再ベイク"))
            {
                if (StaticModelResource* impostorModel = ModelManager::GetStaticModel(
                        ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId))
                {
                    ImpostorRenderer::InvalidateModel(impostorModel);
                }
            }
            ImGui::EndDisabled();

            auto& impostorSettings = ImpostorRenderer::GetSettings();
            if (ImGui::TreeNode("インポスター全体設定"))
            {
                ImGui::Checkbox("インポスター描画を有効化", &impostorSettings.Enabled);
                ImGui::SliderFloat("被覆率しきい値", &impostorSettings.AlphaCutoff, 0.05f, 0.95f, "%.2f");
                ImGui::SliderFloat("Mip バイアス", &impostorSettings.MipBias, -1.0f, 2.0f, "%.2f");
                const auto stats = ImpostorRenderer::GetStatistics();
                ImGui::Text("アトラス: %u 生成済み / %u 待ち / %u 失敗", stats.ReadyAtlases, stats.PendingAtlases,
                            stats.FailedAtlases);
                ImGui::Text("VRAM: %.1f MB", static_cast<double>(stats.AtlasBytes) / (1024.0 * 1024.0));
                ImGui::Text("描画: %u インスタンス / %u ドローコール", stats.DrawnInstances, stats.DrawCalls);
                if (ImGui::Button("全アトラスを再生成"))
                {
                    ImpostorRenderer::InvalidateAll();
                }
                ImGui::TreePop();
            }
        }
        changed |= ImGui::Checkbox("Use asset LOD metadata", &lod.UseAssetSettings);
        StaticModelResource* lodModel =
            ComponentManager::HasComponent<StaticModelComponent>(entity)
                ? ModelManager::GetStaticModel(
                      ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId)
                : nullptr;
        if (lod.UseAssetSettings && lodModel)
        {
            lod.ScreenLOD = lodModel->GetLODMetadata();
        }
        bool screenChanged = false;
        int lodCount = int(lod.ScreenLOD.Count);
        if (ImGui::SliderInt("LOD Count", &lodCount, 1, 8))
        {
            lod.ScreenLOD.Count = uint32_t(lodCount);
            screenChanged = true;
        }
        screenChanged |= ImGui::SliderFloat("LOD Hysteresis", &lod.ScreenLOD.Hysteresis, 0, 0.5f);
        for (uint32_t level = 0; level < lod.ScreenLOD.Count; ++level)
        {
            ImGui::PushID(int(level));
            ImGui::Text("LOD%u", level);
            const float maximum = level == 0 ? 1.0f : lod.ScreenLOD.Thresholds[level - 1];
            if (lod.ScreenLOD.Thresholds[level] > maximum)
            {
                lod.ScreenLOD.Thresholds[level] = maximum;
                screenChanged = true;
            }
            screenChanged |=
                ImGui::SliderFloat("Screen Height Threshold", &lod.ScreenLOD.Thresholds[level], 0, maximum);
            if (level > 0)
            {
                char path[260]{};
                strncpy_s(path, lod.ScreenLOD.MeshPaths[level].c_str(), _TRUNCATE);
                if (ImGui::InputText("Mesh asset (Enter to apply)", path, sizeof(path),
                                     ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    lod.ScreenLOD.MeshPaths[level] = path;
                    screenChanged = true;
                }
            }
            ImGui::PopID();
        }
        if (screenChanged)
        {
            lod.UseAssetSettings = false;
            changed = true;
        }
        if (lodModel)
        {
            auto notifyAssetUsers = [&]() {
                for (uint32_t id = 0; id < Registry::GetEntities().size(); ++id)
                {
                    if (Registry::IsAlive(id) && ComponentManager::HasComponent<StaticModelComponent>(id) &&
                        ComponentManager::GetComponentUnchecked<StaticModelComponent>(id).ModelId ==
                            ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId)
                    {
                        GpuSceneChanges::Notify(id);
                    }
                }
            };
            if (ImGui::Button("Save LOD asset metadata") && Visibility::ValidLOD(lod.ScreenLOD))
            {
                ofstream file(lodModel->GetSourcePath() + ".lod");
                if (Visibility::SaveLOD(file, lod.ScreenLOD))
                {
                    lodModel->SetLODMetadata(lod.ScreenLOD);
                    lod.UseAssetSettings = true;
                    changed = true;
                    notifyAssetUsers();
                }
                else
                {
                    Debug::Log("LOD metadata save failed\n");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload LOD asset metadata"))
            {
                ifstream file(lodModel->GetSourcePath() + ".lod");
                if (Visibility::LoadLOD(file, lod.ScreenLOD))
                {
                    lodModel->SetLODMetadata(lod.ScreenLOD);
                    lod.UseAssetSettings = true;
                    changed = true;
                    notifyAssetUsers();
                }
            }
            ImGui::TextDisabled("Metadata: %s.lod", lodModel->GetSourcePath().c_str());
        }
        if (changed)
        {
            GpuSceneChanges::Notify(entity);
            BeginUndoCapture(entity, before);
        }
    }

    if (ComponentManager::HasComponent<SpriteComponent>(entity) && ImGui::CollapsingHeader("スプライト"))
    {
        auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        ImGui::Checkbox("3D", &sprite.Is3D);
        ImGui::Checkbox("UV変換", &sprite.UseUvTransform);
        ImGui::DragFloat2("UVオフセット", &sprite.UvOffset.x, 0.001f);
        ImGui::DragFloat2("UVスケール", &sprite.UvScale.x, 0.001f);
        ImGui::Checkbox("ポストプロセス使用", &sprite.UsePostProcess);
    }
}
