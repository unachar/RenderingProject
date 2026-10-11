#include "pch.h"
#include "dlssupscaler.h"
#include "imguimanager.h"
#include "proceduralscene.h"
#include "terrainediting.h"
#include "vertexdeformation.h"
#include "nrddenoiser.h"
#include "raytracing.h"
#include "world.h"

namespace
{
const char* kNoiseNames[] = { "Perlin", "Value", "Worley (Cellular)", "fBm", "Ridged fBm", "Domain Warp", "Composite / 複合ノイズ" };
const char* kMotionNames[] = { "Wave / 波", "Orbit / 軌道", "Noise / ゆらぎ" };

bool DrawNoiseParameters(Procedural::BasicNoiseSettings& noise)
{
    bool changed = ImGui::InputScalar("Seed", ImGuiDataType_U32, &noise.Seed);
    changed |= ImGui::SliderFloat("周波数", &noise.Frequency, 0.005f, 0.5f, "%.3f");
    changed |= ImGui::SliderInt("オクターブ", &noise.Octaves, 1, 8);
    changed |= ImGui::SliderFloat("振幅の減衰", &noise.Persistence, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("周波数の倍率", &noise.Lacunarity, 1.0f, 4.0f);
    changed |= ImGui::SliderFloat("Warp 強度", &noise.WarpStrength, 0.0f, 10.0f);
    return changed;
}

void DrawNoisePreview(const Procedural::TerrainSettings& settings, bool changed)
{
    constexpr int side = 48;
    static std::vector<float> preview;
    if (changed || preview.empty())
    {
        auto previewSettings = settings;
        previewSettings.Resolution = side - 1;
        previewSettings.Height = 0.5f;
        previewSettings.BaseHeight = 0.5f;
        preview = Procedural::BuildHeightMap(previewSettings);
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float extent = (std::max)(0.0f, (std::min)({ available.x, available.y, 240.0f }));
    const float cell = extent / side;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int row = 0; row < side; ++row)
    {
        for (int column = 0; column < side; ++column)
        {
            const float height = preview[row * side + column];
            const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(height, height, height, 1.0f));
            drawList->AddRectFilled(ImVec2(origin.x + column * cell, origin.y + row * cell),
                ImVec2(origin.x + (column + 1) * cell, origin.y + (row + 1) * cell), color);
        }
    }
    ImGui::Dummy(ImVec2(extent, extent));
}
}

