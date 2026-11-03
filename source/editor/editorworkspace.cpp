#include "pch.h"
#include "editorworkspace.h"
#include "materialasset.h"
#include "editorpreferences.h"
#include <imgui_internal.h>
#include <json/json.h>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace
{
    constexpr const char* WorkspacePath = "Save/editor_workspaces.json";
    constexpr size_t MaxWorkspaces = 64;

    bool Contains(const std::vector<std::string>& values, const std::string& value)
    {
        return std::find(values.begin(), values.end(), value) != values.end();
    }
}

void EditorWindowRegistry::RegisterWindow(EditorWindowDefinition definition)
{
    if (definition.Id.empty() || !definition.Visible)
    {
        return;
    }
    for (auto& window : m_Windows)
    {
        if (window.Id == definition.Id)
        {
            window = std::move(definition);
            return;
        }
    }
    m_Windows.push_back(std::move(definition));
}

void EditorWindowRegistry::UnregisterWindow(const std::string& id)
{
    std::erase_if(m_Windows, [&](const auto& window) {
        return window.Id == id;
    });
}

void EditorWindowRegistry::ShowWindow(const std::string& id)
{
    for (const auto& window : m_Windows)
    {
        if (window.Id == id)
        {
            *window.Visible = true;
        }
    }
}

void EditorWindowRegistry::HideWindow(const std::string& id)
{
    for (const auto& window : m_Windows)
    {
        if (window.Id == id)
        {
            *window.Visible = false;
        }
    }
}

void EditorWindowRegistry::ShowEditorType(const std::string& id)
{
    auto& workspaces = EditorWorkspaceManager::Get();
    if (!workspaces.IsAreaDrawn(id))
    {
        workspaces.RequestMaximize(id);
    }
    if (IsTypeVisible(id))
    {
        return;
    }
    ShowWindow(id);
    m_PendingTypes[id] = id;
}

void EditorWindowRegistry::DrawVisibleWindows()
{
    for (const auto& area : m_Windows)
    {
        if (!*area.Visible || !area.Draw || !EditorWorkspaceManager::Get().IsAreaDrawn(area.Id))
        {
            continue;
        }
        const auto selected = m_AreaTypes.find(area.Id);
        const auto& typeId = selected == m_AreaTypes.end() ? area.Id : selected->second;
        const auto renderer = std::find_if(m_Windows.begin(), m_Windows.end(), [&](const auto& type) {
            return type.Id == typeId && type.Draw;
        });
        m_DrawingArea = &area;
        if (renderer != m_Windows.end())
        {
            const bool sourceVisible = *renderer->Visible;
            *renderer->Visible = true;
            renderer->Draw();
            if (renderer->Visible != area.Visible)
            {
                *renderer->Visible = sourceVisible;
            }
        }
        else
        {
            area.Draw();
        }
        m_DrawingArea = nullptr;
    }
}

std::string EditorWindowRegistry::AreaTitle(const EditorWindowDefinition& area) const
{
    const auto selected = m_AreaTypes.find(area.Id);
    const auto& typeId = selected == m_AreaTypes.end() ? area.Id : selected->second;
    const auto type = std::find_if(m_Windows.begin(), m_Windows.end(), [&](const auto& window) {
        return window.Id == typeId;
    });
    return (type == m_Windows.end() ? area.Label : type->Label) + "###EditorArea_" + area.Id;
}

void EditorWindowRegistry::RestoreAreaTypes(const std::unordered_map<std::string, std::string>& types)
{
    m_AreaTypes.clear();
    m_PendingTypes.clear();
    for (const auto& [area, type] : types)
    {
        const auto known = std::find_if(m_Windows.begin(), m_Windows.end(), [&](const auto& window) {
            return window.Id == type && window.Draw;
        });
        if (known != m_Windows.end())
        {
            m_AreaTypes.emplace(area, type);
        }
    }
}

