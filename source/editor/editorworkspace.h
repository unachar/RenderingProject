#pragma once
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <imgui.h>

// Window IDs are persistence keys; labels can be translated without changing them.
struct EditorWindowDefinition
{
    std::string Id;
    std::string Label;
    std::string DockTitle;
    bool* Visible = nullptr;
    std::function<void()> Draw;
};

class EditorWindowRegistry
{
  public:
    void RegisterWindow(EditorWindowDefinition definition);
    void UnregisterWindow(const std::string& id);
    void ShowWindow(const std::string& id);
    void ShowEditorType(const std::string& id);
    void HideWindow(const std::string& id);
    void DrawVisibleWindows();
    void DrawWindowMenu();
    // Begin/End still follow ImGui's contract. Only the primary panel window uses this entry point.
    static bool BeginEditorWindow(const char* title, bool* open = nullptr, ImGuiWindowFlags flags = 0);
    std::string AreaTitle(const EditorWindowDefinition& area) const;

    const std::unordered_map<std::string, std::string>& AreaTypes() const
    {
        return m_AreaTypes;
    }

    void RestoreAreaTypes(const std::unordered_map<std::string, std::string>& types);
    void ApplyPendingTypes();
    bool IsTypeVisible(const std::string& id) const;

    const std::vector<EditorWindowDefinition>& Windows() const
    {
        return m_Windows;
    }

  private:
    std::vector<EditorWindowDefinition> m_Windows;
    std::unordered_map<std::string, std::string> m_AreaTypes;
    std::unordered_map<std::string, std::string> m_PendingTypes;
    const EditorWindowDefinition* m_DrawingArea = nullptr;
};

class EditorWorkspaceManager
{
  public:
    static EditorWorkspaceManager& Get();

    EditorWindowRegistry& Registry()
    {
        return m_Registry;
    }

    void Initialize(bool persistent);

    size_t WorkspaceCount() const
    {
        return m_Workspaces.size();
    }

    const std::string& WorkspaceName(size_t index) const
    {
        static const std::string empty;
        return index < m_Workspaces.size() ? m_Workspaces[index].Name : empty;
    }

    size_t ActiveWorkspace() const
    {
        return m_Active;
    }

    void RequestSwitch(size_t index);
    // Apply queued changes before NewFrame, never while docking windows are being submitted.
    void BeginFrame();
    // Workspace タブ（Blender / UE 風）を現在のウィンドウ内に描く。reserveRight は右側に残す幅
    void DrawTabs(float reserveRight = 0.0f);
    // ツールバーの下（ドッキング領域の直上）に Workspace タブ専用のバーを作って描く
    void DrawTabBar();
    void DrawDockSpace();
    void RequestReset();
    void RequestMaximize(const std::string& area);
    bool IsAreaDrawn(const std::string& area) const;
    bool IsMaximized(const std::string& area) const;

    ImVec2 AreaWorkPosition() const
    {
        return m_AreaWorkPosition;
    }

    ImVec2 AreaWorkSize() const
    {
        return m_AreaWorkSize;
    }

    void AutoSave();
    void Shutdown();

  private:
    struct Workspace
    {
        std::string Id;
        std::string Name;
        std::string Preset;
        std::string Layout;
        std::vector<std::string> VisibleWindows;
        std::unordered_map<std::string, std::string> AreaTypes;
        // #15 ユーザーが保存したレイアウト（「保存したレイアウトに戻す」で復元。空 = 未保存）
        std::string SavedLayout;
        std::vector<std::string> SavedVisibleWindows;
        std::unordered_map<std::string, std::string> SavedAreaTypes;
    };
    enum class Action
    {
        None,
        Switch,
        Add,
        Duplicate,
        Delete,
        MoveLeft,
        MoveRight,
        Reset,
        Maximize,
        SaveLayout,
        RestoreSaved
    };
    void CaptureActive();
    void ApplyActive();
    void Save();
    bool Load();
    Workspace MakeWorkspace(const std::string& name, const std::string& preset);
    void Queue(Action action, const std::string& id);
    ImGuiID DockspaceId() const;
    uint64_t VisibilitySignature() const;

    EditorWindowRegistry m_Registry;
    std::vector<Workspace> m_Workspaces;
    size_t m_Active = 0;
    unsigned m_NextId = 1;
    bool m_Initialized = false;
    bool m_Persistent = true;
    bool m_BuildLayout = false;
    double m_NextSave = 0;
    uint64_t m_LastVisibilitySignature = 0;
    bool m_ForceSave = true;
    Action m_Action = Action::None;
    std::string m_ActionId;
    std::string m_LastSaved;
    std::string m_SaveError;
    std::string m_MaximizedArea;
    std::string m_MaximizedLayout;
    ImVec2 m_AreaWorkPosition{};
    ImVec2 m_AreaWorkSize{};
    char m_Rename[128]{};
};