void ImGuiManager::DrawProceduralWindow()
{
    ImGui::SetNextWindowSize(ImVec2(800.0f, 670.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(600.0f, 360.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("プロシージャルエディター", &m_ShowProceduralWindow))
    {
        ImGui::End();
        return;
    }
    static Procedural::TerrainSettings settings;
    static Procedural::TerrainEditState terrainEdit;
    static char presetPath[260] = "Save/terrain.proc";
    static std::string status;
    const float statusHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTabBar("ProceduralTabs"))
    {
        if (ImGui::BeginTabItem("地形・マップ"))
        {
            const bool selectedTerrain = Registry::IsAlive(m_SelectedEntity) &&
                ComponentManager::HasComponent<ProceduralTerrainComponent>(m_SelectedEntity);
            const auto* selectedSettings = selectedTerrain ?
                &ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(m_SelectedEntity).Settings : nullptr;
            const bool synchronized = terrainEdit.SynchronizeSelection(
                selectedTerrain ? m_SelectedEntity : Procedural::TerrainEditState::InvalidTarget, selectedSettings);
            if (synchronized && selectedTerrain)
            {
                settings = terrainEdit.GetSettings();
            }
            ImVec2 contentSize = ImGui::GetContentRegionAvail();
            contentSize.y = (std::max)(1.0f, contentSize.y - statusHeight - ImGui::GetStyle().ItemSpacing.y);
            const float previewWidth = std::clamp(contentSize.x * 0.32f, 160.0f, 260.0f);
            const float controlsWidth = contentSize.x - previewWidth - ImGui::GetStyle().ItemSpacing.x;
            ImGui::BeginChild("TerrainControls", ImVec2(controlsWidth, contentSize.y));
            bool liveUpdate = ProceduralScene::GetLiveUpdate();
            if (ImGui::Checkbox("選択した地形をリアルタイムで更新", &liveUpdate))
            {
                ProceduralScene::SetLiveUpdate(liveUpdate);
            }
            int method = static_cast<int>(settings.Noise.Method);
            bool changed = ImGui::Combo("生成手法", &method, kNoiseNames, IM_ARRAYSIZE(kNoiseNames)) || synchronized;
            settings.Noise.Method = static_cast<Procedural::NoiseMethod>(method);
            if (settings.Noise.Method == Procedural::NoiseMethod::Composite)
            {
                changed |= ImGui::SliderInt("合成するノイズ数", &settings.Noise.LayerCount, 2, 5);
                ImGui::SeparatorText("共通パラメータ");
                changed |= DrawNoiseParameters(settings.Noise);
                ImGui::TextWrapped("共通値を調整して一括反映してください。個別設定は反映するまで保持されます。");
                if (ImGui::Button("共通パラメータを全ノイズに反映"))
                {
                    Procedural::ApplyCommonNoiseParameters(settings.Noise);
                    changed = true;
                }
                ImGui::SeparatorText("個別パラメータ");
                ImGui::TextWrapped("合成比率で重み付き平均を取ります。比率が 0 のノイズは合成しません。");
                for (int index = 0; index < settings.Noise.LayerCount; ++index)
                {
                    ImGui::PushID(index);
                    auto& layer = settings.Noise.Layers[index];
                    if (ImGui::TreeNodeEx("Layer", ImGuiTreeNodeFlags_DefaultOpen, "ノイズ %d", index + 1))
                    {
                        int layerMethod = static_cast<int>(layer.Noise.Method);
                        changed |= ImGui::Combo("ノイズの種類", &layerMethod, kNoiseNames, IM_ARRAYSIZE(kNoiseNames) - 1);
                        layer.Noise.Method = static_cast<Procedural::NoiseMethod>(layerMethod);
                        changed |= ImGui::SliderFloat("合成比率", &layer.Weight, 0.0f, 5.0f);
                        changed |= DrawNoiseParameters(layer.Noise);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            else
            {
                changed |= DrawNoiseParameters(settings.Noise);
            }
            changed |= ImGui::SliderInt("解像度", &settings.Resolution, 4, 256);
            changed |= ImGui::SliderFloat("マップの幅", &settings.Size, 1.0f, 500.0f, "%.1f m");
            changed |= ImGui::SliderFloat("高さ", &settings.Height, 0.0f, 100.0f, "%.1f m");
            changed |= ImGui::SliderFloat("基準の高さ", &settings.BaseHeight, -100.0f, 100.0f, "%.1f m");
            ImGui::Text("三角形: %d", settings.Resolution * settings.Resolution * 2);
            if (ImGui::Button("新しい地形を生成"))
            {
                const EntityID created = ProceduralScene::GenerateTerrain(settings);
                if (created != g_kINVALID_ENTITY)
                {
                    EntitySnapshot before;
                    before.Entity = created;
                    PushUndoSnapshot(before);
                    m_SelectedEntity = created;
                    status = "地形を生成しました。";
                }
                else
                {
                    status = "地形の生成に失敗しました。";
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!selectedTerrain);
            if (ImGui::Button("選択した地形を更新"))
            {
                const auto before = CaptureEntity(m_SelectedEntity);
                if (ProceduralScene::GenerateTerrain(settings, m_SelectedEntity) != g_kINVALID_ENTITY)
                {
                    PushUndoSnapshot(before);
                    terrainEdit.SetSettings(settings);
                    terrainEdit.MarkApplied(ImGui::GetTime());
                    status = "地形を更新しました。Ctrl+Z で戻せます。";
                }
            }
            if (ImGui::Button("選択した地形の設定を取得"))
            {
                settings = ComponentManager::GetComponentUnchecked<ProceduralTerrainComponent>(m_SelectedEntity).Settings;
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::InputText("プリセットファイル", presetPath, sizeof(presetPath));
            if (ImGui::Button("設定を保存"))
            {
                status = "設定を保存できませんでした。保存先を確認してください。";
                if (ProceduralScene::SavePreset(presetPath, settings))
                {
                    status = "生成設定を保存しました。";
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("設定を読み込む"))
            {
                status = "設定ファイルを読み込めませんでした。";
                if (ProceduralScene::LoadPreset(presetPath, settings))
                {
                    status = "生成設定を読み込みました。";
                    changed = true;
                }
            }
            terrainEdit.SetSettings(settings);
            if (liveUpdate && selectedTerrain && terrainEdit.ShouldApply(ImGui::GetTime(), ImGui::IsAnyItemActive()))
            {
                const auto before = CaptureEntity(m_SelectedEntity);
                if (ProceduralScene::GenerateTerrain(settings, m_SelectedEntity) != g_kINVALID_ENTITY)
                {
                    BeginUndoCapture(m_SelectedEntity, before);
                    terrainEdit.MarkApplied(ImGui::GetTime());
                    status = "選択した地形に反映しました。Ctrl+Z で戻せます。";
                }
                else
                {
                    status = "地形を更新できませんでした。";
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("TerrainPreview", ImVec2(previewWidth, contentSize.y),
                ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::SeparatorText("ノイズプレビュー");
            DrawNoisePreview(settings, changed);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("動き"))
        {
            const bool selected = Registry::IsAlive(m_SelectedEntity) &&
                ComponentManager::HasComponent<TransformComponent>(m_SelectedEntity);
            if (!selected)
            {
                ImGui::TextWrapped("動かしたいオブジェクトをヒエラルキーで選択してください。");
            }
            else if (!ComponentManager::HasComponent<ProceduralMotionComponent>(m_SelectedEntity))
            {
                if (ImGui::Button("プロシージャルな動きを追加"))
                {
                    const auto before = CaptureEntity(m_SelectedEntity);
                    ComponentManager::AddComponent(m_SelectedEntity, ComponentType::PROCEDURAL_MOTION);
                    auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(m_SelectedEntity);
                    motion.Origin = ComponentManager::GetComponentUnchecked<TransformComponent>(m_SelectedEntity).Position;
                    PushUndoSnapshot(before);
                }
            }
            else
            {
                ImGui::Text("対象: %s", GetEntityDisplayName(m_SelectedEntity));
                auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(m_SelectedEntity);
                const auto before = CaptureEntity(m_SelectedEntity);
                const bool vertexSupported = !ComponentManager::HasComponent<AnimationModelComponent>(m_SelectedEntity) &&
                    (ComponentManager::HasComponent<MeshComponent>(m_SelectedEntity) ||
                        ComponentManager::HasComponent<StaticModelComponent>(m_SelectedEntity));
                int target = static_cast<int>(motion.Target);
                bool targetChanged = false;
                if (ImGui::BeginCombo("動かす対象", target == 0 ? "オブジェクト全体" : "メッシュの頂点"))
                {
                    if (ImGui::Selectable("オブジェクト全体", target == 0)) { target = 0; targetChanged = true; }
                    ImGui::BeginDisabled(!vertexSupported);
                    if (ImGui::Selectable("メッシュの頂点", target == 1)) { target = 1; targetChanged = true; }
                    ImGui::EndDisabled();
                    ImGui::EndCombo();
                }
                if (targetChanged)
                {
                    motion.Target = static_cast<Procedural::MotionTarget>(target);
                    ProceduralScene::ResetMotion(m_SelectedEntity);
                }
                if (motion.Target == Procedural::MotionTarget::MeshVertices && !vertexSupported)
                {
                    ImGui::TextWrapped("頂点モードは生成地形・プリミティブ・静的モデルに対応しています。この対象はオブジェクト全体のモードを使用してください。");
                }
                int method = static_cast<int>(motion.Method);
                bool changed = ImGui::Combo("動きの手法", &method, kMotionNames, IM_ARRAYSIZE(kMotionNames)) || targetChanged;
                motion.Method = static_cast<Procedural::MotionMethod>(method);
                changed |= ImGui::Checkbox("有効", &motion.Enabled);
                changed |= ImGui::Checkbox("編集画面でプレビュー", &motion.PreviewInEditor);
                changed |= ImGui::DragFloat3("振幅 X / Y / Z", &motion.Amplitude.x, 0.05f, -100.0f, 100.0f);
                changed |= ImGui::SliderFloat("速度 (周期/秒)", &motion.Speed, -2.0f, 2.0f);
                if (motion.Target == Procedural::MotionTarget::MeshVertices)
                {
                    changed |= ImGui::SliderFloat("頂点の空間周波数", &motion.SpatialFrequency, 0.001f, 4.0f, "%.3f");
                }
                changed |= ImGui::InputScalar("動きの Seed", ImGuiDataType_U32, &motion.Seed);
                changed |= ImGui::DragFloat3("原点", &motion.Origin.x, 0.1f);
                if (changed)
                {
                    BeginUndoCapture(m_SelectedEntity, before);
                }
                ImGui::Text("再生時間: %.2f 秒", motion.Time);
                if (ImGui::Button("停止して原点へ戻す"))
                {
                    PushUndoSnapshot(before);
                    motion.Enabled = false;
                    ProceduralScene::ResetMotion(m_SelectedEntity);
                }
                ImGui::SameLine();
                if (ImGui::Button("最初から再生"))
                {
                    PushUndoSnapshot(before);
                    ProceduralScene::ResetMotion(m_SelectedEntity);
                    motion.Enabled = true;
                    motion.PreviewInEditor = true;
                }
                ImGui::TextWrapped("Play で再生、Pause で一時停止、Stop で Play 前の状態へ戻ります。");
                ImGui::TextWrapped("物理を使う対象は Kinematic にしてください。Static / Dynamic は物理の位置を優先して動きを停止します。");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::BeginChild("ProceduralStatus", ImVec2(0.0f, statusHeight),
        ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::EndChild();
    ImGui::End();
}

void ImGuiManager::DrawRaytracingWindow()
{
    ImGui::SetNextWindowSize(ImVec2(440.0f, 370.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("レイトレーシング", &m_ShowRaytracingWindow))
    {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", Raytracing::GetStatus());
    ImGui::Text("加速構造のインスタンス: %u", Raytracing::GetInstanceCount());
    ImGui::BeginDisabled(!Raytracing::IsAvailable());
    auto& settings = Raytracing::GetSettings();
    ImGui::Checkbox("レイトレーシングを有効にする", &settings.Enabled);
    ImGui::Checkbox("全ライトの影", &settings.Shadows);
    ImGui::Checkbox("シーンのレイトレーシング反射", &settings.Reflections);
    ImGui::Checkbox("環境遮蔽 (AO)", &settings.AmbientOcclusion);
    ImGui::BeginDisabled(!NrdDenoiser::IsCompiled());
    ImGui::Checkbox("NVIDIA NRD で影のノイズを除去", &settings.Denoise);
    ImGui::EndDisabled();
    ImGui::Text("NRD: %s / GPU パス: %u", NrdDenoiser::GetVersion(), Raytracing::GetDenoiserDispatchCount());
    ImGui::SliderInt("サンプル数", &settings.Samples, 1, 8);
    ImGui::SliderFloat("影の最大距離", &settings.ShadowDistance, 10.0f, 2000.0f, "%.0f m");
    ImGui::SliderFloat("AO の半径", &settings.AmbientRadius, 0.05f, 10.0f, "%.2f m");
    ImGui::SliderFloat("表面からのオフセット", &settings.NormalBias, 0.001f, 0.1f, "%.3f m");
    ImGui::SliderFloat("太陽の角半径", &settings.SunAngularRadius, 0.0f, 0.05f, "%.4f rad");
    ImGui::EndDisabled();
    ImGui::TextWrapped("実三角形を使う太陽の影と AO です。透明面は対象外です。非対応 GPU は既存描画を使用します。");
    ImGui::Separator();
    ImGui::TextWrapped("DLSS: %s", DlssUpscaler::GetStatus());
    ImGui::TextWrapped("アップスケーラーは環境設定から選択できます。");
    ImGui::End();
}