void EditorWindowRegistry::ApplyPendingTypes()
{
    for (const auto& [area, type] : m_PendingTypes)
    {
        m_AreaTypes[area] = type;
    }
    m_PendingTypes.clear();
}

bool EditorWindowRegistry::IsTypeVisible(const std::string& id) const
{
    for (const auto& area : m_Windows)
    {
        const auto selected = m_AreaTypes.find(area.Id);
        if (*area.Visible && EditorWorkspaceManager::Get().IsAreaDrawn(area.Id) &&
            (selected == m_AreaTypes.end() ? area.Id : selected->second) == id)
        {
            return true;
        }
    }
    return false;
}

bool EditorWindowRegistry::BeginEditorWindow(const char* title, bool* open, ImGuiWindowFlags flags)
{
    auto& registry = EditorWorkspaceManager::Get().Registry();
    const auto* area = registry.m_DrawingArea;
    if (!area)
    {
        return ImGui::Begin(title, open, flags);
    }
    auto& workspaces = EditorWorkspaceManager::Get();
    if (workspaces.IsMaximized(area->Id))
    {
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(workspaces.AreaWorkPosition());
        ImGui::SetNextWindowSize(workspaces.AreaWorkSize());
        flags &= ~ImGuiWindowFlags_AlwaysAutoResize;
        flags |= ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    }
    const auto label = registry.AreaTitle(*area);
    // Area owns visibility. Changing editor type does not close its source editor's other areas.
    const bool expanded = ImGui::Begin(label.c_str(), area->Visible, flags);
    if (!*area->Visible && workspaces.IsMaximized(area->Id))
    {
        workspaces.RequestMaximize(area->Id);
    }
    if (!expanded)
    {
        return false;
    }
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->DockId == 0 && !workspaces.IsMaximized(area->Id))
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 available = viewport->WorkSize;
        const ImVec2 size(ImMin(window->Size.x, available.x), ImMin(window->Size.y, available.y));
        const ImVec2 maximum(viewport->WorkPos.x + available.x - size.x, viewport->WorkPos.y + available.y - size.y);
        const ImVec2 position(ImClamp(window->Pos.x, viewport->WorkPos.x, maximum.x),
                              ImClamp(window->Pos.y, viewport->WorkPos.y, maximum.y));
        if (size.x != window->Size.x || size.y != window->Size.y)
        {
            ImGui::SetWindowSize(size);
        }
        if (position.x != window->Pos.x || position.y != window->Pos.y)
        {
            ImGui::SetWindowPos(position);
        }
    }
    const bool ctrlSpace = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                           !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl &&
                           ImGui::IsKeyPressed(ImGuiKey_Space, false);
    if (ctrlSpace)
    {
        workspaces.RequestMaximize(area->Id);
    }
    if (!EditorPreferences::Get().ShowAreaHeaders && !workspaces.IsMaximized(area->Id))
    {
        // ヘッダー非表示（設定）時は最大化している領域以外ではヘッダー行を省略する。Ctrl+Space は有効
        return true;
    }
    // Blender のエリアヘッダーのように、1 行・小さめのフレームで表示する
    const float uiScale = EditorPreferences::Get().UiScale;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * uiScale, 1.0f * uiScale));
    const bool maximized = workspaces.IsMaximized(area->Id);
    const char* maximizeLabel = maximized ? "⤡##AreaMaximize" : "⤢##AreaMaximize";
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float maximizeWidth =
        ImGui::CalcTextSize(maximizeLabel, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const bool controlsFitOnOneRow = availableWidth >= 80.0f + maximizeWidth + ImGui::GetStyle().ItemSpacing.x;
    float selectorWidth = availableWidth;
    if (controlsFitOnOneRow)
    {
        selectorWidth -= maximizeWidth + ImGui::GetStyle().ItemSpacing.x;
    }
    ImGui::SetNextItemWidth((std::max)(1.0f, (std::min)(200.0f * uiScale, selectorWidth)));
    const auto selected = registry.m_AreaTypes.find(area->Id);
    const auto& typeId = selected == registry.m_AreaTypes.end() ? area->Id : selected->second;
    if (ImGui::BeginCombo("##EditorType", label.substr(0, label.find("###")).c_str(), ImGuiComboFlags_HeightLarge))
    {
        for (const auto& type : registry.m_Windows)
        {
            if (!type.Draw)
            {
                continue;
            }
            if (ImGui::Selectable(type.Label.c_str(), type.Id == typeId))
            {
                registry.m_PendingTypes[area->Id] = type.Id;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Editor Type: 配置とサイズを維持して、この領域の機能を切り替えます");
    if (controlsFitOnOneRow)
    {
        ImGui::SameLine(ImGui::GetCursorPosX() + availableWidth - maximizeWidth);
    }
    if (ImGui::SmallButton(maximizeLabel))
    {
        workspaces.RequestMaximize(area->Id);
    }
    ImGui::SetItemTooltip(maximized ? "元の配置に戻す (Ctrl+Space)" : "この領域を最大化 (Ctrl+Space)");
    ImGui::PopStyleVar();
    ImGui::Separator();
    return true;
}

void EditorWindowRegistry::DrawWindowMenu()
{
    for (const auto& window : m_Windows)
    {
        ImGui::PushID(window.Id.c_str());
        std::string label = window.Label;
        const auto selected = m_AreaTypes.find(window.Id);
        if (selected != m_AreaTypes.end() && selected->second != window.Id)
        {
            const auto type = std::find_if(m_Windows.begin(), m_Windows.end(), [&](const auto& candidate) {
                return candidate.Id == selected->second;
            });
            if (type != m_Windows.end())
            {
                label += " / " + type->Label;
            }
        }
        ImGui::MenuItem(label.c_str(), nullptr, window.Visible);
        ImGui::PopID();
    }
}

EditorWorkspaceManager& EditorWorkspaceManager::Get()
{
    static EditorWorkspaceManager manager;
    return manager;
}

EditorWorkspaceManager::Workspace EditorWorkspaceManager::MakeWorkspace(const std::string& name,
                                                                        const std::string& preset)
{
    Workspace workspace;
    do
    {
        workspace.Id = "workspace_" + std::to_string(m_NextId++);
    } while (std::any_of(m_Workspaces.begin(), m_Workspaces.end(), [&](const auto& existing) {
        return existing.Id == workspace.Id;
    }));
    workspace.Name = name;
    workspace.Preset = preset;
    if (preset == "Shading")
    {
        workspace.VisibleWindows = {"scene", "material", "inspector", "assets", "console"};
    }
    else if (preset == "Animation")
    {
        workspace.VisibleWindows = {"scene", "hierarchy", "inspector", "animation", "sequencer"};
    }
    else if (preset == "Sequencer")
    {
        workspace.VisibleWindows = {"scene", "hierarchy", "inspector", "sequencer", "assets"};
    }
    else if (preset == "Rendering")
    {
        workspace.VisibleWindows = {"scene", "render_debug", "performance", "gbuffer", "render_settings"};
    }
    else if (preset == "Procedural")
    {
        workspace.VisibleWindows = {"procedural_studio", "procedural", "textures", "console"};
    }
    else
    {
        workspace.VisibleWindows = {"scene", "hierarchy", "inspector", "assets", "console"};
    }
    return workspace;
}

void EditorWorkspaceManager::Initialize(bool persistent)
{
    if (m_Initialized)
    {
        return;
    }
    m_Initialized = true;
    m_Persistent = persistent;
    ImGui::GetIO().IniFilename = nullptr;
    if (persistent && Load())
    {
        ApplyActive();
        return;
    }
    for (const char* preset : {"Layout", "Shading", "Animation", "Sequencer", "Rendering", "Procedural"})
    {
        m_Workspaces.push_back(MakeWorkspace(preset, preset));
    }

    // Import the old layout only once. Existing project visibility preferences are kept.
    if (persistent)
    {
        std::ifstream legacy("Save/editor_layout.cfg", std::ios::binary);
        if (legacy)
        {
            std::ostringstream contents;
            contents << legacy.rdbuf();
            m_Workspaces.front().Layout = contents.str();
        }
        if (m_Workspaces.front().Layout.empty())
        {
            size_t length = 0;
            const char* layout = ImGui::SaveIniSettingsToMemory(&length);
            m_Workspaces.front().Layout.assign(layout, length);
        }
        m_Workspaces.front().VisibleWindows.clear();
        for (const auto& window : m_Registry.Windows())
        {
            if (*window.Visible)
            {
                m_Workspaces.front().VisibleWindows.push_back(window.Id);
            }
        }
    }
    ApplyActive();
}

void EditorWorkspaceManager::CaptureActive()
{
    auto& workspace = m_Workspaces[m_Active];
    size_t length = 0;
    if (m_MaximizedArea.empty())
    {
        const char* layout = ImGui::SaveIniSettingsToMemory(&length);
        workspace.Layout.assign(layout, length);
    }
    else
    {
        workspace.Layout = m_MaximizedLayout;
    }
    workspace.VisibleWindows.clear();
    workspace.AreaTypes = m_Registry.AreaTypes();
    for (const auto& window : m_Registry.Windows())
    {
        if (*window.Visible)
        {
            workspace.VisibleWindows.push_back(window.Id);
        }
    }
}

void EditorWorkspaceManager::ApplyActive()
{
    const auto& workspace = m_Workspaces[m_Active];
    m_Registry.RestoreAreaTypes(workspace.AreaTypes);
    ImGui::ClearIniSettings();
    if (!workspace.Layout.empty())
    {
        // Migrate legacy display-name keys to stable area identities without changing their DockId/geometry.
        std::string layout = workspace.Layout;
        for (const auto& window : m_Registry.Windows())
        {
            if (!window.Draw)
            {
                continue;
            }
            const auto oldHeader = "[Window][" + window.DockTitle + "]";
            const auto newHeader = "[Window][###EditorArea_" + window.Id + "]";
            const auto position = layout.find(oldHeader);
            if (position != std::string::npos)
            {
                layout.replace(position, oldHeader.size(), newHeader);
            }
        }
        const std::string oldMaterial = "[Window][###MaterialAssetEditor]";
        const auto materialPosition = layout.find(oldMaterial);
        if (materialPosition != std::string::npos)
        {
            layout.replace(materialPosition, oldMaterial.size(), "[Window][###EditorArea_material]");
        }
        ImGui::LoadIniSettingsFromMemory(layout.data(), layout.size());
    }
    for (const auto& window : m_Registry.Windows())
    {
        *window.Visible = Contains(workspace.VisibleWindows, window.Id);
    }
    m_BuildLayout = workspace.Layout.empty();
}

void EditorWorkspaceManager::Queue(Action action, const std::string& id)
{
    m_Action = action;
    m_ActionId = id;
}

void EditorWorkspaceManager::BeginFrame()
{
    m_Registry.ApplyPendingTypes();
    if (!m_Initialized || m_Action == Action::None)
    {
        return;
    }
    CaptureActive();
    if (m_Action == Action::Maximize)
    {
        if (m_MaximizedArea.empty())
        {
            m_MaximizedLayout = m_Workspaces[m_Active].Layout;
            m_MaximizedArea = m_ActionId;
        }
        else
        {
            m_MaximizedArea.clear();
            m_MaximizedLayout.clear();
            ApplyActive();
        }
        m_Action = Action::None;
        m_ActionId.clear();
        return;
    }
    // Switching workspaces or changing their structure always exits the temporary full-area view.
    if (!m_MaximizedArea.empty())
    {
        m_MaximizedArea.clear();
        m_MaximizedLayout.clear();
        ApplyActive();
    }
    const auto found = std::find_if(m_Workspaces.begin(), m_Workspaces.end(), [&](const auto& workspace) {
        return workspace.Id == m_ActionId;
    });
    const size_t target = static_cast<size_t>(found - m_Workspaces.begin());
    const std::string activeId = m_Workspaces[m_Active].Id;
    bool apply = false;
    if (m_Action == Action::Add && m_Workspaces.size() < MaxWorkspaces)
    {
        m_Workspaces.push_back(MakeWorkspace("Workspace", "Layout"));
        m_Active = m_Workspaces.size() - 1;
        apply = true;
    }
    else if (target < m_Workspaces.size())
    {
        switch (m_Action)
        {
            case Action::Switch:
                m_Active = target;
                apply = true;
                break;
            case Action::Duplicate:
                if (m_Workspaces.size() < MaxWorkspaces)
                {
                    Workspace copy = m_Workspaces[target];
                    copy.Id = MakeWorkspace(copy.Name, copy.Preset).Id;
                    copy.Name += " Copy";
                    m_Workspaces.insert(m_Workspaces.begin() + target + 1, std::move(copy));
                    m_Active = target + 1;
                    apply = true;
                }
                break;
            case Action::Delete:
                if (m_Workspaces.size() > 1)
                {
                    const bool deletedActive = target == m_Active;
                    m_Workspaces.erase(m_Workspaces.begin() + target);
                    if (deletedActive)
                    {
                        m_Active = (std::min)(target, m_Workspaces.size() - 1);
                        apply = true;
                    }
                    else if (target < m_Active)
                    {
                        --m_Active;
                    }
                }
                break;
            case Action::MoveLeft:
            case Action::MoveRight: {
                const size_t other = m_Action == Action::MoveLeft ? (target == 0 ? 0 : target - 1)
                                                                  : (std::min)(target + 1, m_Workspaces.size() - 1);
                std::swap(m_Workspaces[target], m_Workspaces[other]);
                for (size_t i = 0; i < m_Workspaces.size(); ++i)
                {
                    if (m_Workspaces[i].Id == activeId)
                    {
                        m_Active = i;
                    }
                }
                break;
            }
            case Action::Reset: {
                const auto preset = MakeWorkspace(m_Workspaces[target].Name, m_Workspaces[target].Preset);
                m_Workspaces[target].VisibleWindows = preset.VisibleWindows;
                m_Workspaces[target].AreaTypes.clear();
                m_Workspaces[target].Layout.clear();
                apply = target == m_Active;
                break;
            }
            default:
                break;
        }
    }
    m_Action = Action::None;
    m_ActionId.clear();
    if (apply)
    {
        ApplyActive();
    }
    Save();
}

void EditorWorkspaceManager::DrawTabBar()
{
    if (!m_Initialized || m_Workspaces.empty())
    {
        return;
    }
    // ツールバーと同じくビューポートの作業領域から差し引くので、ドックスペースはこの下から始まる
    const float uiScale = EditorPreferences::Get().UiScale;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float padding = 3.0f * uiScale;
    const float height = ImGui::GetFrameHeight() + padding * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * uiScale, padding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorPreferences::PanelDark());
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus;
    const bool open = ImGui::BeginViewportSideBar("##EditorWorkspaceBar", viewport, ImGuiDir_Up, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open)
    {
        DrawTabs(0.0f);
        // ドック領域との境界線
        const ImVec2 min = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, min.y + size.y - 1.0f), ImVec2(min.x + size.x, min.y + size.y - 1.0f),
                                            ImGui::GetColorU32(ImGuiCol_Border));
    }
    ImGui::End();
}

