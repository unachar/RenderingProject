#include "pch.h"
#include "editorshell.h"
#include "editorpreferences.h"
#include "imguimanager.h"
#include "editorworkspace.h"
#include "editorscenenavigation.h"
#include "projectmanager.h"
#include "materialmanager.h"
#include "world.h"
#include "camera.h"
#include "sun.h"
#include "light.h"
#include "graphicsdevice.h"
#include "gpumemorybudget.h"
#include "performancegovernor.h"
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    using Prefs = EditorPreferences::Settings;

    // ------------------------------------------------------------------ 通知トースト
    struct Toast
    {
        std::string Text;
        EditorShell::LogLevel Level = EditorShell::LogLevel::Info;
        double Start = -1.0; // 描画スレッドで初めて表示したときに設定する
    };

    std::mutex g_ToastMutex;
    std::deque<Toast> g_PendingToasts;
    std::deque<Toast> g_ActiveToasts;

    // ------------------------------------------------------------------ コマンド検索
    struct PaletteCommand
    {
        std::string Category;
        std::string Label;
        std::string Shortcut;
        std::string SearchKey; // 小文字化した検索対象
        std::function<void()> Run;
        bool Enabled = true;
    };

    struct PaletteState
    {
        bool Open = false;
        bool JustOpened = false;
        bool ScrollToSelected = false;
        char Query[128]{};
        std::string LastQuery = "\x01";
        int Selected = 0;
        std::vector<PaletteCommand> Commands;
        std::vector<int> Filtered;
        std::vector<std::string> Recent;
    };

    PaletteState g_Palette;
    std::function<void()> g_DeferredCommand;

    // ------------------------------------------------------------------ その他の状態
    bool g_ShowShortcuts = false;
    bool g_OpenViewportAddMenu = false;

    struct PieState
    {
        bool Open = false;
        bool Sticky = false;
        ImVec2 Center{};
        double OpenTime = 0.0;
        int Hovered = -1;
    };

    PieState g_Pie;

    struct ViewAnimation
    {
        bool Active = false;
        float Time = 0.0f;
        float FromPitch = 0.0f;
        float FromYaw = 0.0f;
        float ToPitch = 0.0f;
        float ToYaw = 0.0f;
        XMFLOAT3 Pivot{};
        float Distance = 10.0f;
    };

    ViewAnimation g_ViewAnimation;
    int g_NavPressedAxis = -1;
    float g_SnapValues[3]{};

    struct PrimitiveDescriptor
    {
        const char* Label;
        const char* Name;
        ObjectType Type;
        bool Is3D;
    };

    const PrimitiveDescriptor kPrimitives[] = {
        {"球", "Sphere", ObjectType::SPHERE, true},
        {"キューブ", "Cube", ObjectType::CUBE, true},
        {"カプセル", "Capsule", ObjectType::CAPSULE, true},
        {"円柱", "Cylinder", ObjectType::CYLINDER, true},
        {"円錐", "Cone", ObjectType::CONE, true},
        {"三角錐", "Tetrahedron", ObjectType::TETRAHEDRON, true},
        {"三角柱", "Triangular Prism", ObjectType::TRIANGULAR_PRISM, true},
        {"平面", "Plane", ObjectType::PLANE, true},
        {"円", "Circle", ObjectType::CIRCLE, false},
        {"四角", "Square", ObjectType::QUAD, false},
        {"三角", "Triangle", ObjectType::TRIANGLE, false},
    };

    struct LightDescriptor
    {
        const char* Label;
        LightType Type;
    };

    const LightDescriptor kLights[] = {
        {"Directional", LightType::Directional}, {"Point", LightType::Point},
        {"Spot", LightType::Spot},               {"Rect Area", LightType::Rect},
        {"Capsule", LightType::Capsule},         {"Volume", LightType::Volume},
    };

    float Scale()
    {
        return EditorPreferences::Get().UiScale;
    }

    ImVec4 WithAlpha(ImVec4 color, float alpha)
    {
        color.w = alpha;
        return color;
    }

    std::string ToLowerAscii(const char* text)
    {
        std::string result = text ? text : "";
        for (char& c : result)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return result;
    }

    bool ContainsAnyCaseInsensitive(const std::string& lowered, std::initializer_list<const char*> needles)
    {
        for (const char* needle : needles)
        {
            if (lowered.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // アクセント色で強調するトグルボタン
    bool ToggleButton(const char* label, bool active, const char* tooltip = nullptr, ImVec2 size = ImVec2(0, 0))
    {
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, EditorPreferences::Accent());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorPreferences::AccentHovered());
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorPreferences::AccentActive());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        }
        const bool pressed = ImGui::Button(label, size);
        if (active)
        {
            ImGui::PopStyleColor(4);
        }
        if (tooltip && tooltip[0] != '\0')
        {
            ImGui::SetItemTooltip("%s", tooltip);
        }
        return pressed;
    }

    void VerticalSeparator()
    {
        ImGui::SameLine();
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(position.x + 2.0f, position.y + 3.0f),
                                            ImVec2(position.x + 2.0f, position.y + height - 3.0f),
                                            ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        ImGui::Dummy(ImVec2(5.0f, height));
        ImGui::SameLine();
    }

    // 同じ行に置けなければ改行する（狭いパネルでも崩れないヘッダー用）
    void SameLineIfFits(float width)
    {
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < width)
        {
            ImGui::NewLine();
        }
    }

    bool SnapControl(const char* id, const char* icon, bool& enabled, float& value, const float* steps, int count,
                     const char* format, const char* tooltip)
    {
        ImGui::PushID(id);
        bool changed = false;
        if (ToggleButton(icon, enabled, tooltip))
        {
            enabled = !enabled;
            changed = true;
        }
        ImGui::SameLine(0.0f, 1.0f);
        char preview[32];
        snprintf(preview, sizeof(preview), format, value);
        ImGui::SetNextItemWidth(ImGui::CalcTextSize(preview).x + ImGui::GetFrameHeight() + 10.0f * Scale());
        if (ImGui::BeginCombo("##Step", preview, ImGuiComboFlags_HeightLarge))
        {
            for (int i = 0; i < count; ++i)
            {
                char item[32];
                snprintf(item, sizeof(item), format, steps[i]);
                if (ImGui::Selectable(item, fabsf(steps[i] - value) < 1e-5f))
                {
                    value = steps[i];
                    enabled = true;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }

    const float kTranslateSteps[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f};
    const float kRotateSteps[] = {1.0f, 5.0f, 10.0f, 15.0f, 22.5f, 30.0f, 45.0f, 90.0f};
    const float kScaleSteps[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f};

    // ------------------------------------------------------------------ カメラ視点
    bool EditorCamera(EntityID& entity)
    {
        return EditorSceneNavigation::GetEditorCamera(entity);
    }

    void OrbitEditorCamera(float deltaYaw, float deltaPitch)
    {
        EntityID cameraEntity;
        if (!EditorCamera(cameraEntity))
        {
            return;
        }
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
        const float distance = (std::max)(EditorSceneNavigation::PivotDistance(), 0.5f);
        const XMVECTOR eye = XMLoadFloat3(&transform.Position);
        const XMVECTOR pivot = XMVectorAdd(eye, XMVectorScale(EditorSceneNavigation::ForwardOf(transform), distance));
        transform.Rotation.y += deltaYaw;
        transform.Rotation.x =
            (std::clamp)(transform.Rotation.x + deltaPitch, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
        const XMVECTOR forward = EditorSceneNavigation::ForwardOf(transform);
        EditorSceneNavigation::Focus().Active = false;
        g_ViewAnimation.Active = false;
        EditorSceneNavigation::Store(transform, camera, XMVectorSubtract(pivot, XMVectorScale(forward, distance)),
                                     forward);
    }

    void UpdateViewAnimation()
    {
        auto& animation = g_ViewAnimation;
        if (!animation.Active)
        {
            return;
        }
        EntityID cameraEntity;
        if (!EditorCamera(cameraEntity))
        {
            animation.Active = false;
            return;
        }
        auto& camera = ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity);
        auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
        animation.Time += ImGui::GetIO().DeltaTime;
        const float t = (std::min)(animation.Time / 0.22f, 1.0f);
        const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        transform.Rotation.x = animation.FromPitch + (animation.ToPitch - animation.FromPitch) * eased;
        transform.Rotation.y = animation.FromYaw + (animation.ToYaw - animation.FromYaw) * eased;
        const XMVECTOR forward = EditorSceneNavigation::ForwardOf(transform);
        const XMVECTOR eye =
            XMVectorSubtract(XMLoadFloat3(&animation.Pivot), XMVectorScale(forward, animation.Distance));
        EditorSceneNavigation::Store(transform, camera, eye, forward);
        if (t >= 1.0f)
        {
            animation.Active = false;
        }
    }
} // namespace

// ============================================================================ ログ / 通知
EditorShell::LogLevel EditorShell::ClassifyLog(const char* text)
{
    const std::string lowered = ToLowerAscii(text);
    if (ContainsAnyCaseInsensitive(lowered, {"error", "failed", "failure", "exception", "invalid", "crash"}) ||
        strstr(text, "失敗") || strstr(text, "エラー") || strstr(text, "できません") || strstr(text, "不正"))
    {
        return LogLevel::Error;
    }
    if (ContainsAnyCaseInsensitive(lowered, {"warn", "not found", "missing", "skipped", "fallback"}) ||
        strstr(text, "警告") || strstr(text, "見つかりません") || strstr(text, "スキップ") ||
        strstr(text, "未対応"))
    {
        return LogLevel::Warning;
    }
    if (ContainsAnyCaseInsensitive(lowered, {"saved", "success", "completed"}) || strstr(text, "保存しました") ||
        strstr(text, "完了") || strstr(text, "成功"))
    {
        return LogLevel::Success;
    }
    return LogLevel::Info;
}

void EditorShell::PushToast(LogLevel level, const char* text)
{
    const auto& prefs = EditorPreferences::Get();
    if (!prefs.ShowToasts || !text || text[0] == '\0' || level == LogLevel::Info ||
        (level == LogLevel::Success && !prefs.ToastOnSuccess))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_ToastMutex);
    // 同じ内容が連続した場合は 1 件にまとめる（毎フレーム出るエラーで画面が埋まらないように）
    for (auto& pending : g_PendingToasts)
    {
        if (pending.Text == text)
        {
            return;
        }
    }
    for (auto& active : g_ActiveToasts)
    {
        if (active.Text == text)
        {
            active.Start = -1.0; // 表示時間をリセット
            return;
        }
    }
    g_PendingToasts.push_back({text, level, -1.0});
    while (g_PendingToasts.size() > 8)
    {
        g_PendingToasts.pop_front();
    }
}

ImVec4 EditorShell::LogLevelColor(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Success:
            return EditorPreferences::Success();
        case LogLevel::Warning:
            return EditorPreferences::Warning();
        case LogLevel::Error:
            return EditorPreferences::Error();
        default:
            return EditorPreferences::TextColor();
    }
}

