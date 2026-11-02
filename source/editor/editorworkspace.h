#pragma once
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
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
    // Apply queued changes before NewFrame, never while docking windows are being submitted.
    void BeginFrame();
    void DrawTabs();
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
        Maximize
    };
    void CaptureActive();
    void ApplyActive();
    void Save();
    bool Load();
    Workspace MakeWorkspace(const std::string& name, const std::string& preset);
    void Queue(Action action, const std::string& id);
    ImGuiID DockspaceId() const;

    EditorWindowRegistry m_Registry;
    std::vector<Workspace> m_Workspaces;
    size_t m_Active = 0;
    unsigned m_NextId = 1;
    bool m_Initialized = false;
    bool m_Persistent = true;
    bool m_BuildLayout = false;
    double m_NextSave = 0;
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
