#include "pch.h"
#include "niagarawindow.h"
#include "imguimanager.h"
#include "editorworkspace.h"
#include "niagaragraph.h"
#include "graphvisuals.h"
#include "timelinewidgets.h"
#include "niagararenderer.h"
#include "../effects/niagarascene.h"
#include "proceduralio.h"
#include "../../External/imgui-node-editor/imgui_node_editor.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <optional>

namespace
{
    namespace Nodes = ax::NodeEditor;
    using namespace Niagara;
    constexpr wchar_t assetFilter[] = L"Niagara (*.naiagara)\0*.naiagara\0";

    // Niagaraウィンドウ内だけに適用し、他のエディターのテーマを変えない。
    struct EditorStyle
    {
        ImGuiDir OriginalMenuButtonPosition;

        EditorStyle() : OriginalMenuButtonPosition(ImGui::GetStyle().WindowMenuButtonPosition)
        {
            // 折り畳み機能を保ったまま、左の矢印を右側の最小化ボタンに置き換える。
            ImGui::GetStyle().WindowMenuButtonPosition = ImGuiDir_None;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6, 6});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6, 3});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6, 4});
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.075f, 0.075f, 0.075f, 1});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.105f, 0.105f, 0.105f, 1});
            ImGui::PushStyleColor(ImGuiCol_Border, {0.04f, 0.04f, 0.04f, 1});
            ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.055f, 0.055f, 0.055f, 1});
            ImGui::PushStyleColor(ImGuiCol_Header, {0.19f, 0.19f, 0.19f, 1});
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.25f, 0.29f, 0.34f, 1});
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0.12f, 0.32f, 0.54f, 1});
            ImGui::PushStyleColor(ImGuiCol_Button, {0.19f, 0.19f, 0.19f, 1});
            ImGui::PushStyleColor(ImGuiCol_Tab, {0.10f, 0.10f, 0.10f, 1});
            ImGui::PushStyleColor(ImGuiCol_TabSelected, {0.19f, 0.19f, 0.19f, 1});
            ImGui::PushStyleColor(ImGuiCol_CheckMark, {0.08f, 0.52f, 0.95f, 1});
        }

        ~EditorStyle()
        {
            ImGui::GetStyle().WindowMenuButtonPosition = OriginalMenuButtonPosition;
            ImGui::PopStyleColor(11);
            ImGui::PopStyleVar(5);
        }
    };

    void PanelTitle(const char* title)
    {
        const auto min = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(min, {min.x + ImGui::GetContentRegionAvail().x, min.y + height},
                                                  IM_COL32(43, 43, 43, 255));
        ImGui::SetCursorScreenPos({min.x + 7, min.y + ImGui::GetStyle().FramePadding.y});
        ImGui::TextUnformatted(title);
        ImGui::SetCursorScreenPos({min.x, min.y + height + 6});
    }

    struct Document
    {
        System Data;
        System Applied;
        std::filesystem::path Path;
        std::filesystem::path PendingPath;
        std::vector<System> Undo, Redo;
        std::optional<System> EditStart;
        Runtime Preview;
        Nodes::EditorContext* GraphContext = nullptr;
        Nodes::EditorContext* OverviewContext = nullptr;
        bool RestoreOverview = true;
        uint64_t SelectedEmitter = 0, SelectedModule = 0, GraphModule = 0;
        bool Loaded = false, Dirty = false, Playing = true;
        bool Focused = false, PendingQuit = false;
        bool PendingClose = false, PendingNew = false, ConfirmSwitch = false;
        bool ShowGrid = true, Snap = true, RuntimeDirty = false, GraphView = false;
        float Yaw = 0.5f, Pitch = 0.25f, Distance = 8.0f, Speed = 1.0f;
        DirectX::XMFLOAT3 Target{0, 1, 0};
        DirectX::XMFLOAT4 Background{0.025f, 0.028f, 0.035f, 1};
        int LastUpdateFrame = -1, LastDrawFrame = -1;
        float PixelsPerSecond = 80.0f, ViewStart = 0.0f;
        float IntervalStart = 0.0f, IntervalEnd = 0.0f, IntervalMouse = 0.0f;
        int IntervalEdge = 0;
        ImGuiTextFilter ParameterFilter;
        std::string Diagnostic, Status;
    };

    Document document;

    struct WindowState
    {
        bool Maximized = false;
        bool Minimized = false;
        bool ApplyGeometry = false;
        ImVec2 RestorePosition{};
        ImVec2 RestoreSize{};
    };

    WindowState windowState;

    void ApplyWindowGeometry()
    {
        if (windowState.Maximized)
        {
            const auto* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
        }
        else if (windowState.ApplyGeometry)
        {
            ImGui::SetNextWindowPos(windowState.RestorePosition);
            ImGui::SetNextWindowSize(windowState.RestoreSize);
        }
        windowState.ApplyGeometry = false;
        ImGui::SetNextWindowCollapsed(windowState.Minimized);
    }

    void DrawWindowControls()
    {
        auto* window = ImGui::GetCurrentWindow();
        const ImRect title = window->TitleBarRect();
        const float size = title.GetHeight() - 2.0f;
        if (size <= 0 || title.GetWidth() < size * 5)
        {
            return;
        }
        const float right = title.Max.x - ImGui::GetStyle().FramePadding.x - ImGui::GetFontSize() - 6;
        auto* draw = window->DrawList;
        const auto previousClip = window->ClipRect;
        window->ClipRect = title;
        draw->PushClipRect(title.Min, title.Max, false);
        for (int control = 0; control < 2; ++control)
        {
            const float x = right - size * (2 - control);
            const ImRect rect({x, title.Min.y + 1}, {x + size, title.Max.y - 1});
            const ImGuiID id = window->GetID(control == 0 ? "##MinimizeNiagara" : "##MaximizeNiagara");
            bool hovered = false;
            bool held = false;
            ImGui::ItemAdd(rect, id);
            const bool pressed = ImGui::ButtonBehavior(rect, id, &hovered, &held);
            if (hovered || held)
            {
                draw->AddRectFilled(rect.Min, rect.Max,
                                    ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered));
            }
            const auto center = rect.GetCenter();
            const float half = size * 0.22f;
            const auto color = ImGui::GetColorU32(ImGuiCol_Text);
            if (control == 0)
            {
                draw->AddLine({center.x - half, center.y + half * 0.5f}, {center.x + half, center.y + half * 0.5f},
                              color, 1.5f);
            }
            else
            {
                if (windowState.Maximized)
                {
                    draw->AddRect({center.x - half + 3, center.y - half - 3},
                                  {center.x + half + 3, center.y + half - 3}, color);
                }
                draw->AddRect({center.x - half, center.y - half}, {center.x + half, center.y + half}, color);
            }
            if (hovered)
            {
                ImGui::SetTooltip("%s", control == 0 ? (windowState.Minimized ? "展開" : "最小化")
                                                     : (windowState.Maximized ? "元のサイズに戻す" : "最大化"));
            }
            if (pressed && control == 0)
            {
                windowState.Minimized = !windowState.Minimized;
            }
            else if (pressed)
            {
                if (!windowState.Maximized)
                {
                    windowState.RestorePosition = window->Pos;
                    windowState.RestoreSize = window->SizeFull;
                }
                windowState.Maximized = !windowState.Maximized;
                windowState.Minimized = false;
                windowState.ApplyGeometry = true;
            }
        }
        draw->PopClipRect();
        window->ClipRect = previousClip;
    }

    void ClearGraphContext()
    {
        if (document.GraphContext)
        {
            Nodes::DestroyEditor(document.GraphContext);
            document.GraphContext = nullptr;
        }
        document.GraphModule = 0;
    }

    Emitter* SelectedEmitter()
    {
        for (auto& emitter : document.Data.Emitters)
        {
            if (emitter.Id == document.SelectedEmitter)
            {
                return &emitter;
            }
        }
        return nullptr;
    }

    Module* SelectedModule()
    {
        auto* emitter = SelectedEmitter();
        auto& modules = emitter ? emitter->Modules : document.Data.Modules;
        for (auto& module : modules)
        {
            if (module.Id == document.SelectedModule)
            {
                return &module;
            }
        }
        return nullptr;
    }

    void SelectFirst()
    {
        document.SelectedEmitter = document.Data.Emitters.empty() ? 0 : document.Data.Emitters.front().Id;
        document.SelectedModule = 0;
        ClearGraphContext();
        document.RestoreOverview = true;
    }

    void RefreshRuntime()
    {
        if (!Validate(document.Data, document.Diagnostic))
        {
            document.Status = "編集内容は未適用です。最後に成功した状態を表示しています。";
            return;
        }
        const float time = document.Preview.Time();
        document.Applied = document.Data;
        document.Preview.Reset(document.Applied);
        document.Preview.Seek((std::min)(time, document.Data.Duration));
        document.Status.clear();
    }

    void FinishEdit()
    {
        if (document.EditStart)
        {
            document.Undo.push_back(std::move(*document.EditStart));
            if (document.Undo.size() > 64)
            {
                document.Undo.erase(document.Undo.begin());
            }
            document.EditStart.reset();
        }
    }

    void History(bool redo)
    {
        FinishEdit();
        auto& source = redo ? document.Redo : document.Undo;
        auto& target = redo ? document.Undo : document.Redo;
        if (source.empty())
        {
            return;
        }
        target.push_back(document.Data);
        document.Data = std::move(source.back());
        source.pop_back();
        document.Dirty = true;
        ClearGraphContext();
        document.RestoreOverview = true;
        RefreshRuntime();
    }

    bool SaveDocument(bool saveAs = false)
    {
        auto path = document.Path;
        if (saveAs || path.empty())
        {
            if (!ProceduralIO::SaveFileDialog(assetFilter, L"naiagara", L"NewNiagara.naiagara", path))
            {
                return false;
            }
        }
        FinishEdit();
        if (!Save(path, document.Data, document.Diagnostic))
        {
            return false;
        }
        document.Path = path;
        document.Dirty = false;
        document.Status = "保存しました。";
        return true;
    }

    void LoadDocument(const std::filesystem::path& path)
    {
        System data;
        if (!Load(path, data, document.Diagnostic))
        {
            return;
        }
        document.Data = std::move(data);
        document.Path = path;
        document.Undo.clear();
        document.Redo.clear();
        document.EditStart.reset();
        document.Loaded = true;
        document.Dirty = false;
        document.Applied = document.Data;
        document.Preview.Reset(document.Applied);
        document.Status = "読み込みました。";
        SelectFirst();
    }

    void NewDocument()
    {
        document.Data = MakeDefaultSystem();
        document.Path.clear();
        document.Undo.clear();
        document.Redo.clear();
        document.EditStart.reset();
        document.Loaded = true;
        document.Dirty = true;
        document.Applied = document.Data;
        document.Preview.Reset(document.Applied);
        document.Diagnostic.clear();
        SelectFirst();
    }

    void ConfirmPending(bool* open)
    {
        if (document.ConfirmSwitch)
        {
            ImGui::OpenPopup("変更を保存しますか？##Niagara");
            document.ConfirmSwitch = false;
        }
        if (ImGui::BeginPopupModal("変更を保存しますか？##Niagara", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("編集中のNiagaraシステムに未保存の変更があります。");
            const bool save = ImGui::Button("保存して続行");
            ImGui::SameLine();
            const bool discard = ImGui::Button("破棄して続行");
            ImGui::SameLine();
            const bool cancel = ImGui::Button("キャンセル");
            if (discard || (save && SaveDocument()))
            {
                if (discard && (document.PendingQuit || document.PendingClose))
                {
                    document.Dirty = false;
                    document.Loaded = false;
                    document.Path.clear();
                    ClearGraphContext();
                }
                if (document.PendingQuit)
                {
                    document.Dirty = false;
                    document.PendingQuit = false;
                    ImGuiManager::RequestQuit();
                }
                else if (document.PendingClose)
                {
                    *open = false;
                }
                else if (document.PendingNew)
                {
                    NewDocument();
                }
                else
                {
                    LoadDocument(document.PendingPath);
                }
                document.PendingClose = document.PendingNew = document.PendingQuit = false;
                document.PendingPath.clear();
                ImGui::CloseCurrentPopup();
            }
            else if (cancel)
            {
                document.PendingClose = document.PendingNew = document.PendingQuit = false;
                document.PendingPath.clear();
                ImGui::CloseCurrentPopup();
            }
            if (!document.Diagnostic.empty())
            {
                ImGui::TextWrapped("%s", document.Diagnostic.c_str());
            }
            ImGui::EndPopup();
        }
    }

    float DisplayTime()
    {
        const float time = document.Preview.Time();
        if (document.Applied.Loop && document.Applied.Duration > 0.0f)
        {
            return std::fmod(time, document.Applied.Duration);
        }
        return (std::min)(time, document.Applied.Duration);
    }

    void Seek(float time)
    {
        document.Playing = false;
        document.Preview.Seek((std::clamp)(time, 0.0f, document.Data.Duration));
    }

    void DrawToolbar()
    {
        if (ImGui::Button("新規"))
        {
            if (document.Dirty)
            {
                document.PendingNew = document.ConfirmSwitch = true;
            }
            else
            {
                NewDocument();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("開く"))
        {
            std::filesystem::path path;
            if (ProceduralIO::OpenFileDialog(assetFilter, path))
            {
                NiagaraWindow::Open(path);
            }
        }
        ImGui::BeginDisabled(!document.Loaded);
        ImGui::SameLine();
        if (ImGui::Button("保存"))
        {
            SaveDocument();
        }
        ImGui::SameLine();
        if (ImGui::Button("別名で保存"))
        {
            SaveDocument(true);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(document.Undo.empty() && !document.EditStart);
        if (ImGui::Button("元に戻す"))
        {
            History(false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(document.Redo.empty());
        if (ImGui::Button("やり直す"))
        {
            History(true);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("シーンへ配置"))
        {
            if ((!document.Dirty && !document.Path.empty()) || SaveDocument())
            {
                ImGuiManager::PlaceNiagaraAsset(document.Path);
            }
        }
        ImGui::EndDisabled();
        if (document.Loaded)
        {
            const auto filename = document.Path.empty() ? std::string("未保存のシステム")
                                                        : ProceduralIO::ToUtf8(document.Path.filename());
            ImGui::Text("%s%s", filename.c_str(), document.Dirty ? " *" : "");
        }
    }

    bool DrawParameters()
    {
        bool changed = false;
        PanelTitle("パラメーター / Parameters");
        document.ParameterFilter.Draw("検索", -1.0f);
        if (ImGui::Button("パラメーターを追加"))
        {
            Parameter parameter;
            unsigned int suffix = 1;
            do
            {
                parameter.Name = "User.Parameter" + std::to_string(suffix++);
            } while (std::any_of(document.Data.Parameters.begin(), document.Data.Parameters.end(),
                                 [&](const auto& existing) {
                                     return existing.Name == parameter.Name;
                                 }));
            document.Data.Parameters.push_back(std::move(parameter));
            changed = true;
        }
        int remove = -1;
        const char* scopes[] = {"User Exposed", "System Attributes", "Emitter Attributes", "Particle Attributes"};
        for (int group = 0; group < 4; ++group)
        {
            if (!ImGui::CollapsingHeader(scopes[group], ImGuiTreeNodeFlags_DefaultOpen))
            {
                continue;
            }
            for (size_t index = 0; index < document.Data.Parameters.size(); ++index)
            {
                auto& parameter = document.Data.Parameters[index];
                if (static_cast<int>(parameter.Scope) != group ||
                    !document.ParameterFilter.PassFilter(parameter.Name.c_str()))
                {
                    continue;
                }
                ImGui::PushID(static_cast<int>(index));
                const auto typeColor = parameter.Type == ParameterType::Bool ? IM_COL32(190, 40, 40, 255)
                                       : parameter.Type == ParameterType::Int
                                           ? IM_COL32(40, 185, 165, 255)
                                           : GraphVisuals::TypeColor(static_cast<MaterialGraphType>(
                                                 (std::clamp)(static_cast<int>(parameter.Type) - 1, 1, 4)));
                GraphVisuals::PinIcon(typeColor, true);
                ImGui::SameLine(0, 4);
                if (ImGui::TreeNodeEx("Parameter", ImGuiTreeNodeFlags_DefaultOpen, "%s", parameter.Name.c_str()))
                {
                    char name[256]{};
                    snprintf(name, sizeof(name), "%s", parameter.Name.c_str());
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputText("##Name", name, sizeof(name)))
                    {
                        const std::string oldName = parameter.Name;
                        parameter.Name = name;
                        auto rename = [&](std::vector<Module>& modules) {
                            for (auto& module : modules)
                            {
                                if (module.ParameterName == oldName)
                                {
                                    module.ParameterName = name;
                                }
                                if (module.WriteParameter == oldName)
                                {
                                    module.WriteParameter = name;
                                }
                                for (auto& node : module.Graph.Nodes)
                                {
                                    if (node.Name == oldName)
                                    {
                                        node.Name = name;
                                    }
                                }
                            }
                        };
                        rename(document.Data.Modules);
                        for (auto& emitter : document.Data.Emitters)
                        {
                            rename(emitter.Modules);
                        }
                        changed = true;
                    }
                    int type = static_cast<int>(parameter.Type);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##Type", &type, "Bool\0Int\0Float\0Vector2\0Vector3\0Vector4\0Color\0"))
                    {
                        parameter.Type = static_cast<ParameterType>(type);
                        changed = true;
                    }
                    int scope = static_cast<int>(parameter.Scope);
                    if (ImGui::Combo("スコープ", &scope, "User\0System\0Emitter\0Particles\0"))
                    {
                        parameter.Scope = static_cast<ParameterScope>(scope);
                        changed = true;
                    }
                    switch (parameter.Type)
                    {
                        case ParameterType::Bool: {
                            bool value = parameter.Value[0] != 0.0f;
                            if (ImGui::Checkbox("値", &value))
                            {
                                parameter.Value[0] = value ? 1.0f : 0.0f;
                                changed = true;
                            }
                            break;
                        }
                        case ParameterType::Int: {
                            int value = static_cast<int>(parameter.Value[0]);
                            if (ImGui::DragInt("値", &value))
                            {
                                parameter.Value[0] = static_cast<float>(value);
                                changed = true;
                            }
                            break;
                        }
                        case ParameterType::Color:
                            changed |= ImGui::ColorEdit4("値", parameter.Value.data());
                            break;
                        default: {
                            const int count =
                                static_cast<int>(parameter.Type) - static_cast<int>(ParameterType::Float) + 1;
                            changed |=
                                ImGui::DragScalarN("値", ImGuiDataType_Float, parameter.Value.data(), count, 0.01f);
                            break;
                        }
                    }
                    if (ImGui::SmallButton("削除"))
                    {
                        remove = static_cast<int>(index);
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        if (remove >= 0)
        {
            document.Data.Parameters.erase(document.Data.Parameters.begin() + remove);
            changed = true;
        }
        return changed;
    }

    void DrawPreview()
    {
        PanelTitle("プレビュー / Preview");
        if (ImGui::Button("全体を表示"))
        {
            float radius = 1.0f;
            for (const auto& particle : document.Preview.Particles())
            {
                const auto& p = particle.Position;
                radius = (std::max)(radius, std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z));
            }
            document.Distance = (std::clamp)(radius * 2.5f, 0.25f, 2000.0f);
        }
        ImGui::SameLine();
        ImGui::Checkbox("グリッド", &document.ShowGrid);
        ImGui::ColorEdit3("背景", &document.Background.x, ImGuiColorEditFlags_NoInputs);
        const float width = (std::max)(64.0f, ImGui::GetContentRegionAvail().x);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size(
            width, (std::max)(100.0f, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 4));
        NiagaraRenderer::SetPreview(document.Applied, document.Preview.Particles(), document.Yaw, document.Pitch,
                                    document.Distance, document.Target, document.ShowGrid, document.Background);
        const auto texture = NiagaraRenderer::PreviewTexture();
        // ImageだけではActiveIdが設定されず、親ウィンドウの移動に入力が渡る。
        ImGui::InvisibleButton("NiagaraPreview", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(origin.x + size.x, origin.y + size.y);
        if (texture)
        {
            draw->AddImage(texture, origin, end);
        }
        else
        {
            draw->AddRectFilled(origin, end, IM_COL32(22, 23, 27, 255));
        }
        if (hovered)
        {
            document.Distance =
                (std::clamp)(document.Distance * std::pow(0.88f, ImGui::GetIO().MouseWheel), 0.25f, 2000.0f);
        }
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            document.Yaw += ImGui::GetIO().MouseDelta.x * 0.01f;
            document.Pitch = (std::clamp)(document.Pitch + ImGui::GetIO().MouseDelta.y * 0.01f, -1.5f, 1.5f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            const float amount = document.Distance * 0.0015f;
            const auto delta = ImGui::GetIO().MouseDelta;
            document.Target.x -= std::cos(document.Yaw) * delta.x * amount;
            document.Target.z += std::sin(document.Yaw) * delta.x * amount;
            document.Target.y += delta.y * amount;
        }
        ImGui::Text("粒子 %zu / 上限で破棄 %llu", document.Preview.Particles().size(), document.Preview.Dropped());
        ImGui::TextDisabled("左ドラッグ: 回転 / 中ドラッグ: パン / ホイール: ズーム");
        if (!NiagaraRenderer::LastError().empty())
        {
            ImGui::TextWrapped("%s", NiagaraRenderer::LastError().c_str());
        }
    }

    const char* StageName(Stage stage)
    {
        switch (stage)
        {
            case Stage::SystemSpawn:
                return "System Spawn";
            case Stage::SystemUpdate:
                return "System Update";
            case Stage::EmitterSpawn:
                return "Emitter Spawn";
            case Stage::EmitterUpdate:
                return "Emitter Update";
            case Stage::ParticleSpawn:
                return "Particle Spawn";
            case Stage::ParticleUpdate:
                return "Particle Update";
        }
        return "Unknown";
    }

    Stage ModuleStage(const Module& module)
    {
        if (module.Kind == ModuleKind::CustomGraph)
        {
            return module.ExecutionStage;
        }
        if (module.Kind == ModuleKind::SpawnRate || module.Kind == ModuleKind::Burst)
        {
            return Stage::EmitterUpdate;
        }
        if (module.Kind == ModuleKind::Initialize || module.Kind == ModuleKind::Velocity ||
            module.Kind == ModuleKind::ConeVelocity)
        {
            return Stage::ParticleSpawn;
        }
        return Stage::ParticleUpdate;
    }

    ImU32 StageColor(Stage stage)
    {
        switch (stage)
        {
            case Stage::SystemSpawn:
            case Stage::SystemUpdate:
                return IM_COL32(54, 124, 146, 255);
            case Stage::EmitterSpawn:
            case Stage::EmitterUpdate:
                return IM_COL32(158, 94, 48, 255);
            case Stage::ParticleSpawn:
                return IM_COL32(94, 136, 60, 255);
            case Stage::ParticleUpdate:
                return IM_COL32(64, 112, 76, 255);
        }
        return IM_COL32(70, 70, 70, 255);
    }

    bool DrawOverviewStack(std::vector<Module>& modules, uint64_t emitterId)
    {
        bool changed = false;
        std::optional<Stage> previous;
        for (auto& module : modules)
        {
            const auto stage = ModuleStage(module);
            if (previous != stage)
            {
                ImGui::Separator();
                const auto min = ImGui::GetCursorScreenPos();
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(min, {min.x + 222, min.y + ImGui::GetTextLineHeightWithSpacing()},
                                    IM_COL32(47, 47, 47, 255));
                draw->AddRectFilled(min, {min.x + 3, min.y + ImGui::GetTextLineHeightWithSpacing()}, StageColor(stage));
                ImGui::Indent(8);
                ImGui::TextUnformatted(StageName(stage));
                ImGui::Unindent(8);
                previous = stage;
            }
            ImGui::PushID(static_cast<int>(module.Id));
            if (ImGui::Selectable(ModuleName(module.Kind), document.SelectedModule == module.Id, 0,
                                  {190, ImGui::GetFrameHeight()}))
            {
                document.SelectedEmitter = emitterId;
                document.SelectedModule = module.Id;
            }
            ImGui::SameLine();
            changed |= ImGui::Checkbox("##Enabled", &module.Enabled);
            ImGui::PopID();
        }
        return changed;
    }

    bool DrawOverview()
    {
        bool changed = false;
        if (ImGui::Button("エミッターを追加"))
        {
            auto prototype = MakeDefaultSystem().Emitters.front();
            prototype.Id = NextId(document.Data);
            uint64_t identity = prototype.Id + 1;
            for (auto& module : prototype.Modules)
            {
                module.Id = identity++;
            }
            prototype.Name = "Emitter " + std::to_string(document.Data.Emitters.size() + 1);
            prototype.Position = {280.0f + document.Data.Emitters.size() * 270.0f, 30.0f};
            document.SelectedEmitter = prototype.Id;
            document.SelectedModule = 0;
            document.Data.Emitters.push_back(std::move(prototype));
            document.RestoreOverview = true;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("全体を表示"))
        {
            Nodes::SetCurrentEditor(document.OverviewContext);
            if (document.OverviewContext)
            {
                Nodes::NavigateToContent();
            }
            Nodes::SetCurrentEditor(nullptr);
        }
        if (!document.OverviewContext)
        {
            Nodes::Config config;
            config.SettingsFile = nullptr;
            document.OverviewContext = Nodes::CreateEditor(&config);
            document.RestoreOverview = true;
        }
        Nodes::SetCurrentEditor(document.OverviewContext);
        const Nodes::NodeId systemNode(static_cast<uintptr_t>(~uint64_t(0)));
        const bool fitOverview = document.RestoreOverview;
        if (document.RestoreOverview)
        {
            Nodes::SetNodePosition(systemNode, {0, 30});
            for (const auto& emitter : document.Data.Emitters)
            {
                Nodes::SetNodePosition(Nodes::NodeId(emitter.Id), {emitter.Position[0], emitter.Position[1]});
            }
            document.RestoreOverview = false;
        }
        GraphVisuals::PushCanvasStyle();
        Nodes::Begin("NiagaraSystemCanvas", {0, (std::max)(120.0f, ImGui::GetContentRegionAvail().y)});
        Nodes::BeginNode(systemNode);
        if (ImGui::Selectable(document.Data.Name.c_str(), document.SelectedEmitter == 0, 0, {210, 26}))
        {
            document.SelectedEmitter = document.SelectedModule = 0;
        }
        const float systemHeader = ImGui::GetItemRectMax().y;
        ImGui::TextDisabled("System / %zu Emitters", document.Data.Emitters.size());
        changed |= DrawOverviewStack(document.Data.Modules, 0);
        Nodes::EndNode();
        GraphVisuals::DrawHeader(systemNode, systemHeader, StageColor(Stage::SystemUpdate));
        for (auto& emitter : document.Data.Emitters)
        {
            ImGui::PushID(static_cast<int>(emitter.Id));
            Nodes::BeginNode(Nodes::NodeId(emitter.Id));
            if (ImGui::Selectable(emitter.Name.c_str(), document.SelectedEmitter == emitter.Id, 0, {210, 26}))
            {
                document.SelectedEmitter = emitter.Id;
                document.SelectedModule = 0;
            }
            const float emitterHeader = ImGui::GetItemRectMax().y;
            changed |= ImGui::Checkbox("有効", &emitter.Enabled);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("ソロ", &emitter.Solo);
            changed |= DrawOverviewStack(emitter.Modules, emitter.Id);
            Nodes::EndNode();
            GraphVisuals::DrawHeader(Nodes::NodeId(emitter.Id), emitterHeader, StageColor(Stage::EmitterUpdate));
            const auto position = Nodes::GetNodePosition(Nodes::NodeId(emitter.Id));
            if (position.x != emitter.Position[0] || position.y != emitter.Position[1])
            {
                emitter.Position = {position.x, position.y};
                changed = true;
            }
            ImGui::PopID();
        }
        if (fitOverview)
        {
            Nodes::NavigateToContent(0.0f);
        }
        Nodes::End();
        Nodes::PopStyleColor(6);
        Nodes::SetCurrentEditor(nullptr);
        return changed;
    }

    bool DrawCurve(Module& module)
    {
        bool changed = false;
        if (ImGui::TreeNode("寿命カーブ（0～1）"))
        {
            if (ImGui::Button("キーを追加"))
            {
                module.Curve.push_back({1.0f, module.Secondary});
                changed = true;
            }
            int remove = -1;
            for (size_t index = 0; index < module.Curve.size(); ++index)
            {
                auto& key = module.Curve[index];
                ImGui::PushID(static_cast<int>(index));
                changed |= ImGui::DragFloat("時刻", &key.Time, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::DragFloat4("値", key.Value.data(), 0.01f);
                if (ImGui::SmallButton("削除"))
                {
                    remove = static_cast<int>(index);
                }
                ImGui::PopID();
            }
            if (remove >= 0)
            {
                module.Curve.erase(module.Curve.begin() + remove);
                changed = true;
            }
            if (changed)
            {
                std::stable_sort(module.Curve.begin(), module.Curve.end(), [](const auto& a, const auto& b) {
                    return a.Time < b.Time;
                });
            }
            ImGui::TreePop();
        }
        return changed;
    }

    bool DrawModuleValues(Module& module)
    {
        bool changed = ImGui::Checkbox("モジュール有効", &module.Enabled);
        const char* binding = module.ParameterName.empty() ? "直接指定" : module.ParameterName.c_str();
        if (ImGui::BeginCombo("入力の参照", binding))
        {
            if (ImGui::Selectable("直接指定", module.ParameterName.empty()))
            {
                module.ParameterName.clear();
                changed = true;
            }
            for (const auto& parameter : document.Data.Parameters)
            {
                if (ImGui::Selectable(parameter.Name.c_str(), parameter.Name == module.ParameterName))
                {
                    module.ParameterName = parameter.Name;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(!module.ParameterName.empty());
        switch (module.Kind)
        {
            case ModuleKind::ConeVelocity:
                changed |= ImGui::DragFloat3("軸", module.Value.data(), 0.01f);
                changed |= ImGui::DragFloat("速度 (m/s)", &module.Value[3], 0.05f, 0, 10000, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::DragFloat("開き角 (度)", &module.Secondary[0], 0.1f, 0, 180, "%.1f",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::DragFloat("速度のランダム幅", &module.Secondary[1], 0.05f, 0, 10000, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::PointAttraction:
                changed |= ImGui::DragFloat3("中心", module.Value.data(), 0.05f);
                changed |= ImGui::DragFloat("吸引加速度 (m/s²)", &module.Value[3], 0.05f);
                changed |= ImGui::DragFloat("半径 (m)", &module.Secondary[0], 0.05f, 0.01f, 10000, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::DragFloat("減衰指数", &module.Secondary[1], 0.01f, 0, 16, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::VortexForce:
                changed |= ImGui::DragFloat3("渦の軸", module.Value.data(), 0.01f);
                changed |= ImGui::DragFloat("接線加速度 (m/s²)", &module.Value[3], 0.05f);
                changed |= ImGui::DragFloat3("渦の中心", module.Secondary.data(), 0.05f);
                changed |= ImGui::DragFloat("半径 (m)", &module.Secondary[3], 0.05f, 0.01f, 10000, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::LimitVelocity:
                changed |= ImGui::DragFloat("最大速度 (m/s)", &module.Value[0], 0.05f, 0, 10000, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextWrapped("このモジュールより前の力を含めて速度を制限します。");
                break;
            case ModuleKind::RenderRadialWave:
                changed |= ImGui::DragFloat3("波の中心", module.Value.data(), 0.05f);
                changed |= ImGui::DragFloat("振幅 (m/s²)", &module.Value[3], 0.05f);
                changed |= ImGui::DragFloat("位相速度 (周期/秒)", &module.Secondary[0], 0.05f);
                changed |= ImGui::DragFloat("周波数 (周期/m)", &module.Secondary[1], 0.01f, 0, 100, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::DragFloat("位相 (rad)", &module.Secondary[2], 0.01f);
                changed |= ImGui::DragFloat("影響半径 (m)", &module.Secondary[3], 0.05f, 0.01f, 10000, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::RenderSpeedColor:
                changed |= ImGui::ColorEdit3("低速カラー", module.Value.data(),
                                             ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
                changed |= ImGui::ColorEdit3("高速カラー", module.Secondary.data(),
                                             ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
                changed |= ImGui::DragFloat("最低速度 (m/s)", &module.Value[3], 0.05f, 0, 10000, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::DragFloat("最高速度 (m/s)", &module.Secondary[3], 0.05f, 0, 10000, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextWrapped("初期カラーのRGBへ乗算します。アルファは変更しません。");
                break;
            case ModuleKind::SpawnRate:
                changed |= ImGui::DragFloat("発生数 / 秒", &module.Value[0], 1.0f, 0.0f, 100000.0f, "%.1f",
                                            ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::Burst:
                changed |= ImGui::DragFloat("発生数", &module.Value[0], 1.0f, 0.0f, 100000.0f, "%.0f",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::DragFloat("発生時刻 (秒)", &module.Value[1], 0.01f, 0.0f, document.Data.Duration,
                                            "%.3f", ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::Initialize:
                changed |= ImGui::ColorEdit4("初期カラー", module.Value.data());
                changed |= ImGui::DragFloat("寿命 (秒)", &module.Secondary[0], 0.01f, 0.01f, 3600.0f, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::DragFloat("サイズ (m)", &module.Secondary[1], 0.01f, 0.001f, 1000.0f, "%.3f",
                                            ImGuiSliderFlags_AlwaysClamp);
                changed |= ImGui::DragFloat("発生半径 (m)", &module.Secondary[2], 0.01f, 0.0f, 1000.0f, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::Velocity:
            case ModuleKind::Gravity:
                changed |= ImGui::DragFloat3(module.Kind == ModuleKind::Velocity ? "速度 (m/s)" : "加速度 (m/s²)",
                                             module.Value.data(), 0.05f);
                if (module.Kind == ModuleKind::Velocity)
                {
                    changed |= ImGui::DragFloat3("ランダム幅 (±)", module.Secondary.data(), 0.01f, 0.0f, 1000.0f,
                                                 "%.2f", ImGuiSliderFlags_AlwaysClamp);
                }
                break;
            case ModuleKind::ColorOverLife:
                changed |= ImGui::ColorEdit4("開始カラー", module.Value.data());
                changed |= ImGui::ColorEdit4("終了カラー", module.Secondary.data());
                changed |= DrawCurve(module);
                break;
            case ModuleKind::ScaleOverLife:
                changed |= ImGui::DragFloat("開始倍率", &module.Value[0], 0.01f);
                changed |= ImGui::DragFloat("終了倍率", &module.Secondary[0], 0.01f);
                changed |= DrawCurve(module);
                break;
            case ModuleKind::CustomGraph: {
                int attribute = static_cast<int>(module.Target);
                if (module.ExecutionStage <= Stage::ParticleUpdate &&
                    ImGui::Combo("書き込み属性", &attribute, "Position\0Velocity\0Color\0Size\0Lifetime\0"))
                {
                    module.Target = static_cast<Attribute>(attribute);
                    changed = true;
                }
                int stage = static_cast<int>(module.ExecutionStage);
                if (ImGui::Combo(
                        "実行段階", &stage,
                        "Particle Spawn\0Particle Update\0System Spawn\0System Update\0Emitter Spawn\0Emitter Update\0"))
                {
                    module.ExecutionStage = static_cast<Stage>(stage);
                    changed = true;
                }
                if (module.ExecutionStage >= Stage::SystemSpawn)
                {
                    if (ImGui::BeginCombo("書き込み先", module.WriteParameter.empty() ? "パラメーターを選択"
                                                                                      : module.WriteParameter.c_str()))
                    {
                        for (const auto& parameter : document.Data.Parameters)
                        {
                            const bool systemStage = module.ExecutionStage == Stage::SystemSpawn ||
                                                     module.ExecutionStage == Stage::SystemUpdate;
                            if (parameter.Scope != (systemStage ? ParameterScope::System : ParameterScope::Emitter))
                            {
                                continue;
                            }
                            if (ImGui::Selectable(parameter.Name.c_str(), parameter.Name == module.WriteParameter))
                            {
                                module.WriteParameter = parameter.Name;
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                ImGui::TextWrapped("中央のモジュールグラフで処理を編集し、出力に使うノードを選択します。");
                break;
            }
            case ModuleKind::Drag:
                changed |= ImGui::DragFloat("抵抗係数", &module.Value[0], 0.01f, 0.0f, 100.0f, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::Noise:
                changed |= ImGui::DragFloat3("揺らぎの加速度", module.Value.data(), 0.01f);
                changed |= ImGui::DragFloat("周波数", &module.Secondary[0], 0.01f, 0.001f, 1000.0f, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::GroundCollision:
                changed |= ImGui::DragFloat("地面の高さ", &module.Value[0], 0.01f);
                changed |= ImGui::DragFloat("反発係数", &module.Value[1], 0.01f, 0.0f, 1.0f, "%.2f",
                                            ImGuiSliderFlags_AlwaysClamp);
                break;
            case ModuleKind::DeathEventSpawn: {
                changed |= ImGui::DragFloat("死亡時の発生数", &module.Value[0], 1.0f, 0.0f, 10000.0f, "%.0f",
                                            ImGuiSliderFlags_AlwaysClamp);
                int target = static_cast<int>(module.Secondary[0]);
                const char* targetName = target >= 0 && target < static_cast<int>(document.Data.Emitters.size())
                                             ? document.Data.Emitters[target].Name.c_str()
                                             : "未設定";
                if (ImGui::BeginCombo("発生先エミッター", targetName))
                {
                    for (size_t index = 0; index < document.Data.Emitters.size(); ++index)
                    {
                        if (ImGui::Selectable(document.Data.Emitters[index].Name.c_str(),
                                              target == static_cast<int>(index)))
                        {
                            module.Secondary[0] = static_cast<float>(index);
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            default:
                break;
        }
        ImGui::EndDisabled();
        return changed;
    }

    bool DrawSelection()
    {
        bool changed = false;
        PanelTitle("選択 / Selection");
        auto* emitter = SelectedEmitter();
        if (!emitter)
        {
            char name[256]{};
            snprintf(name, sizeof(name), "%s", document.Data.Name.c_str());
            if (ImGui::InputText("システム名", name, sizeof(name)))
            {
                document.Data.Name = name;
                changed = true;
            }
            changed |= ImGui::InputScalar("乱数シード", ImGuiDataType_U32, &document.Data.Seed);
        }
        if (emitter)
        {
            char name[256]{};
            snprintf(name, sizeof(name), "%s", emitter->Name.c_str());
            if (ImGui::InputText("名前", name, sizeof(name)))
            {
                emitter->Name = name;
                changed = true;
            }
            changed |= ImGui::Checkbox("ローカル空間", &emitter->LocalSpace);
            int backend = static_cast<int>(emitter->Simulation);
            if (ImGui::Combo("シミュレーション", &backend, "CPU\0GPU (参照実装)\0"))
            {
                emitter->Simulation = static_cast<Backend>(backend);
                changed = true;
            }
            if (emitter->Simulation == Backend::GPU)
            {
                ImGui::TextWrapped(
                    "GPU参照実装は各ステップで結果を読み戻します。大量粒子向けの性能最適化は未実施です。");
            }
            int renderer = static_cast<int>(emitter->Renderer);
            if (ImGui::Combo("描画", &renderer, "Sprite\0Mesh\0Ribbon\0"))
            {
                emitter->Renderer = static_cast<RendererKind>(renderer);
                changed = true;
            }
            changed |= ImGui::DragFloat("開始 (秒)", &emitter->Start, 0.01f, 0.0f, document.Data.Duration, "%.2f",
                                        ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragFloat("終了 (秒)", &emitter->End, 0.01f, emitter->Start, document.Data.Duration,
                                        "%.2f", ImGuiSliderFlags_AlwaysClamp);
            int maximum = static_cast<int>(emitter->MaxParticles);
            if (ImGui::DragInt("粒子数上限", &maximum, 100.0f, 1, 100000, "%d", ImGuiSliderFlags_AlwaysClamp))
            {
                emitter->MaxParticles = static_cast<uint32_t>(maximum);
                changed = true;
            }
            if (ImGui::Button("複製"))
            {
                auto copy = *emitter;
                copy.Id = NextId(document.Data);
                uint64_t id = copy.Id + 1;
                for (auto& module : copy.Modules)
                {
                    module.Id = id++;
                }
                copy.Name += " Copy";
                copy.Position[0] += 270.0f;
                document.RestoreOverview = true;
                document.SelectedEmitter = copy.Id;
                document.SelectedModule = 0;
                document.Data.Emitters.push_back(std::move(copy));
                return true;
            }
            ImGui::SameLine();
            if (ImGui::Button("エミッターを削除"))
            {
                const auto removed = static_cast<size_t>(emitter - document.Data.Emitters.data());
                for (auto& other : document.Data.Emitters)
                {
                    for (auto& module : other.Modules)
                    {
                        if (module.Kind != ModuleKind::DeathEventSpawn)
                        {
                            continue;
                        }
                        const size_t target = static_cast<size_t>(module.Secondary[0]);
                        if (target == removed)
                        {
                            module.Enabled = false;
                            module.Secondary[0] = 0;
                        }
                        else if (target > removed)
                        {
                            module.Secondary[0] -= 1.0f;
                        }
                    }
                }
                std::erase_if(document.Data.Emitters, [&](const auto& item) {
                    return item.Id == document.SelectedEmitter;
                });
                SelectFirst();
                return true;
            }
        }
        auto& modules = emitter ? emitter->Modules : document.Data.Modules;
        if (ImGui::Button("モジュールを追加"))
        {
            ImGui::OpenPopup("ModulePalette");
        }
        if (ImGui::BeginPopup("ModulePalette"))
        {
            for (int kind = 0; kind < static_cast<int>(ModuleKind::Count); ++kind)
            {
                if (!emitter && static_cast<ModuleKind>(kind) != ModuleKind::CustomGraph)
                {
                    continue;
                }
                if (ImGui::MenuItem(ModuleName(static_cast<ModuleKind>(kind))))
                {
                    auto module = MakeModule(static_cast<ModuleKind>(kind));
                    module.Id = NextId(document.Data);
                    if (!emitter)
                    {
                        module.ExecutionStage = Stage::SystemUpdate;
                    }
                    document.SelectedModule = module.Id;
                    modules.push_back(std::move(module));
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
        for (const auto& module : modules)
        {
            ImGui::PushID(static_cast<int>(module.Id));
            if (ImGui::Selectable(ModuleName(module.Kind), document.SelectedModule == module.Id))
            {
                document.SelectedModule = module.Id;
            }
            ImGui::PopID();
        }
        if (auto* module = SelectedModule())
        {
            ImGui::SeparatorText(ModuleName(module->Kind));
            changed |= DrawModuleValues(*module);
            auto iterator = std::find_if(modules.begin(), modules.end(), [&](const auto& item) {
                return item.Id == module->Id;
            });
            const size_t index = static_cast<size_t>(iterator - modules.begin());
            if (ImGui::Button("上へ") && index > 0)
            {
                std::swap(modules[index], modules[index - 1]);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("下へ") && index + 1 < modules.size())
            {
                std::swap(modules[index], modules[index + 1]);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("モジュールを削除"))
            {
                modules.erase(modules.begin() + index);
                document.SelectedModule = 0;
                changed = true;
            }
        }
        return changed;
    }

    bool DrawTimeline()
    {
        bool changed = false;

        if (ImGui::Button(document.Playing ? "一時停止" : "再生"))
        {
            document.Playing = !document.Playing;
        }
        ImGui::SameLine();
        if (ImGui::Button("先頭"))
        {
            Seek(0.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("-1f"))
        {
            Seek(DisplayTime() - 1.0f / 60.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("+1f"))
        {
            Seek(DisplayTime() + 1.0f / 60.0f);
        }
        ImGui::SameLine();
        changed |= ImGui::Checkbox("ループ", &document.Data.Loop);
        ImGui::SameLine();
        ImGui::Checkbox("フレームスナップ", &document.Snap);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(85);
        ImGui::DragFloat("速度", &document.Speed, 0.05f, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        changed |= ImGui::DragFloat("長さ", &document.Data.Duration, 0.1f, 0.1f, 3600.0f, "%.2fs",
                                    ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        ImGui::Text("%.3f s", DisplayTime());
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float width = (std::max)(100.0f, ImGui::GetContentRegionAvail().x);
        const float labelWidth = (std::min)(150.0f, width * 0.25f);
        const ImVec2 max(min.x + width, min.y + 27.0f);
        auto* draw = ImGui::GetWindowDrawList();
        TimelineWidgets::Axis axis{min.x + labelWidth, document.ViewStart, document.PixelsPerSecond};
        draw->AddRectFilled(min, max, IM_COL32(20, 21, 25, 255));
        draw->PushClipRect({axis.Left, min.y}, max, true);
        const float major = std::pow(10.0f, std::ceil(std::log10(70.0f / document.PixelsPerSecond)));
        TimelineWidgets::DrawTicks(draw, axis, {axis.Left, min.y}, max, major, major / 5.0f, 0,
                                   ImGui::GetColorU32(ImGuiCol_Text), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                   [](float time, float step, char* label, size_t count) {
                                       snprintf(label, count, step < 1 ? "%.2fs" : "%.0fs", time);
                                   });
        TimelineWidgets::DrawPlayhead(draw, axis.ToX(DisplayTime()), {axis.Left, min.y}, max,
                                      IM_COL32(240, 190, 80, 255));
        draw->PopClipRect();
        ImGui::SetCursorScreenPos({axis.Left, min.y});
        ImGui::InvisibleButton("NiagaraRuler", {width - labelWidth, 27},
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            document.ViewStart =
                (std::max)(0.0f, document.ViewStart - ImGui::GetIO().MouseDelta.x / document.PixelsPerSecond);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            Seek(TimelineWidgets::ScrubTime(axis, ImGui::GetIO().MousePos.x, document.Data.Duration, document.Snap,
                                            60.0f));
        }
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0)
        {
            const float time = axis.ToTime(ImGui::GetIO().MousePos.x);
            document.PixelsPerSecond =
                (std::clamp)(document.PixelsPerSecond * std::pow(1.2f, ImGui::GetIO().MouseWheel), 2.0f, 2000.0f);
            document.ViewStart =
                (std::max)(0.0f, time - (ImGui::GetIO().MousePos.x - axis.Left) / document.PixelsPerSecond);
        }
        ImGui::SetCursorScreenPos({min.x, max.y});
        ImGui::BeginChild("EmitterLanes", {0, (std::max)(50.0f, ImGui::GetContentRegionAvail().y)},
                          ImGuiChildFlags_Borders);
        for (auto& emitter : document.Data.Emitters)
        {
            ImGui::PushID(static_cast<int>(emitter.Id));
            const ImVec2 row = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(emitter.Name.c_str(), document.SelectedEmitter == emitter.Id, 0,
                                  {labelWidth - 10, 22}))
            {
                document.SelectedEmitter = emitter.Id;
            }
            const float laneMin = (std::max)(axis.Left, axis.ToX(emitter.Start));
            const float laneMax = (std::min)(min.x + width - 16.0f, axis.ToX(emitter.End));
            if (laneMax > laneMin)
            {
                auto* lanes = ImGui::GetWindowDrawList();
                lanes->AddRectFilled({laneMin, row.y + 2}, {laneMax, row.y + 20},
                                     ImGui::GetColorU32(emitter.Enabled ? ImGuiCol_HeaderActive : ImGuiCol_FrameBg), 2);
                ImGui::SetCursorScreenPos({laneMin, row.y});
                ImGui::InvisibleButton("EmitterInterval", {laneMax - laneMin, 22});
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    ImGui::SetItemTooltip("ドラッグで区間を移動 / 両端をドラッグで開始・終了を変更");
                }
                if (ImGui::IsItemActivated())
                {
                    document.SelectedEmitter = emitter.Id;
                    document.IntervalStart = emitter.Start;
                    document.IntervalEnd = emitter.End;
                    document.IntervalMouse = ImGui::GetIO().MousePos.x;
                    document.IntervalEdge =
                        std::abs(document.IntervalMouse - axis.ToX(emitter.Start)) < 7
                            ? -1
                            : (std::abs(document.IntervalMouse - axis.ToX(emitter.End)) < 7 ? 1 : 0);
                }
                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    float offset = (ImGui::GetIO().MousePos.x - document.IntervalMouse) / document.PixelsPerSecond;
                    if (document.Snap)
                    {
                        offset = TimelineWidgets::SnapFrame(offset, 60.0f);
                    }
                    const float previousStart = emitter.Start;
                    const float previousEnd = emitter.End;
                    if (document.IntervalEdge < 0)
                    {
                        emitter.Start = (std::clamp)(document.IntervalStart + offset, 0.0f, emitter.End);
                    }
                    else if (document.IntervalEdge > 0)
                    {
                        emitter.End =
                            (std::clamp)(document.IntervalEnd + offset, emitter.Start, document.Data.Duration);
                    }
                    else
                    {
                        const float span = document.IntervalEnd - document.IntervalStart;
                        emitter.Start = (std::clamp)(document.IntervalStart + offset, 0.0f,
                                                     (std::max)(0.0f, document.Data.Duration - span));
                        emitter.End = emitter.Start + span;
                    }
                    changed |= emitter.Start != previousStart || emitter.End != previousEnd;
                }
            }
            const float head = axis.ToX(DisplayTime());
            if (head >= axis.Left && head < min.x + width - 16.0f)
            {
                ImGui::GetWindowDrawList()->AddLine({head, row.y}, {head, row.y + 22}, IM_COL32(240, 190, 80, 255));
            }
            ImGui::SetCursorScreenPos({row.x, row.y + 24});
            ImGui::Dummy({0.0f, 0.0f});
            ImGui::PopID();
        }
        ImGui::EndChild();
        return changed;
    }
}

void NiagaraWindow::Open(const std::filesystem::path& path)
{
    if (document.Dirty)
    {
        document.PendingPath = path;
        document.PendingClose = document.PendingNew = document.PendingQuit = false;
        document.ConfirmSwitch = true;
    }
    else
    {
        LoadDocument(path);
    }
    windowState.Minimized = false;
    ImGui::SetWindowFocus("Niagara###NiagaraWindow");
}

void NiagaraWindow::Draw(bool* open)
{
    if (!open || !*open)
    {
        document.Focused = false;
        return;
    }
    document.LastDrawFrame = ImGui::GetFrameCount();
    EditorStyle editorStyle;
    const auto workSize = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize({(std::min)(1600.0f, workSize.x * 0.95f), (std::min)(950.0f, workSize.y * 0.95f)},
                             ImGuiCond_FirstUseEver);
    ApplyWindowGeometry();
    bool visible = *open;
    const bool contentDrawerOpen = EditorWorkspaceManager::Get().Registry().IsContentDrawerOpen();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar |
                                   (contentDrawerOpen ? ImGuiWindowFlags_NoBringToFrontOnFocus : 0) |
                                   (document.Dirty ? ImGuiWindowFlags_UnsavedDocument : 0) |
                                   (windowState.Maximized ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize : 0);
    const bool expanded = ImGui::Begin("Niagara###NiagaraWindow", &visible, flags);
    windowState.Minimized = ImGui::IsWindowCollapsed();
    DrawWindowControls();
    if (!expanded)
    {
        document.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        ImGui::End();
        if (!visible && !document.Dirty)
        {
            *open = false;
        }
        else if (!visible)
        {
            document.PendingClose = document.ConfirmSwitch = true;
        }
        return;
    }
    if (!visible)
    {
        if (document.Dirty)
        {
            document.PendingClose = document.ConfirmSwitch = true;
        }
        else
        {
            *open = false;
        }
    }
    document.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("ファイル"))
        {
            if (ImGui::MenuItem("保存", "Ctrl+S", false, document.Loaded))
            {
                SaveDocument();
            }
            if (ImGui::MenuItem("別名で保存", "Ctrl+Shift+S", false, document.Loaded))
            {
                SaveDocument(true);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("編集"))
        {
            if (ImGui::MenuItem("元に戻す", "Ctrl+Z", false, !document.Undo.empty() || document.EditStart.has_value()))
            {
                History(false);
            }
            if (ImGui::MenuItem("やり直す", "Ctrl+Y", false, !document.Redo.empty()))
            {
                History(true);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("表示"))
        {
            ImGui::MenuItem("プレビューのグリッド", nullptr, &document.ShowGrid);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    DrawToolbar();
    ImGui::Separator();
    if (document.Loaded)
    {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_S))
            {
                SaveDocument(ImGui::GetIO().KeyShift);
            }
            if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Z))
            {
                History(ImGui::GetIO().KeyShift);
            }
            if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Y))
            {
                History(true);
            }
        }
        const System before = document.Data;
        bool changed = false;
        const float fullHeight = (std::max)(240.0f, ImGui::GetContentRegionAvail().y - 26.0f);
        const float timelineHeight = (std::clamp)(fullHeight * 0.25f, 165.0f, 240.0f);
        const float height = (std::max)(100.0f, fullHeight - timelineHeight - 8);
        if (ImGui::BeginTable("NiagaraWorkspace", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                              {0, fullHeight}))
        {
            ImGui::TableSetupColumn("Workspace", ImGuiTableColumnFlags_WidthStretch, 0.76f);
            ImGui::TableSetupColumn("Selection", ImGuiTableColumnFlags_WidthStretch, 0.24f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("NiagaraWorkspaceBody", {0, fullHeight});
            const bool graphView = document.GraphView;
            if (ImGui::BeginTable("NiagaraPanels", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                                  {0, height}))
            {
                ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.23f);
                ImGui::TableSetupColumn("Parameters", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableSetupColumn("Overview / Graph", ImGuiTableColumnFlags_WidthStretch, 0.52f);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::BeginChild("NiagaraPreviewPanel", {0, height}, ImGuiChildFlags_Borders);
                DrawPreview();
                ImGui::EndChild();
                ImGui::TableSetColumnIndex(1);
                ImGui::BeginChild("NiagaraScriptSidebar", {0, height});
                if (graphView)
                {
                    ImGui::BeginChild("NiagaraScriptDetails", {0, height * 0.32f}, ImGuiChildFlags_Borders);
                    PanelTitle("スクリプト詳細 / Script Details");
                    auto* module = SelectedModule();
                    if (module && module->Kind == ModuleKind::CustomGraph)
                    {
                        changed |= DrawModuleValues(*module);
                    }
                    else
                    {
                        ImGui::TextWrapped("Custom Graphモジュールを選択してください。");
                    }
                    ImGui::EndChild();
                }
                ImGui::BeginChild("NiagaraParametersPanel", {0, graphView ? height * 0.47f : 0},
                                  ImGuiChildFlags_Borders);
                changed |= DrawParameters();
                ImGui::EndChild();
                if (graphView)
                {
                    ImGui::BeginChild("NiagaraGraphStats", {0, 0}, ImGuiChildFlags_Borders);
                    PanelTitle("統計 / Stats");
                    if (const auto* module = SelectedModule(); module && module->Kind == ModuleKind::CustomGraph)
                    {
                        ImGui::Text("Nodes: %zu / Links: %zu", module->Graph.Nodes.size(), module->Graph.Links.size());
                    }
                    ImGui::EndChild();
                }
                ImGui::EndChild();
                ImGui::TableSetColumnIndex(2);
                ImGui::BeginChild("OverviewAndGraph", {0, height}, ImGuiChildFlags_Borders);
                if (ImGui::BeginTabBar("NiagaraCenter"))
                {
                    if (ImGui::BeginTabItem("システム概要"))
                    {
                        document.GraphView = false;
                        changed |= DrawOverview();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("モジュールグラフ"))
                    {
                        document.GraphView = true;
                        auto* module = SelectedModule();
                        if (module && module->Kind == ModuleKind::CustomGraph)
                        {
                            if (!document.GraphContext || document.GraphModule != module->Id)
                            {
                                ClearGraphContext();
                                Nodes::Config config;
                                config.SettingsFile = nullptr;
                                document.GraphContext = Nodes::CreateEditor(&config);
                                document.GraphModule = module->Id;
                                Nodes::SetCurrentEditor(document.GraphContext);
                                for (const auto& node : module->Graph.Nodes)
                                {
                                    Nodes::SetNodePosition(Nodes::NodeId(node.Id),
                                                           {node.Position[0], node.Position[1]});
                                }
                                Nodes::SetCurrentEditor(nullptr);
                            }
                            changed |= NiagaraGraph::Draw(*module, document.GraphContext, document.Diagnostic);
                        }
                        else
                        {
                            ImGui::TextWrapped(
                                "Custom Graphモジュールを追加・選択すると、マテリアルと共通のノードで粒子処理を編集できます。");
                        }
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndChild();
                ImGui::EndTable();
            }
            ImGui::BeginChild("NiagaraBottomPanel", {0, 0}, ImGuiChildFlags_Borders);
            if (ImGui::BeginTabBar("NiagaraBottomTabs"))
            {
                if (ImGui::BeginTabItem("タイムライン"))
                {
                    changed |= DrawTimeline();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("カーブ"))
                {
                    auto* module = SelectedModule();
                    if (module &&
                        (module->Kind == ModuleKind::ColorOverLife || module->Kind == ModuleKind::ScaleOverLife))
                    {
                        changed |= DrawCurve(*module);
                    }
                    else
                    {
                        ImGui::TextDisabled("Color Over Life / Scale Over Lifeを選択してください。");
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Niagaraログ"))
                {
                    ImGui::TextWrapped("%s", document.Diagnostic.empty() ? "システム検証: エラーなし"
                                                                         : document.Diagnostic.c_str());
                    if (!document.Preview.Diagnostic().empty())
                    {
                        ImGui::TextWrapped("%s", document.Preview.Diagnostic().c_str());
                    }
                    if (!NiagaraRenderer::LastError().empty())
                    {
                        ImGui::TextWrapped("%s", NiagaraRenderer::LastError().c_str());
                    }
                    if (!document.Status.empty())
                    {
                        ImGui::TextWrapped("%s", document.Status.c_str());
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::EndChild();
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginChild("NiagaraSelection", {0, fullHeight}, ImGuiChildFlags_Borders);
            changed |= DrawSelection();
            ImGui::EndChild();
            ImGui::EndTable();
        }
        if (changed)
        {
            if (!document.EditStart)
            {
                document.EditStart = before;
            }
            document.Redo.clear();
            document.Dirty = true;
            document.RuntimeDirty = true;
        }
        if (!ImGui::IsAnyItemActive())
        {
            FinishEdit();
            if (document.RuntimeDirty)
            {
                RefreshRuntime();
                document.RuntimeDirty = false;
            }
        }
        if (document.Preview.Failed())
        {
            document.Playing = false;
        }
        if (document.LastUpdateFrame != ImGui::GetFrameCount())
        {
            document.LastUpdateFrame = ImGui::GetFrameCount();
            document.Preview.UpdateSeek(60);
            if (document.Playing && !document.Preview.Seeking())
            {
                document.Preview.Advance((std::min)(ImGui::GetIO().DeltaTime, 0.1f) * document.Speed);
            }
        }
        if (document.Preview.Seeking())
        {
            ImGui::TextUnformatted("シーク位置を再計算中…");
            ImGui::SameLine();
            if (ImGui::SmallButton("シークを中断"))
            {
                Seek(document.Preview.Time());
            }
        }
        if (!document.Preview.Diagnostic().empty())
        {
            ImGui::TextWrapped("%s", document.Preview.Diagnostic().c_str());
        }
    }
    else
    {
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Niagaraシステムを新規作成するか、アセットブラウザーで.naiagaraファイルをダブルクリックしてください。");
    }
    if (!document.Diagnostic.empty())
    {
        ImGui::TextColored({1.0f, 0.55f, 0.35f, 1.0f}, "%s", document.Diagnostic.c_str());
    }
    if (!document.Status.empty())
    {
        ImGui::TextWrapped("%s", document.Status.c_str());
    }
    ConfirmPending(open);
    ImGui::End();
}

void NiagaraWindow::Shutdown()
{
    ClearGraphContext();
    if (document.OverviewContext)
    {
        Nodes::DestroyEditor(document.OverviewContext);
    }
    document = Document{};
}

bool NiagaraWindow::HasFocus()
{
    return document.Focused && document.LastDrawFrame >= ImGui::GetFrameCount() - 1;
}

bool NiagaraWindow::CanQuit()
{
    if (!document.Dirty)
    {
        return true;
    }
    document.PendingQuit = true;
    document.PendingClose = document.PendingNew = false;
    document.ConfirmSwitch = true;
    return false;
}