const char* EditorShell::LogLevelIcon(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Success:
            return "✓";
        case LogLevel::Warning:
            return "⚠";
        case LogLevel::Error:
            return "✕";
        default:
            return "•";
    }
}

// ============================================================================ 共通メニュー
void EditorShell::DrawAddMenuItems()
{
    if (ImGui::MenuItem("空のオブジェクト"))
    {
        ImGuiManager::SelectEntity(ImGuiManager::CreateEmptyEntity());
    }
    if (ImGui::BeginMenu("3D シェイプ"))
    {
        for (const auto& primitive : kPrimitives)
        {
            if (primitive.Is3D && ImGui::MenuItem(primitive.Label))
            {
                ImGuiManager::CreatePrimitiveEntity(primitive.Name, primitive.Type);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("2D シェイプ"))
    {
        for (const auto& primitive : kPrimitives)
        {
            if (!primitive.Is3D && ImGui::MenuItem(primitive.Label))
            {
                ImGuiManager::CreatePrimitiveEntity(primitive.Name, primitive.Type);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("ライト"))
    {
        for (const auto& light : kLights)
        {
            if (ImGui::MenuItem(light.Label))
            {
                ImGuiManager::SelectEntity(ImGuiManager::CreateLightEntity(light.Type));
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Sun"))
        {
            ImGuiManager::SelectEntity(Sun::CreateDefault());
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("ゲームカメラ"))
    {
        ImGuiManager::SelectEntity(ImGuiManager::CreateGameCameraEntity());
    }
}

void EditorShell::DrawViewMenuItems()
{
    auto& prefs = EditorPreferences::Get();
    ImGui::MenuItem("ライト可視化", nullptr, &ImGuiManager::m_ShowLightDebug);
    ImGui::MenuItem("ナビゲーションギズモ", nullptr, &prefs.ShowNavigationGizmo);
    ImGui::MenuItem("統計オーバーレイ", nullptr, &prefs.ShowViewportStats);
    ImGui::Separator();
    if (ImGui::MenuItem("正面", "Num 1"))
        SetViewDirection(0.0f, 0.0f, 1.0f);
    if (ImGui::MenuItem("背面", "Ctrl+Num 1"))
        SetViewDirection(0.0f, 0.0f, -1.0f);
    if (ImGui::MenuItem("右", "Num 3"))
        SetViewDirection(-1.0f, 0.0f, 0.0f);
    if (ImGui::MenuItem("左", "Ctrl+Num 3"))
        SetViewDirection(1.0f, 0.0f, 0.0f);
    if (ImGui::MenuItem("上", "Num 7"))
        SetViewDirection(0.0f, -1.0f, 0.0f);
    if (ImGui::MenuItem("下", "Ctrl+Num 7"))
        SetViewDirection(0.0f, 1.0f, 0.0f);
    ImGui::Separator();
    if (ImGui::MenuItem("選択にフォーカス", "F / Num ."))
    {
        EditorSceneNavigation::FocusOn(ImGuiManager::m_SelectedEntity);
    }
}

// ============================================================================ ショートカット / コマンド検索
void EditorShell::HandleGlobalShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
    {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
    {
        ToggleShortcutHelp();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false))
    {
        OpenCommandPalette();
    }
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Comma, false))
    {
        EditorWorkspaceManager::Get().Registry().ShowEditorType("preferences");
    }
}

void EditorShell::ToggleShortcutHelp()
{
    g_ShowShortcuts = !g_ShowShortcuts;
}

void EditorShell::OpenCommandPalette()
{
    auto& palette = g_Palette;
    palette.Open = true;
    palette.JustOpened = true;
    palette.Query[0] = '\0';
    palette.LastQuery = "\x01";
    palette.Selected = 0;
    palette.Commands.clear();

    auto add = [&](const char* category, std::string label, const char* shortcut, std::function<void()> run,
                   bool enabled = true) {
        PaletteCommand command;
        command.Category = category;
        command.Label = std::move(label);
        command.Shortcut = shortcut ? shortcut : "";
        command.SearchKey = ToLowerAscii((command.Category + " " + command.Label + " " + command.Shortcut).c_str());
        command.Run = std::move(run);
        command.Enabled = enabled;
        palette.Commands.push_back(std::move(command));
    };

    const bool hasSelection =
        ImGuiManager::m_SelectedEntity != g_kINVALID_ENTITY && Registry::IsAlive(ImGuiManager::m_SelectedEntity);

    add("ファイル", "新規シーン", "Ctrl+N", [] { ImGuiManager::RequestNewScene(); });
    add("ファイル", "シーンを保存", "Ctrl+S", [] { SaveSceneAndReferences(false); });
    add("ファイル", "名前を付けてシーンを保存", "Ctrl+Shift+S", [] { SaveSceneAndReferences(true); });
    add("ファイル", "終了", "Alt+F4", [] { ImGuiManager::RequestQuit(); });

    add("編集", "元に戻す", "Ctrl+Z", [] { ImGuiManager::Undo(); }, !ImGuiManager::m_UndoStack.empty());
    add("編集", "やり直し", "Ctrl+Y", [] { ImGuiManager::Redo(); }, !ImGuiManager::m_RedoStack.empty());
    add("編集", "複製", "Ctrl+D", [] { ImGuiManager::DuplicateSelectedEntity(); }, hasSelection);
    add("編集", "削除", "Delete", [] { ImGuiManager::DeleteSelectedEntity(); }, hasSelection);
    add("編集", "選択解除", "", [] { ImGuiManager::SelectEntity(g_kINVALID_ENTITY); }, hasSelection);
    add("編集", "選択にフォーカス", "F", [] { EditorSceneNavigation::FocusOn(ImGuiManager::m_SelectedEntity); },
        hasSelection);
    add("編集", "Undo History を開く", "", [] { EditorWorkspaceManager::Get().Registry().ShowEditorType("history"); });
    add("編集", "エディター設定", "Ctrl+,",
        [] { EditorWorkspaceManager::Get().Registry().ShowEditorType("preferences"); });

    add("ツール", "ハンド", "Q", [] { ImGuiManager::m_GizmoOperation = 3; });
    add("ツール", "移動", "W", [] { ImGuiManager::m_GizmoOperation = 0; });
    add("ツール", "回転", "E", [] { ImGuiManager::m_GizmoOperation = 1; });
    add("ツール", "スケール", "R", [] { ImGuiManager::m_GizmoOperation = 2; });
    add("ツール", "ワールド / ローカル切り替え", "X",
        [] { ImGuiManager::m_GizmoLocalSpace = !ImGuiManager::m_GizmoLocalSpace; });
    add("ツール", "移動スナップ 切り替え", "",
        [] { EditorPreferences::Get().Snap.Translate = !EditorPreferences::Get().Snap.Translate; });
    add("ツール", "回転スナップ 切り替え", "",
        [] { EditorPreferences::Get().Snap.Rotate = !EditorPreferences::Get().Snap.Rotate; });
    add("ツール", "スケールスナップ 切り替え", "",
        [] { EditorPreferences::Get().Snap.Scale = !EditorPreferences::Get().Snap.Scale; });

    add("再生", ProjectManager::IsPlaying() ? "停止" : "再生", "Ctrl+P", [] { ProjectManager::TogglePlay(); });
    add("再生", ProjectManager::IsPaused() ? "再開" : "一時停止", "Ctrl+Shift+P", [] { ProjectManager::TogglePause(); },
        ProjectManager::IsPlaying());

    add("作成", "空のオブジェクト", "", [] { ImGuiManager::SelectEntity(ImGuiManager::CreateEmptyEntity()); });
    for (const auto& primitive : kPrimitives)
    {
        const PrimitiveDescriptor descriptor = primitive;
        add("作成", std::string(primitive.Is3D ? "3D シェイプ: " : "2D シェイプ: ") + primitive.Label + " (" +
                        primitive.Name + ")",
            "", [descriptor] { ImGuiManager::CreatePrimitiveEntity(descriptor.Name, descriptor.Type); });
    }
    for (const auto& light : kLights)
    {
        const LightType type = light.Type;
        add("作成", std::string("ライト: ") + light.Label, "",
            [type] { ImGuiManager::SelectEntity(ImGuiManager::CreateLightEntity(type)); });
    }
    add("作成", "ライト: Sun", "", [] { ImGuiManager::SelectEntity(Sun::CreateDefault()); });
    add("作成", "ゲームカメラ", "", [] { ImGuiManager::SelectEntity(ImGuiManager::CreateGameCameraEntity()); });

    add("視点", "正面", "Num 1", [] { SetViewDirection(0.0f, 0.0f, 1.0f); });
    add("視点", "背面", "Ctrl+Num 1", [] { SetViewDirection(0.0f, 0.0f, -1.0f); });
    add("視点", "右", "Num 3", [] { SetViewDirection(-1.0f, 0.0f, 0.0f); });
    add("視点", "左", "Ctrl+Num 3", [] { SetViewDirection(1.0f, 0.0f, 0.0f); });
    add("視点", "上", "Num 7", [] { SetViewDirection(0.0f, -1.0f, 0.0f); });
    add("視点", "下", "Ctrl+Num 7", [] { SetViewDirection(0.0f, 1.0f, 0.0f); });
    add("表示", "ライト可視化 切り替え", "",
        [] { ImGuiManager::m_ShowLightDebug = !ImGuiManager::m_ShowLightDebug; });
    add("表示", "ツールバー 切り替え", "",
        [] { EditorPreferences::Get().ShowToolbar = !EditorPreferences::Get().ShowToolbar; });
    add("表示", "ステータスバー 切り替え", "",
        [] { EditorPreferences::Get().ShowStatusBar = !EditorPreferences::Get().ShowStatusBar; });
    add("表示", "ショートカット一覧", "F1", [] { g_ShowShortcuts = true; });
    add("表示", "Workspace の初期レイアウトに戻す", "", [] { EditorWorkspaceManager::Get().RequestReset(); });

    for (int i = 0; i < static_cast<int>(EditorPreferences::ThemePreset::Count); ++i)
    {
        const auto preset = static_cast<EditorPreferences::ThemePreset>(i);
        add("テーマ", EditorPreferences::ThemeName(preset), "", [preset] {
            EditorPreferences::Get().Theme = preset;
            EditorPreferences::MarkStyleDirty();
        });
    }

    auto& workspaces = EditorWorkspaceManager::Get();
    for (size_t i = 0; i < workspaces.WorkspaceCount(); ++i)
    {
        add("Workspace", "切り替え: " + workspaces.WorkspaceName(i), "",
            [i] { EditorWorkspaceManager::Get().RequestSwitch(i); }, i != workspaces.ActiveWorkspace());
    }
    for (const auto& window : workspaces.Registry().Windows())
    {
        const std::string id = window.Id;
        const bool visible = window.Visible && *window.Visible;
        add("ウィンドウ", (visible ? "閉じる: " : "開く: ") + window.Label, "", [id, visible] {
            auto& registry = EditorWorkspaceManager::Get().Registry();
            if (visible)
            {
                registry.HideWindow(id);
            }
            else
            {
                registry.ShowEditorType(id);
            }
        });
    }
}

namespace
{
    int PaletteInputCallback(ImGuiInputTextCallbackData* data)
    {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
        {
            if (data->EventKey == ImGuiKey_UpArrow)
            {
                --g_Palette.Selected;
            }
            else if (data->EventKey == ImGuiKey_DownArrow)
            {
                ++g_Palette.Selected;
            }
            g_Palette.ScrollToSelected = true;
        }
        return 0;
    }

    void RefilterPalette()
    {
        auto& palette = g_Palette;
        palette.Filtered.clear();
        const std::string query = ToLowerAscii(palette.Query);
        std::vector<std::string> tokens;
        size_t start = 0;
        while (start < query.size())
        {
            const size_t end = query.find(' ', start);
            const std::string token = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!token.empty())
            {
                tokens.push_back(token);
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }

        std::vector<std::pair<int, int>> scored; // (score, index)
        for (int i = 0; i < static_cast<int>(palette.Commands.size()); ++i)
        {
            const auto& command = palette.Commands[i];
            int score = 0;
            bool match = true;
            for (const auto& token : tokens)
            {
                const size_t found = command.SearchKey.find(token);
                if (found == std::string::npos)
                {
                    match = false;
                    break;
                }
                const std::string labelLower = ToLowerAscii(command.Label.c_str());
                const size_t inLabel = labelLower.find(token);
                score += inLabel == 0 ? 0 : (inLabel != std::string::npos ? 10 + static_cast<int>(inLabel) : 60);
            }
            if (!match)
            {
                continue;
            }
            const auto recent = std::find(palette.Recent.begin(), palette.Recent.end(), command.Label);
            if (recent != palette.Recent.end())
            {
                score -= 100 - static_cast<int>(recent - palette.Recent.begin());
            }
            if (!command.Enabled)
            {
                score += 200;
            }
            scored.emplace_back(score, i);
        }
        std::stable_sort(scored.begin(), scored.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [score, index] : scored)
        {
            palette.Filtered.push_back(index);
        }
        palette.Selected = 0;
        palette.LastQuery = palette.Query;
    }

    void RunPaletteCommand(int filteredIndex)
    {
        auto& palette = g_Palette;
        if (filteredIndex < 0 || filteredIndex >= static_cast<int>(palette.Filtered.size()))
        {
            return;
        }
        const auto& command = palette.Commands[palette.Filtered[filteredIndex]];
        if (!command.Enabled)
        {
            return;
        }
        auto recent = std::find(palette.Recent.begin(), palette.Recent.end(), command.Label);
        if (recent != palette.Recent.end())
        {
            palette.Recent.erase(recent);
        }
        palette.Recent.insert(palette.Recent.begin(), command.Label);
        if (palette.Recent.size() > 8)
        {
            palette.Recent.resize(8);
        }
        g_DeferredCommand = command.Run; // ウィンドウ外（フレームの最後）で実行する
        palette.Open = false;
    }

    void DrawCommandPalette()
    {
        auto& palette = g_Palette;
        if (!palette.Open)
        {
            return;
        }
        if (palette.LastQuery != palette.Query)
        {
            RefilterPalette();
        }
        const float scale = Scale();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = (std::min)(620.0f * scale, viewport->WorkSize.x - 40.0f);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 40.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, viewport->WorkSize.y * 0.75f));
        if (palette.JustOpened)
        {
            ImGui::SetNextWindowFocus();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 10.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
        ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(EditorPreferences::Accent(), 0.8f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoCollapse;
        if (ImGui::Begin("##CommandPalette", nullptr, flags))
        {
            ImGui::TextColored(EditorPreferences::Accent(), "⌕");
            ImGui::SameLine();
            if (palette.JustOpened)
            {
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-1.0f);
            const bool enter = ImGui::InputTextWithHint(
                "##Query", "コマンドを検索（例: ライト / 保存 / テーマ / 正面）", palette.Query, sizeof(palette.Query),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, PaletteInputCallback);
            if (palette.LastQuery != palette.Query)
            {
                RefilterPalette();
            }
            const int count = static_cast<int>(palette.Filtered.size());
            if (count > 0)
            {
                palette.Selected = (palette.Selected % count + count) % count;
            }
            ImGui::Separator();

            const float rowHeight = ImGui::GetTextLineHeightWithSpacing() + 4.0f * scale;
            const int visibleRows = (std::min)((std::max)(count, 1), 14);
            ImGui::BeginChild("##PaletteResults", ImVec2(0.0f, visibleRows * rowHeight), ImGuiChildFlags_None);
            if (count == 0)
            {
                ImGui::TextDisabled("一致するコマンドがありません");
            }
            ImGuiListClipper clipper;
            clipper.Begin(count, rowHeight);
            if (palette.ScrollToSelected && count > 0)
            {
                clipper.IncludeItemByIndex(palette.Selected);
            }
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& command = palette.Commands[palette.Filtered[i]];
                    ImGui::PushID(i);
                    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                    ImGuiSelectableFlags selectableFlags = command.Enabled ? 0 : ImGuiSelectableFlags_Disabled;
                    if (ImGui::Selectable("##Command", i == palette.Selected, selectableFlags,
                                          ImVec2(0.0f, rowHeight - ImGui::GetStyle().ItemSpacing.y)))
                    {
                        RunPaletteCommand(i);
                    }
                    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x * ImGui::GetIO().MouseDelta.x +
                                                          ImGui::GetIO().MouseDelta.y * ImGui::GetIO().MouseDelta.y >
                                                      0.0f)
                    {
                        palette.Selected = i;
                    }
                    if (i == palette.Selected && palette.ScrollToSelected)
                    {
                        ImGui::SetScrollHereY(0.5f);
                    }
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    const float textY = rowStart.y + (rowHeight - ImGui::GetStyle().ItemSpacing.y -
                                                      ImGui::GetTextLineHeight()) * 0.5f;
                    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    const ImU32 text = command.Enabled ? ImGui::GetColorU32(ImGuiCol_Text) : dim;
                    const float categoryWidth = 92.0f * scale;
                    draw->AddText(ImVec2(rowStart.x + 6.0f * scale, textY), EditorPreferences::AccentU32(0.9f),
                                  command.Category.c_str());
                    draw->AddText(ImVec2(rowStart.x + categoryWidth, textY), text, command.Label.c_str());
                    if (!command.Shortcut.empty())
                    {
                        const float shortcutWidth = ImGui::CalcTextSize(command.Shortcut.c_str()).x;
                        draw->AddText(ImVec2(rowStart.x + ImGui::GetContentRegionAvail().x - shortcutWidth - 6.0f * scale,
                                             textY),
                                      dim, command.Shortcut.c_str());
                    }
                    ImGui::PopID();
                }
            }
            palette.ScrollToSelected = false;
            ImGui::EndChild();
            ImGui::TextDisabled("↑↓ 選択   Enter 実行   Esc 閉じる   %d 件", count);

            if (enter && count > 0)
            {
                RunPaletteCommand(palette.Selected);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                palette.Open = false;
            }
            if (!palette.JustOpened && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            {
                palette.Open = false;
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        palette.JustOpened = false;
    }

    void DrawShortcutHelp()
    {
        if (!g_ShowShortcuts)
        {
            return;
        }
        const float scale = Scale();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(760.0f * scale, 0.0f), ImGuiCond_Appearing);
        if (ImGui::Begin("ショートカット一覧##EditorShortcuts", &g_ShowShortcuts,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            struct Entry
            {
                const char* Keys;
                const char* Action;
            };
            const Entry general[] = {
                {"F3", "コマンド検索"},          {"F1", "このショートカット一覧"},
                {"Ctrl+,", "エディター設定"},    {"Ctrl+S / Ctrl+Shift+S", "保存 / 名前を付けて保存"},
                {"Ctrl+N", "新規シーン"},        {"Ctrl+Z / Ctrl+Y", "元に戻す / やり直し"},
                {"Ctrl+D", "複製"},              {"Delete", "削除"},
                {"F2", "名前の変更"},            {"Ctrl+P / Ctrl+Shift+P", "再生・停止 / 一時停止"},
                {"Ctrl+Space", "パネルを最大化 / 戻す"},
            };
            const Entry viewport3d[] = {
                {"Q / W / E / R", "ハンド / 移動 / 回転 / スケール"},
                {"X", "ワールド / ローカル"},
                {"Ctrl（ドラッグ中）", "スナップを一時的に反転"},
                {"Z（長押し / タップ）", "パイメニュー"},
                {"Shift+A", "追加メニュー"},
                {"F / Num .", "選択にフォーカス"},
                {"Num 1 / 3 / 7", "正面 / 右 / 上（Ctrl で反対側）"},
                {"右ドラッグ + WASD/QE", "フライ移動"},
                {"中ドラッグ", "パン"},
                {"Alt + 左ドラッグ", "オービット"},
                {"ナビギズモ", "軸クリックで視点切替 / ドラッグで回転"},
            };
            auto table = [](const char* id, const Entry* entries, int count) {
                if (ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
                {
                    for (int i = 0; i < count; ++i)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextColored(EditorPreferences::Accent(), "%s", entries[i].Keys);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(entries[i].Action);
                    }
                    ImGui::EndTable();
                }
            };
            if (ImGui::BeginTable("##ShortcutColumns", 2, ImGuiTableFlags_SizingStretchSame))
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::SeparatorText("全般");
                table("##General", general, IM_ARRAYSIZE(general));
                ImGui::TableNextColumn();
                ImGui::SeparatorText("3D ビューポート");
                table("##Viewport", viewport3d, IM_ARRAYSIZE(viewport3d));
                ImGui::EndTable();
            }
            if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                g_ShowShortcuts = false;
            }
        }
        ImGui::End();
    }

    void DrawToasts()
    {
        {
            std::lock_guard<std::mutex> lock(g_ToastMutex);
            while (!g_PendingToasts.empty())
            {
                g_ActiveToasts.push_back(std::move(g_PendingToasts.front()));
                g_PendingToasts.pop_front();
            }
            while (g_ActiveToasts.size() > 5)
            {
                g_ActiveToasts.pop_front();
            }
        }
        if (g_ActiveToasts.empty())
        {
            return;
        }
        const double now = ImGui::GetTime();
        const float scale = Scale();
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
        const float margin = 14.0f * scale;
        const float maxWidth = (std::min)(440.0f * scale, viewport->WorkSize.x * 0.5f);
        float y = viewport->WorkPos.y + viewport->WorkSize.y - margin;
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        std::lock_guard<std::mutex> lock(g_ToastMutex);
        for (auto it = g_ActiveToasts.rbegin(); it != g_ActiveToasts.rend(); ++it)
        {
            Toast& toast = *it;
            if (toast.Start < 0.0)
            {
                toast.Start = now;
            }
            const double duration = toast.Level == EditorShell::LogLevel::Error     ? 7.0
                                    : toast.Level == EditorShell::LogLevel::Warning ? 5.0
                                                                                    : 3.0;
            const double age = now - toast.Start;
            const float fadeIn = static_cast<float>((std::min)(age / 0.15, 1.0));
            const float fadeOut = static_cast<float>((std::clamp)((duration - age) / 0.4, 0.0, 1.0));
            const float alpha = fadeIn * fadeOut;
            const float wrap = maxWidth - 46.0f * scale;
            const ImVec2 textSize = ImGui::CalcTextSize(toast.Text.c_str(), nullptr, false, wrap);
            const float width = (std::min)(textSize.x + 46.0f * scale, maxWidth);
            const float height = textSize.y + 16.0f * scale;
            const float slide = (1.0f - fadeIn) * 24.0f * scale;
            const ImVec2 max(viewport->WorkPos.x + viewport->WorkSize.x - margin + slide, y);
            const ImVec2 min(max.x - width, y - height);
            if (mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y && age > 0.5)
            {
                toast.Start = now - 0.5; // ホバー中は消さない
            }
            const ImVec4 levelColor = EditorShell::LogLevelColor(toast.Level);
            ImVec4 background = EditorPreferences::PanelDark();
            background.w = 0.96f * alpha;
            draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 3.0f), ImVec2(max.x + 2.0f, max.y + 3.0f),
                                IM_COL32(0, 0, 0, static_cast<int>(90 * alpha)), 6.0f * scale);
            draw->AddRectFilled(min, max, ImGui::GetColorU32(background), 6.0f * scale);
            draw->AddRect(min, max, ImGui::GetColorU32(WithAlpha(levelColor, 0.55f * alpha)), 6.0f * scale);
            draw->AddRectFilled(min, ImVec2(min.x + 4.0f * scale, max.y), ImGui::GetColorU32(WithAlpha(levelColor, alpha)),
                                6.0f * scale, ImDrawFlags_RoundCornersLeft);
            draw->AddText(ImVec2(min.x + 12.0f * scale, min.y + 8.0f * scale),
                          ImGui::GetColorU32(WithAlpha(levelColor, alpha)), EditorShell::LogLevelIcon(toast.Level));
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + 32.0f * scale, min.y + 8.0f * scale),
                          ImGui::GetColorU32(WithAlpha(EditorPreferences::TextColor(), alpha)), toast.Text.c_str(),
                          nullptr, wrap);
            y = min.y - 8.0f * scale;
        }
        while (!g_ActiveToasts.empty())
        {
            const Toast& front = g_ActiveToasts.front();
            const double duration = front.Level == EditorShell::LogLevel::Error     ? 7.0
                                    : front.Level == EditorShell::LogLevel::Warning ? 5.0
                                                                                    : 3.0;
            if (front.Start >= 0.0 && now - front.Start > duration)
            {
                g_ActiveToasts.pop_front();
                continue;
            }
            break;
        }
    }

} // namespace