void EditorWorkspaceManager::DrawTabs(float reserveRight)
{
    // DrawTabBar（ツールバー下の専用バー）から呼ぶ。
    const float uiScale = EditorPreferences::Get().UiScale;
    const ImVec4 accent = EditorPreferences::Accent();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * uiScale, ImGui::GetStyle().ItemSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
    size_t overflowFrom = m_Workspaces.size();
    for (size_t i = 0; i < m_Workspaces.size(); ++i)
    {
        auto& workspace = m_Workspaces[i];
        const float width = ImGui::CalcTextSize(workspace.Name.c_str()).x + 22.0f * uiScale;
        if (i != 0)
        {
            ImGui::SameLine();
        }
        if (i != 0 && ImGui::GetContentRegionAvail().x - width < reserveRight + ImGui::GetFrameHeight())
        {
            overflowFrom = i;
            break;
        }
        ImGui::PushID(workspace.Id.c_str());
        const bool active = i == m_Active;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        if (ImGui::Selectable((workspace.Name + "###WorkspaceTab").c_str(), active, 0,
                              ImVec2(width, ImGui::GetFrameHeight())))
        {
            Queue(Action::Switch, workspace.Id);
        }
        if (active)
        {
            ImGui::PopStyleColor(2);
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(start.x, start.y + ImGui::GetFrameHeight() - 2.0f * uiScale),
                ImVec2(start.x + width, start.y + ImGui::GetFrameHeight()), ImGui::GetColorU32(accent));
        }
        ImGui::SetItemTooltip("Workspace: %s\n右クリック: 名前変更 / 複製 / 移動 / 削除", workspace.Name.c_str());
        if (ImGui::BeginPopupContextItem("WorkspaceMenu"))
        {
            if (ImGui::IsWindowAppearing())
            {
                snprintf(m_Rename, sizeof(m_Rename), "%s", workspace.Name.c_str());
            }
            ImGui::TextUnformatted("Workspace");
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputText("名前", m_Rename, sizeof(m_Rename), ImGuiInputTextFlags_EnterReturnsTrue) &&
                m_Rename[0] != '\0')
            {
                workspace.Name = m_Rename;
                m_ForceSave = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("複製", nullptr, false, m_Workspaces.size() < MaxWorkspaces))
            {
                Queue(Action::Duplicate, workspace.Id);
            }
            if (ImGui::MenuItem("左へ移動", nullptr, false, i > 0))
            {
                Queue(Action::MoveLeft, workspace.Id);
            }
            if (ImGui::MenuItem("右へ移動", nullptr, false, i + 1 < m_Workspaces.size()))
            {
                Queue(Action::MoveRight, workspace.Id);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("初期レイアウトに戻す"))
            {
                Queue(Action::Reset, workspace.Id);
            }
            if (ImGui::MenuItem("Workspaceを削除", nullptr, false, m_Workspaces.size() > 1))
            {
                Queue(Action::Delete, workspace.Id);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (overflowFrom < m_Workspaces.size())
    {
        // 入りきらない Workspace はドロップダウンにまとめる
        if (ImGui::BeginMenu("»##WorkspaceOverflow"))
        {
            for (size_t i = overflowFrom; i < m_Workspaces.size(); ++i)
            {
                if (ImGui::MenuItem(m_Workspaces[i].Name.c_str(), nullptr, i == m_Active))
                {
                    Queue(Action::Switch, m_Workspaces[i].Id);
                }
            }
            ImGui::EndMenu();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_Workspaces.size() >= MaxWorkspaces);
    if (ImGui::Selectable("+##AddWorkspace", false, 0, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
    {
        Queue(Action::Add, {});
    }
    ImGui::SetItemTooltip("Workspace を追加");
    ImGui::EndDisabled();
    ImGui::PopStyleVar(2);
    if (!m_SaveError.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.55f, 0.25f, 1), "レイアウト保存エラー");
        ImGui::SetItemTooltip("%s", m_SaveError.c_str());
    }
}

void EditorWorkspaceManager::RequestSwitch(size_t index)
{
    if (m_Initialized && index < m_Workspaces.size())
    {
        Queue(Action::Switch, m_Workspaces[index].Id);
    }
}

uint64_t EditorWorkspaceManager::VisibilitySignature() const
{
    // FNV-1a。表示中のウィンドウとエリアタイプの組み合わせが変わったかだけを検出する
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](const std::string& text) {
        for (unsigned char c : text)
        {
            hash = (hash ^ c) * 1099511628211ull;
        }
        hash = (hash ^ 0xFFu) * 1099511628211ull;
    };
    for (const auto& window : m_Registry.Windows())
    {
        if (*window.Visible)
        {
            mix(window.Id);
        }
    }
    for (const auto& [area, type] : m_Registry.AreaTypes())
    {
        mix(area);
        mix(type);
    }
    mix(std::to_string(m_Active));
    return hash;
}

ImGuiID EditorWorkspaceManager::DockspaceId() const
{
    // Same runtime IDs are safe: switches clear and restore the entire settings snapshot.
    // Retaining the original root also permits importing editor_layout.cfg without data loss.
    return ImGui::GetID("MainEditorDockSpace");
}

void EditorWorkspaceManager::DrawDockSpace()
{
    m_AreaWorkPosition = ImGui::GetCursorScreenPos();
    m_AreaWorkSize = ImGui::GetContentRegionAvail();
    const ImGuiID root = DockspaceId();
    if (m_BuildLayout || !ImGui::DockBuilderGetNode(root))
    {
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, ImGui::GetContentRegionAvail());
        ImGuiID center = root;
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.27f, nullptr, &center);
        const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
        const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
        const auto& preset = m_Workspaces[m_Active].Preset;
        for (const auto& window : m_Registry.Windows())
        {
            if (!*window.Visible || window.DockTitle.empty())
            {
                continue;
            }
            ImGuiID node = right;
            if (window.Id == "scene" || (preset == "Shading" && window.Id == "material") ||
                (preset == "Procedural" && window.Id == "procedural_studio"))
            {
                node = center;
            }
            else if (window.Id == "hierarchy" || window.Id == "render_debug" || window.Id == "textures")
            {
                node = left;
            }
            else if (window.Id == "assets" || window.Id == "console" || window.Id == "sequencer" ||
                     window.Id == "performance")
            {
                node = bottom;
            }
            if (preset == "Shading" && window.Id == "scene")
            {
                node = left;
            }
            const auto title = window.Draw ? m_Registry.AreaTitle(window) : window.DockTitle;
            ImGui::DockBuilderDockWindow(title.c_str(), node);
        }
        ImGui::DockBuilderFinish(root);
        m_BuildLayout = false;
    }
    ImGui::DockSpace(root, ImVec2(0, 0));
}