void EditorShell::SaveSceneAndReferences(bool saveAs)
{
    // Ctrl+S と同じ処理（シーン + マテリアル参照）
    ImGuiManager::SaveCurrentScene(saveAs);
    std::string error;
    std::error_code directoryError;
    std::filesystem::create_directories("Save", directoryError);
    if (!MaterialManager::SaveSceneReferences("Save/scene_materials.json", error))
    {
        ImGuiManager::AddLog("Scene save failed: %s", error.c_str());
    }
}

// ============================================================================ メニューバー右側 / ツールバー / ステータスバー
void EditorShell::DrawMainMenuRightSide()
{
    const float scale = Scale();
    const std::string scene = ImGuiManager::GetSceneDisplayName();
    const bool dirty = ImGuiManager::IsSceneDirty();
    const char* searchLabel = "⌕ 検索  F3";
    const float searchWidth = ImGui::CalcTextSize(searchLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float sceneWidth = ImGui::CalcTextSize(scene.c_str()).x + (dirty ? ImGui::CalcTextSize(" ●").x : 0.0f);
    const float total = sceneWidth + searchWidth + 18.0f * scale;
    const float x = ImGui::GetWindowWidth() - total - ImGui::GetStyle().WindowPadding.x;
    if (x <= ImGui::GetCursorPosX() + 8.0f)
    {
        return;
    }
    ImGui::SetCursorPosX(x);
    ImGui::TextDisabled("%s", scene.c_str());
    if (ImGui::IsItemHovered())
    {
        const auto& path = ImGuiManager::GetScenePath();
        ImGui::SetTooltip("%s", path.empty() ? "未保存のシーン（Ctrl+S で保存）" : path.generic_string().c_str());
    }
    if (dirty)
    {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(EditorPreferences::Warning(), " ●");
        ImGui::SetItemTooltip("未保存の変更があります");
    }
    ImGui::SameLine(0.0f, 14.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    if (ImGui::Button(searchLabel))
    {
        OpenCommandPalette();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void EditorShell::DrawToolbar()
{
    auto& prefs = EditorPreferences::Get();
    if (!prefs.ShowToolbar)
    {
        return;
    }
    const float scale = Scale();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float padding = 5.0f * scale;
    const float height = ImGui::GetFrameHeight() + padding * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, padding));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorPreferences::PanelDark());
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus;
    const bool open = ImGui::BeginViewportSideBar("##EditorMainToolbar", viewport, ImGuiDir_Up, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (open)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 4.0f * scale));
        // ---- 左：保存 / 履歴 / 追加
        if (ImGui::Button("保存"))
        {
            SaveSceneAndReferences(false);
        }
        ImGui::SetItemTooltip("シーンを保存 (Ctrl+S)");
        ImGui::SameLine();
        ImGui::BeginDisabled(ImGuiManager::m_UndoStack.empty() && !ImGuiManager::m_HasPendingUndo);
        if (ImGui::Button("↶"))
        {
            ImGuiManager::Undo();
        }
        ImGui::SetItemTooltip("元に戻す (Ctrl+Z)");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ImGuiManager::m_RedoStack.empty());
        if (ImGui::Button("↷"))
        {
            ImGuiManager::Redo();
        }
        ImGui::SetItemTooltip("やり直し (Ctrl+Y)");
        ImGui::EndDisabled();
        VerticalSeparator();
        if (ImGui::Button("✚ 追加 ▾"))
        {
            ImGui::OpenPopup("##ToolbarAddMenu");
        }
        ImGui::SetItemTooltip("オブジェクトを追加（ビューポート上では Shift+A）");
        if (ImGui::BeginPopup("##ToolbarAddMenu"))
        {
            DrawAddMenuItems();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("⌕ コマンド"))
        {
            OpenCommandPalette();
        }
        ImGui::SetItemTooltip("コマンド検索 (F3)");

        // ---- 中央：再生コントロール
        const bool playing = ProjectManager::IsPlaying();
        const bool paused = ProjectManager::IsPaused();
        const float buttonWidth = ImGui::GetFrameHeight() * 1.6f;
        const float playGroupWidth = buttonWidth * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        const float centerX = (ImGui::GetWindowWidth() - playGroupWidth) * 0.5f;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < centerX)
        {
            ImGui::SetCursorPosX(centerX);
        }
        {
            const ImVec4 green(0.20f, 0.62f, 0.32f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, playing && !paused ? ImVec4(1, 1, 1, 1) : green);
            if (ToggleButton("▶##ToolbarPlay", playing && !paused, playing ? "再開" : "再生 (Ctrl+P)",
                             ImVec2(buttonWidth, 0.0f)))
            {
                if (!playing)
                {
                    ProjectManager::Play();
                }
                else if (paused)
                {
                    ProjectManager::TogglePause();
                }
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::BeginDisabled(!playing);
            if (ToggleButton("❚❚##ToolbarPause", paused, "一時停止 (Ctrl+Shift+P)", ImVec2(buttonWidth, 0.0f)))
            {
                ProjectManager::TogglePause();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, playing ? EditorPreferences::Error() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            if (ImGui::Button("■##ToolbarStop", ImVec2(buttonWidth, 0.0f)))
            {
                ProjectManager::Stop();
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("停止 (Ctrl+P)");
            ImGui::EndDisabled();
        }

        // ---- 右：表示切替 / 設定
        const char* lightLabel = "☀ ライト";
        const char* prefsLabel = "⚙";
        const float rightWidth = ImGui::CalcTextSize(lightLabel).x + ImGui::CalcTextSize(prefsLabel).x +
                                 ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x;
        const float rightX = ImGui::GetWindowWidth() - rightWidth - 8.0f * scale;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < rightX)
        {
            ImGui::SetCursorPosX(rightX);
        }
        if (ToggleButton(lightLabel, ImGuiManager::m_ShowLightDebug, "ライトの可視化"))
        {
            ImGuiManager::m_ShowLightDebug = !ImGuiManager::m_ShowLightDebug;
        }
        ImGui::SameLine();
        if (ImGui::Button(prefsLabel))
        {
            EditorWorkspaceManager::Get().Registry().ShowEditorType("preferences");
        }
        ImGui::SetItemTooltip("エディター設定 (Ctrl+,)");
        ImGui::PopStyleVar();

        // 再生中は UE と同じくツールバーの下端をアクセント色で示す
        if (playing)
        {
            const ImVec2 min = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(min.x, min.y + size.y - 2.0f * scale), ImVec2(min.x + size.x, min.y + size.y),
                ImGui::GetColorU32(paused ? EditorPreferences::Warning() : EditorPreferences::Success()));
        }
    }
    ImGui::End();
}

void EditorShell::DrawStatusBar()
{
    auto& prefs = EditorPreferences::Get();
    if (!prefs.ShowStatusBar)
    {
        return;
    }
    const float scale = Scale();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetTextLineHeight() + 8.0f * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * scale, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorPreferences::PanelDark());
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus;
    const bool open = ImGui::BeginViewportSideBar("##EditorStatusBar", viewport, ImGuiDir_Down, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (open)
    {
        const ImVec4 dim = EditorPreferences::TextDim();
        // ---- 左：操作ヒント（Blender のステータスバーと同じく状況に応じて変わる）
        const char* hint;
        if (g_Pie.Open)
        {
            hint = "クリック / Z を離す: 決定    右クリック / Esc: キャンセル";
        }
        else if (ImGuizmo::IsUsingAny())
        {
            hint = "Ctrl: スナップを反転    Esc: キャンセル（Ctrl+Z）";
        }
        else if (ImGuiManager::m_IsSceneViewHovered)
        {
            hint = "左: 選択   右+WASD: 飛行   中: パン   Alt+左: 回転   Z: パイ   Shift+A: 追加   Num1/3/7: 視点";
        }
        else
        {
            hint = "F3: コマンド検索   F1: ショートカット   Ctrl+Space: パネル最大化   Ctrl+,: 設定";
        }
        ImGui::TextColored(dim, "%s", hint);

        // ---- 右：統計
        char stats[256];
        int length = 0;
        const size_t selected = ImGuiManager::m_SelectedEntities.size();
        length += snprintf(stats + length, sizeof(stats) - length, "選択 %zu", selected);
        if (GpuMemoryBudget::Stats.Valid && length < static_cast<int>(sizeof(stats)))
        {
            length += snprintf(stats + length, sizeof(stats) - length, "   VRAM %.2f / %.2f GB",
                               GpuMemoryBudget::Stats.Usage / (1024.0 * 1024.0 * 1024.0),
                               GpuMemoryBudget::Stats.Budget / (1024.0 * 1024.0 * 1024.0));
        }
        if (length < static_cast<int>(sizeof(stats)))
        {
            length += snprintf(stats + length, sizeof(stats) - length, "   GPU %.2f ms   %ux%u",
                               PerformanceGovernor::GetSmoothedGpuMs(), GraphicsDevice::GetSceneWidth(),
                               GraphicsDevice::GetSceneHeight());
        }
        const float fps = World::GetFrameRate();
        char fpsText[32];
        snprintf(fpsText, sizeof(fpsText), "%.0f FPS", fps);
        const float statsWidth = ImGui::CalcTextSize(stats).x;
        const float fpsWidth = ImGui::CalcTextSize(fpsText).x;
        const float rightStart = ImGui::GetWindowWidth() - statsWidth - fpsWidth - 24.0f * scale;

        // ---- 中央：最新ログ（クリックでコンソール）
        std::string lastLog;
        EditorShell::LogLevel lastLevel = EditorShell::LogLevel::Info;
        uint32_t lastRepeat = 1;
        {
            std::lock_guard<std::recursive_mutex> lock(ImGuiManager::m_LogMutex);
            if (!ImGuiManager::m_Logs.empty())
            {
                lastLog = ImGuiManager::m_Logs.back().Text;
                lastLevel = static_cast<EditorShell::LogLevel>(ImGuiManager::m_Logs.back().Level);
                lastRepeat = ImGuiManager::m_Logs.back().Repeat;
            }
        }
        ImGui::SameLine(0.0f, 24.0f * scale);
        const float logWidth = rightStart - ImGui::GetCursorPosX() - 12.0f * scale;
        if (!lastLog.empty() && logWidth > 80.0f * scale)
        {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##StatusLog", ImVec2(logWidth, ImGui::GetTextLineHeight())))
            {
                EditorWorkspaceManager::Get().Registry().ShowEditorType("console");
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::SetItemTooltip("%s\n(クリックでコンソールを開く)", lastLog.c_str());
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec4 color = EditorShell::LogLevelColor(lastLevel);
            draw->PushClipRect(start, ImVec2(start.x + logWidth, start.y + ImGui::GetTextLineHeight()), true);
            draw->AddText(start, ImGui::GetColorU32(color), EditorShell::LogLevelIcon(lastLevel));
            char repeatText[24] = "";
            if (lastRepeat > 1)
            {
                snprintf(repeatText, sizeof(repeatText), " (×%u)", lastRepeat);
            }
            const std::string line = lastLog + repeatText;
            draw->AddText(ImVec2(start.x + 16.0f * scale, start.y),
                          ImGui::GetColorU32(hovered ? EditorPreferences::TextColor()
                                                     : (lastLevel == EditorShell::LogLevel::Info ? dim : color)),
                          line.c_str());
            draw->PopClipRect();
        }

        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < rightStart)
        {
            ImGui::SetCursorPosX(rightStart);
        }
        ImGui::TextColored(dim, "%s", stats);
        ImGui::SameLine(0.0f, 12.0f * scale);
        const int target = World::GetTargetFrameRate();
        const ImVec4 fpsColor = fps >= target * 0.97f  ? EditorPreferences::Success()
                                : fps >= target * 0.75f ? EditorPreferences::Warning()
                                                        : EditorPreferences::Error();
        ImGui::TextColored(fpsColor, "%s", fpsText);
    }
    ImGui::End();
}