void EditorWorkspaceManager::RequestReset()
{
    if (m_Initialized)
    {
        Queue(Action::Reset, m_Workspaces[m_Active].Id);
    }
}

void EditorWorkspaceManager::RequestMaximize(const std::string& area)
{
    Queue(Action::Maximize, area);
}

bool EditorWorkspaceManager::IsAreaDrawn(const std::string& area) const
{
    return m_MaximizedArea.empty() || m_MaximizedArea == area;
}

bool EditorWorkspaceManager::IsMaximized(const std::string& area) const
{
    return m_MaximizedArea == area;
}

bool EditorWorkspaceManager::Load()
{
    std::ifstream input(WorkspacePath, std::ios::binary);
    if (!input)
    {
        return false;
    }
    std::error_code error;
    if (std::filesystem::file_size(WorkspacePath, error) > 32 * 1024 * 1024 || error)
    {
        return false;
    }
    Json::Value document;
    Json::CharReaderBuilder reader;
    std::string parseError;
    if (!Json::parseFromStream(reader, input, &document, &parseError) || !document.isObject() ||
        !document["version"].isInt() || document["version"].asInt() != 1 || !document["active"].isString() ||
        !document["workspaces"].isArray() || document["workspaces"].empty() ||
        document["workspaces"].size() > MaxWorkspaces)
    {
        return false;
    }
    std::vector<Workspace> loaded;
    std::unordered_set<std::string> ids;
    for (const auto& value : document["workspaces"])
    {
        if (!value.isObject() || !value["id"].isString() || !value["name"].isString() || !value["preset"].isString() ||
            !value["layout"].isString() || !value["visible"].isArray())
        {
            return false;
        }
        Workspace workspace;
        workspace.Id = value["id"].asString();
        workspace.Name = value["name"].asString();
        workspace.Preset = value["preset"].asString();
        workspace.Layout = value["layout"].asString();
        if (value["types"].isObject())
        {
            for (const auto& area : value["types"].getMemberNames())
            {
                if (value["types"][area].isString())
                {
                    workspace.AreaTypes[area] = value["types"][area].asString();
                }
            }
        }
        if (workspace.Id.empty() || workspace.Name.empty() || !ids.insert(workspace.Id).second)
        {
            return false;
        }
        for (const auto& visible : value["visible"])
        {
            if (visible.isString())
            {
                workspace.VisibleWindows.push_back(visible.asString());
            }
        }
        loaded.push_back(std::move(workspace));
    }
    m_Workspaces = std::move(loaded);
    // Never trust a persisted counter to keep IDs unique after hand editing or old versions.
    m_NextId = 1;
    while (ids.contains("workspace_" + std::to_string(m_NextId)))
    {
        ++m_NextId;
    }
    const auto active = document.get("active", "").asString();
    for (size_t i = 0; i < m_Workspaces.size(); ++i)
    {
        if (m_Workspaces[i].Id == active)
        {
            m_Active = i;
        }
    }
    return true;
}