// ============================================================================ ビューポート
void EditorShell::DrawViewportHeader()
{
    auto& prefs = EditorPreferences::Get();
    const float scale = Scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * scale, 3.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f * scale, 4.0f * scale));

    // ---- 変形ツール（UE のビューポートツールバーと同じ並び）
    struct Tool
    {
        const char* Icon;
        int Operation;
        const char* Tooltip;
    };
    const Tool tools[] = {{"✋", 3, "ハンド (Q)"}, {"✥", 0, "移動 (W)"}, {"↻", 1, "回転 (E)"}, {"⤢", 2, "スケール (R)"}};
    const float toolWidth = ImGui::GetFrameHeight() * 1.3f;
    for (int i = 0; i < 4; ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine(0.0f, 1.0f);
        }
        ImGui::PushID(i);
        if (ToggleButton(tools[i].Icon, ImGuiManager::m_GizmoOperation == tools[i].Operation, tools[i].Tooltip,
                         ImVec2(toolWidth, 0.0f)))
        {
            ImGuiManager::m_GizmoOperation = tools[i].Operation;
        }
        ImGui::PopID();
    }
    const char* spaceLabel = ImGuiManager::m_GizmoLocalSpace ? "⊙ ローカル" : "⊕ ワールド";
    SameLineIfFits(ImGui::CalcTextSize(spaceLabel).x + 20.0f * scale);
    if (ImGui::Button(spaceLabel))
    {
        ImGuiManager::m_GizmoLocalSpace = !ImGuiManager::m_GizmoLocalSpace;
    }
    ImGui::SetItemTooltip("座標系の切り替え (X)\nスケールは常にローカル");

    // ---- 再生状態（左のツール群の直後）
    if (ProjectManager::IsPlaying())
    {
        const char* state = ProjectManager::IsPaused() ? "❚❚ 一時停止中" : "● 実行中";
        ImGui::SameLine(0.0f, 10.0f * scale);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ProjectManager::IsPaused() ? EditorPreferences::Warning() : EditorPreferences::Success(),
                           "%s", state);
    }

    // ---- 右端：スナップ / 表示 / 追加（UE と同じく右寄せ）
    //  幅は前フレームに測った値を使う（初回は概算）。収まらないときは次の行の右端へ折り返す
    static float rightGroupWidth = 360.0f;
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail < rightGroupWidth)
    {
        ImGui::NewLine();
    }
    const float startX = ImGui::GetCursorPosX() + (std::max)(0.0f, ImGui::GetContentRegionAvail().x - rightGroupWidth);
    ImGui::SetCursorPosX(startX);
    ImGui::BeginGroup();
    SnapControl("SnapT", "⊞", prefs.Snap.Translate, prefs.Snap.TranslateStep, kTranslateSteps,
                IM_ARRAYSIZE(kTranslateSteps), "%.2f", "移動スナップ（Ctrl で反転）");
    ImGui::SameLine();
    SnapControl("SnapR", "∠", prefs.Snap.Rotate, prefs.Snap.RotateStep, kRotateSteps, IM_ARRAYSIZE(kRotateSteps),
                "%.0f°", "回転スナップ（Ctrl で反転）");
    ImGui::SameLine();
    SnapControl("SnapS", "⇲", prefs.Snap.Scale, prefs.Snap.ScaleStep, kScaleSteps, IM_ARRAYSIZE(kScaleSteps), "%.2f",
                "スケールスナップ（Ctrl で反転）");
    ImGui::SameLine(0.0f, 8.0f * scale);
    if (ImGui::Button("表示 ▾"))
    {
        ImGui::OpenPopup("##ViewportViewMenu");
    }
    if (ImGui::BeginPopup("##ViewportViewMenu"))
    {
        DrawViewMenuItems();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("追加 ▾"))
    {
        ImGui::OpenPopup("##ViewportAddMenuButton");
    }
    ImGui::SetItemTooltip("Shift+A");
    if (ImGui::BeginPopup("##ViewportAddMenuButton"))
    {
        DrawAddMenuItems();
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
    rightGroupWidth = ImGui::GetItemRectSize().x;
    ImGui::PopStyleVar(2);
}

const float* EditorShell::GizmoSnap(int gizmoOperation)
{
    const auto& snap = EditorPreferences::Get().Snap;
    const bool invert = ImGui::GetIO().KeyCtrl; // Blender と同じく Ctrl で一時的に反転
    switch (gizmoOperation)
    {
        case 0:
            if (snap.Translate != invert)
            {
                g_SnapValues[0] = g_SnapValues[1] = g_SnapValues[2] = snap.TranslateStep;
                return g_SnapValues;
            }
            break;
        case 1:
            if (snap.Rotate != invert)
            {
                g_SnapValues[0] = g_SnapValues[1] = g_SnapValues[2] = snap.RotateStep;
                return g_SnapValues;
            }
            break;
        case 2:
            if (snap.Scale != invert)
            {
                g_SnapValues[0] = g_SnapValues[1] = g_SnapValues[2] = snap.ScaleStep;
                return g_SnapValues;
            }
            break;
        default:
            break;
    }
    return nullptr;
}

void EditorShell::SetViewDirection(float forwardX, float forwardY, float forwardZ)
{
    EntityID cameraEntity;
    if (!EditorCamera(cameraEntity))
    {
        return;
    }
    const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
    const float length = sqrtf(forwardX * forwardX + forwardY * forwardY + forwardZ * forwardZ);
    if (length < 1e-5f)
    {
        return;
    }
    forwardX /= length;
    forwardY /= length;
    forwardZ /= length;
    auto& animation = g_ViewAnimation;
    animation.Distance = (std::max)(EditorSceneNavigation::PivotDistance(), 0.5f);
    const XMVECTOR eye = XMLoadFloat3(&transform.Position);
    XMStoreFloat3(&animation.Pivot,
                  XMVectorAdd(eye, XMVectorScale(EditorSceneNavigation::ForwardOf(transform), animation.Distance)));
    animation.FromPitch = transform.Rotation.x;
    animation.FromYaw = transform.Rotation.y;
    // forward = (cos p * sin y, -sin p, cos p * cos y)（EditorSceneNavigation::ForwardOf と同じ規約）
    animation.ToPitch = (std::clamp)(asinf((std::clamp)(-forwardY, -1.0f, 1.0f)), -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
    animation.ToYaw = (fabsf(forwardX) + fabsf(forwardZ) < 1e-4f) ? transform.Rotation.y : atan2f(forwardX, forwardZ);
    while (animation.ToYaw - animation.FromYaw > XM_PI)
    {
        animation.ToYaw -= XM_2PI;
    }
    while (animation.ToYaw - animation.FromYaw < -XM_PI)
    {
        animation.ToYaw += XM_2PI;
    }
    animation.Time = 0.0f;
    animation.Active = true;
    EditorSceneNavigation::Focus().Active = false;
}

bool EditorShell::DrawNavigationGizmo(ImDrawList* drawList, ImVec2 imageMin, ImVec2 imageSize)
{
    const float scale = Scale();
    const float radius = 44.0f * scale;
    EntityID cameraEntity;
    if (!EditorPreferences::Get().ShowNavigationGizmo || !drawList || imageSize.x < radius * 4.0f ||
        imageSize.y < radius * 3.0f || !EditorCamera(cameraEntity))
    {
        g_NavPressedAxis = -1;
        return false;
    }
    const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity);
    const ImVec2 center(imageMin.x + imageSize.x - radius - 10.0f * scale, imageMin.y + radius + 10.0f * scale);

    XMFLOAT4X4 basis;
    XMStoreFloat4x4(&basis, XMMatrixRotationRollPitchYaw(transform.Rotation.x, transform.Rotation.y, 0.0f));
    const float right[3] = {basis._11, basis._12, basis._13};
    const float up[3] = {basis._21, basis._22, basis._23};
    const float forward[3] = {basis._31, basis._32, basis._33};

    struct AxisItem
    {
        int Axis;
        float Sign;
        ImVec2 Position;
        float Depth;
    };
    AxisItem items[6];
    for (int axis = 0; axis < 3; ++axis)
    {
        for (int s = 0; s < 2; ++s)
        {
            const float sign = s == 0 ? 1.0f : -1.0f;
            AxisItem& item = items[axis * 2 + s];
            item.Axis = axis;
            item.Sign = sign;
            item.Position = ImVec2(center.x + sign * right[axis] * radius * 0.72f,
                                   center.y - sign * up[axis] * radius * 0.72f);
            item.Depth = sign * forward[axis];
        }
    }

    // InvisibleButton でマウスを確保する（ウィンドウ移動・ピッキング・カメラ操作と競合させない）
    // 注意：InvisibleButton の後でカーソルを戻すと、アイテムなしでカーソルを動かしたことになり
    // ImGui 1.92 の「SetCursorPos()/SetCursorScreenPos() to extend window boundaries」エラーになる。
    // 呼び出し側（シーンビュー）は以降カーソルを使わないので、元に戻さない。
    // 修正：ボタンは常に置かず、カーソルが円の中にあるとき（またはドラッグ中）だけ置く。
    // 以前は常に四角いボタンを置いていたため、その範囲（とホバー履歴）が ImGuizmo の
    // 「他のアイテムがホバーされていない」判定を妨げ、右上付近でギズモを掴めないことがあった。
    static bool navActiveLastFrame = false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float dx = mouse.x - center.x;
    const float dy = mouse.y - center.y;
    const bool inDisk = dx * dx + dy * dy <= radius * radius;
    bool hovered = false;
    bool active = false;
    bool activated = false;
    bool deactivated = false;
    if ((inDisk && !ImGuizmo::IsUsingAny()) || navActiveLastFrame)
    {
        ImGui::SetCursorScreenPos(ImVec2(center.x - radius, center.y - radius));
        ImGui::InvisibleButton("##NavigationGizmo", ImVec2(radius * 2.0f, radius * 2.0f));
        hovered = ImGui::IsItemHovered() && inDisk;
        active = ImGui::IsItemActive();
        activated = ImGui::IsItemActivated();
        deactivated = ImGui::IsItemDeactivated();
    }
    navActiveLastFrame = active;

    const float bubble = 9.0f * scale;
    int hoveredItem = -1;
    // 修正：以前は !active の条件があり、押した瞬間（activated のフレームは active も true）に
    // hoveredItem が常に -1 になって g_NavPressedAxis が設定されず、クリックで視点が切り替わらなかった。
    // ドラッグ中（オービット）はハイライトしない
    if (hovered && !(active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)))
    {
        float bestDepth = 1e9f;
        for (int i = 0; i < 6; ++i)
        {
            const float ix = mouse.x - items[i].Position.x;
            const float iy = mouse.y - items[i].Position.y;
            if (ix * ix + iy * iy <= bubble * bubble && items[i].Depth < bestDepth)
            {
                bestDepth = items[i].Depth;
                hoveredItem = i;
            }
        }
    }
    if (activated)
    {
        g_NavPressedAxis = hoveredItem;
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
    {
        g_NavPressedAxis = -1;
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        if (delta.x != 0.0f || delta.y != 0.0f)
        {
            OrbitEditorCamera(delta.x * 0.008f, delta.y * 0.008f);
        }
    }
    if (deactivated && g_NavPressedAxis >= 0 && inDisk)
    {
        const AxisItem& item = items[g_NavPressedAxis];
        float direction[3] = {0.0f, 0.0f, 0.0f};
        direction[item.Axis] = -item.Sign; // 軸の側から中心を見る
        SetViewDirection(direction[0], direction[1], direction[2]);
        g_NavPressedAxis = -1;
    }

    if (hovered || active)
    {
        drawList->AddCircleFilled(center, radius, IM_COL32(255, 255, 255, active ? 36 : 22), 48);
    }
    int order[6] = {0, 1, 2, 3, 4, 5};
    std::sort(std::begin(order), std::end(order), [&](int a, int b) { return items[a].Depth > items[b].Depth; });
    static const char* kLabels[] = {"X", "Y", "Z"};
    for (int index : order)
    {
        const AxisItem& item = items[index];
        const bool positive = item.Sign > 0.0f;
        const float facing = 0.65f + 0.35f * (1.0f - (item.Depth + 1.0f) * 0.5f); // 手前ほど明るい
        const ImU32 color = EditorPreferences::AxisColor(item.Axis, facing);
        if (positive)
        {
            drawList->AddLine(center, item.Position, color, 2.0f * scale);
            drawList->AddCircleFilled(item.Position, bubble, color, 20);
            const ImVec2 labelSize = ImGui::CalcTextSize(kLabels[item.Axis]);
            drawList->AddText(ImVec2(item.Position.x - labelSize.x * 0.5f, item.Position.y - labelSize.y * 0.5f),
                              IM_COL32(20, 20, 20, 255), kLabels[item.Axis]);
        }
        else
        {
            drawList->AddCircleFilled(item.Position, bubble * 0.8f, EditorPreferences::AxisColor(item.Axis, 0.30f), 20);
            drawList->AddCircle(item.Position, bubble * 0.8f, color, 20, 1.5f * scale);
        }
        if (index == hoveredItem)
        {
            drawList->AddCircle(item.Position, bubble + 2.0f * scale, IM_COL32(255, 255, 255, 230), 20, 2.0f * scale);
        }
    }
    if (hoveredItem >= 0)
    {
        static const char* kViewNames[3][2] = {{"右", "左"}, {"上", "下"}, {"背面", "正面"}};
        const AxisItem& item = items[hoveredItem];
        ImGui::SetTooltip("%s から見る", kViewNames[item.Axis][item.Sign > 0.0f ? 0 : 1]);
    }
    return hovered || active;
}