void EditorWorkspaceManager::Save()
{
    if (!m_Persistent || !m_Initialized)
    {
        return;
    }
    Json::Value document(Json::objectValue);
    document["version"] = 1;
    document["active"] = m_Workspaces[m_Active].Id;
    document["workspaces"] = Json::Value(Json::arrayValue);
    for (const auto& workspace : m_Workspaces)
    {
        Json::Value value(Json::objectValue);
        value["id"] = workspace.Id;
        value["name"] = workspace.Name;
        value["preset"] = workspace.Preset;
        value["layout"] = workspace.Layout;
        value["visible"] = Json::Value(Json::arrayValue);
        value["types"] = Json::Value(Json::objectValue);
        for (const auto& [area, type] : workspace.AreaTypes)
        {
            value["types"][area] = type;
        }
        for (const auto& id : workspace.VisibleWindows)
        {
            value["visible"].append(id);
        }
        document["workspaces"].append(std::move(value));
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    const std::string contents = Json::writeString(writer, document);
    if (contents == m_LastSaved)
    {
        return;
    }
    std::error_code directoryError;
    std::filesystem::create_directories("Save", directoryError);
    if (directoryError)
    {
        m_SaveError = directoryError.message();
        return;
    }
    if (WriteAssetFile(WorkspacePath, contents, m_SaveError))
    {
        m_LastSaved = contents;
        m_SaveError.clear();
    }
}

void EditorWorkspaceManager::AutoSave()
{
    if (!m_Initialized || ImGui::GetTime() < m_NextSave || ImGui::IsAnyItemActive() ||
        ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        return;
    }
    m_NextSave = ImGui::GetTime() + 2.0;
    // 最適化：ドッキング配置（ImGui の ini）も表示ウィンドウも変わっていなければ、
    // 2 秒ごとの SaveIniSettingsToMemory と JSON 生成を丸ごと省く。
    ImGuiIO& io = ImGui::GetIO();
    const uint64_t signature = VisibilitySignature();
    if (!m_ForceSave && !io.WantSaveIniSettings && signature == m_LastVisibilitySignature)
    {
        return;
    }
    io.WantSaveIniSettings = false;
    m_LastVisibilitySignature = signature;
    m_ForceSave = false;
    CaptureActive();
    Save();
}

void EditorWorkspaceManager::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }
    CaptureActive();
    Save();
    m_Workspaces.clear();
    m_Registry = {};
    m_Initialized = false;
    m_Active = 0;
    m_Action = Action::None;
    m_MaximizedArea.clear();
    m_MaximizedLayout.clear();
    m_LastSaved.clear();
}