bool EditorShell::IsPieOpen()
{
    return g_Pie.Open;
}

void EditorShell::UpdateViewportInteractions()
{
    UpdateViewAnimation();

    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGuiManager::m_IsSceneViewHovered;
    const bool canUseKeys = hovered && !io.WantTextInput && !ImGui::IsAnyItemActive() &&
                            !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGuizmo::IsUsingAny() && !g_Pie.Open;
    if (canUseKeys)
    {
        const bool editing = !ProjectManager::IsPlaying();
        if (editing && io.KeyShift && !io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_A, false))
        {
            g_OpenViewportAddMenu = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Keypad1, false))
        {
            SetViewDirection(0.0f, 0.0f, io.KeyCtrl ? -1.0f : 1.0f);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Keypad3, false))
        {
            SetViewDirection(io.KeyCtrl ? 1.0f : -1.0f, 0.0f, 0.0f);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Keypad7, false))
        {
            SetViewDirection(0.0f, io.KeyCtrl ? 1.0f : -1.0f, 0.0f);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false))
        {
            EditorSceneNavigation::FocusOn(ImGuiManager::m_SelectedEntity);
        }
        if (editing && !io.KeyShift && !io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        {
            g_Pie = {};
            g_Pie.Open = true;
            g_Pie.Center = io.MousePos;
            g_Pie.OpenTime = ImGui::GetTime();
        }
    }

    if (!g_Pie.Open)
    {
        return;
    }

    // ---- パイメニュー（Blender 風：長押しで選んで離す / タップで開いたままクリック）
    // パイ表示中はシーンのピッキング・カメラ操作・ギズモを止める
    ImGuiManager::m_IsSceneViewHovered = false;
    ImGuiManager::m_SceneGizmoCapturesMouse = true;

    struct PieItem
    {
        const char* Icon;
        const char* Label;
        bool Active;
    };
    const auto& snap = EditorPreferences::Get().Snap;
    const PieItem items[8] = {
        {"↻", "回転", ImGuiManager::m_GizmoOperation == 1},                                          // 右
        {"☀", "ライト可視化", ImGuiManager::m_ShowLightDebug},                                       // 右下
        {"⤢", "スケール", ImGuiManager::m_GizmoOperation == 2},                                      // 下
        {"◎", "フォーカス", false},                                                                    // 左下
        {"✋", "ハンド", ImGuiManager::m_GizmoOperation == 3},                                        // 左
        {ImGuiManager::m_GizmoLocalSpace ? "⊙" : "⊕", ImGuiManager::m_GizmoLocalSpace ? "ローカル" : "ワールド",
         false},                                                                                       // 左上
        {"✥", "移動", ImGuiManager::m_GizmoOperation == 0},                                          // 上
        {"⊞", "スナップ", snap.Translate || snap.Rotate || snap.Scale},                                // 右上
    };

    const float scale = Scale();
    const float deadZone = 22.0f * scale;
    const float itemRadius = 112.0f * scale;
    const ImVec2 delta(io.MousePos.x - g_Pie.Center.x, io.MousePos.y - g_Pie.Center.y);
    const float distance = sqrtf(delta.x * delta.x + delta.y * delta.y);
    g_Pie.Hovered = -1;
    if (distance > deadZone)
    {
        float angle = atan2f(delta.y, delta.x); // 0 = 右、時計回り（画面座標）
        if (angle < 0.0f)
        {
            angle += XM_2PI;
        }
        g_Pie.Hovered = static_cast<int>(floorf(angle / (XM_2PI / 8.0f) + 0.5f)) % 8;
    }

    bool confirm = false;
    bool cancel = false;
    if (ImGui::IsKeyReleased(ImGuiKey_Z))
    {
        if (g_Pie.Hovered >= 0)
        {
            confirm = true;
        }
        else if (ImGui::GetTime() - g_Pie.OpenTime < 0.35)
        {
            g_Pie.Sticky = true;
        }
        else
        {
            cancel = true;
        }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        (g_Pie.Hovered >= 0 ? confirm : cancel) = true;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        cancel = true;
    }

    // ---- 描画
    ImDrawList* draw = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const ImU32 ring = ImGui::GetColorU32(WithAlpha(EditorPreferences::PanelDark(), 0.9f));
    draw->AddCircleFilled(g_Pie.Center, deadZone, ring, 32);
    draw->AddCircle(g_Pie.Center, deadZone, ImGui::GetColorU32(ImGuiCol_Border), 32, 2.0f * scale);
    if (g_Pie.Hovered >= 0)
    {
        const float angle = g_Pie.Hovered * (XM_2PI / 8.0f);
        draw->PathArcTo(g_Pie.Center, deadZone - 2.0f * scale, angle - 0.35f, angle + 0.35f, 12);
        draw->PathStroke(EditorPreferences::AccentU32(), 0, 4.0f * scale);
    }
    for (int i = 0; i < 8; ++i)
    {
        const float angle = i * (XM_2PI / 8.0f);
        const ImVec2 anchor(g_Pie.Center.x + cosf(angle) * itemRadius, g_Pie.Center.y + sinf(angle) * itemRadius);
        char label[64];
        snprintf(label, sizeof(label), "%s  %s", items[i].Icon, items[i].Label);
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const ImVec2 padding(10.0f * scale, 6.0f * scale);
        const ImVec2 min(anchor.x - textSize.x * 0.5f - padding.x, anchor.y - textSize.y * 0.5f - padding.y);
        const ImVec2 max(anchor.x + textSize.x * 0.5f + padding.x, anchor.y + textSize.y * 0.5f + padding.y);
        const bool hot = i == g_Pie.Hovered;
        ImVec4 background = hot ? EditorPreferences::Accent() : EditorPreferences::PanelDark();
        background.w = hot ? 1.0f : 0.94f;
        draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 3.0f), ImVec2(max.x + 2.0f, max.y + 3.0f),
                            IM_COL32(0, 0, 0, 80), 12.0f * scale);
        draw->AddRectFilled(min, max, ImGui::GetColorU32(background), 12.0f * scale);
        if (items[i].Active && !hot)
        {
            draw->AddRect(min, max, EditorPreferences::AccentU32(), 12.0f * scale, 0, 1.5f * scale);
        }
        draw->AddText(ImVec2(min.x + padding.x, min.y + padding.y),
                      hot ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(ImGuiCol_Text), label);
    }

    if (confirm && g_Pie.Hovered >= 0)
    {
        switch (g_Pie.Hovered)
        {
            case 0:
                ImGuiManager::m_GizmoOperation = 1;
                break;
            case 1:
                ImGuiManager::m_ShowLightDebug = !ImGuiManager::m_ShowLightDebug;
                break;
            case 2:
                ImGuiManager::m_GizmoOperation = 2;
                break;
            case 3:
                EditorSceneNavigation::FocusOn(ImGuiManager::m_SelectedEntity);
                break;
            case 4:
                ImGuiManager::m_GizmoOperation = 3;
                break;
            case 5:
                ImGuiManager::m_GizmoLocalSpace = !ImGuiManager::m_GizmoLocalSpace;
                break;
            case 6:
                ImGuiManager::m_GizmoOperation = 0;
                break;
            case 7: {
                auto& snapSettings = EditorPreferences::Get().Snap;
                const bool enable = !(snapSettings.Translate || snapSettings.Rotate || snapSettings.Scale);
                snapSettings.Translate = snapSettings.Rotate = snapSettings.Scale = enable;
                break;
            }
            default:
                break;
        }
    }
    if (confirm || cancel)
    {
        g_Pie = {};
    }
}

void EditorShell::DrawOverlays()
{
    DrawCommandPalette();

    if (g_OpenViewportAddMenu)
    {
        ImGui::OpenPopup("##ViewportAddMenu");
        g_OpenViewportAddMenu = false;
    }
    if (ImGui::BeginPopup("##ViewportAddMenu"))
    {
        ImGui::TextDisabled("追加");
        ImGui::Separator();
        DrawAddMenuItems();
        ImGui::EndPopup();
    }

    DrawShortcutHelp();
    DrawToasts();

    if (g_DeferredCommand)
    {
        auto command = std::move(g_DeferredCommand);
        g_DeferredCommand = nullptr;
        command();
    }
}

// ============================================================================ Undo History / 設定
void EditorShell::DrawHistoryWindow()
{
    ImGui::SetNextWindowSize(ImVec2(320.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!EditorWindowRegistry::BeginEditorWindow("Undo History", &ImGuiManager::m_ShowHistoryWindow))
    {
        ImGui::End();
        return;
    }
    auto label = [](const std::vector<ImGuiManager::EntitySnapshot>& group) {
        if (group.empty())
        {
            return std::string("（空）");
        }
        const auto& first = group.front();
        std::string name;
        if (first.HasName)
        {
            name = first.Name.Name;
        }
        else if (Registry::IsAlive(first.Entity))
        {
            name = ImGuiManager::GetEntityDisplayName(first.Entity);
        }
        else
        {
            name = "#" + std::to_string(first.Entity);
        }
        std::string result = (first.WasAlive ? "編集  " : "作成  ") + name;
        if (group.size() > 1)
        {
            result += "  ほか " + std::to_string(group.size() - 1) + " 件";
        }
        return result;
    };

    auto& undo = ImGuiManager::m_UndoStack;
    auto& redo = ImGuiManager::m_RedoStack;
    ImGui::BeginDisabled(undo.empty());
    if (ImGui::Button("↶ 元に戻す"))
    {
        ImGuiManager::Undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redo.empty());
    if (ImGui::Button("↷ やり直し"))
    {
        ImGuiManager::Redo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %d", undo.size(), EditorPreferences::Get().UndoLimit);
    ImGui::Separator();

    int undoSteps = 0;
    int redoSteps = 0;
    ImGui::BeginChild("##HistoryList");
    if (ImGui::Selectable("◇  初期状態（保持している最古）", undo.empty()))
    {
        undoSteps = static_cast<int>(undo.size());
    }
    for (size_t i = 0; i < undo.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        const bool current = i + 1 == undo.size();
        const std::string text = std::to_string(i + 1) + ".  " + label(undo[i]);
        if (current)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorPreferences::AccentHovered());
        }
        if (ImGui::Selectable(text.c_str(), current))
        {
            undoSteps = static_cast<int>(undo.size() - 1 - i);
        }
        if (current)
        {
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    if (!redo.empty())
    {
        ImGui::SeparatorText("やり直し可能");
        for (size_t d = 0; d < redo.size(); ++d)
        {
            const size_t index = redo.size() - 1 - d;
            ImGui::PushID(static_cast<int>(10000 + d));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const std::string text = "↷  " + label(redo[index]);
            if (ImGui::Selectable(text.c_str(), false))
            {
                redoSteps = static_cast<int>(d + 1);
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();

    for (int i = 0; i < undoSteps; ++i)
    {
        ImGuiManager::Undo();
    }
    for (int i = 0; i < redoSteps; ++i)
    {
        ImGuiManager::Redo();
    }
}

void EditorShell::DrawPreferencesWindow()
{
    ImGui::SetNextWindowSize(ImVec2(520.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!EditorWindowRegistry::BeginEditorWindow("エディター設定", &ImGuiManager::m_ShowPreferencesWindow))
    {
        ImGui::End();
        return;
    }
    EditorPreferences::DrawPreferencesContent();
    ImGui::End();
}

