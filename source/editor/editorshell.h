#pragma once
// ============================================================================
// EditorShell：Unreal Engine 5 + Blender 風のエディター外枠
//  ・メインツールバー（保存 / 元に戻す / 追加 / 変形ツール / スナップ / 再生）
//  ・ステータスバー（操作ヒント / 最新ログ / シーン / 選択数 / VRAM / GPU / FPS）
//  ・コマンド検索（F3）、ショートカット一覧（F1）、通知トースト
//  ・ビューポート：ヘッダー、ナビゲーションギズモ、テンキー視点、Shift+A 追加、Z パイメニュー
//  ・Undo History / エディター設定ウィンドウ
// ============================================================================
#include <imgui.h>
#include <string>
#include <cstdint>

class EditorShell
{
  public:
    enum class LogLevel : uint8_t
    {
        Info = 0,
        Success = 1,
        Warning = 2,
        Error = 3,
    };

    EditorShell() = delete;

    // ---- ログ / 通知（AddLog から呼ばれる。スレッドセーフ）
    static LogLevel ClassifyLog(const char* text);
    static void PushToast(LogLevel level, const char* text);
    static ImVec4 LogLevelColor(LogLevel level);
    static const char* LogLevelIcon(LogLevel level);

    // ---- フレーム処理（ImGuiManager::Update から呼ぶ）
    static void HandleGlobalShortcuts();
    static void DrawMainMenuRightSide();
    static void DrawToolbar();
    static void DrawStatusBar();
    // ウィンドウ描画の後・ピッキングの前に呼ぶ（パイメニュー / Shift+A / テンキー視点 / 視点アニメーション）
    static void UpdateViewportInteractions();
    // 最前面のオーバーレイ（コマンド検索 / トースト / ショートカット一覧 / 追加メニュー）
    static void DrawOverlays();

    // ---- 共通メニュー
    static void DrawAddMenuItems();
    static void DrawViewMenuItems();
    static void OpenCommandPalette();
    static void ToggleShortcutHelp();

    // ---- ビューポート
    static void DrawViewportHeader();
    // 戻り値：マウスをギズモが使用しているか（シーンのピッキング / カメラ操作を止める）
    static bool DrawNavigationGizmo(ImDrawList* drawList, ImVec2 imageMin, ImVec2 imageSize);
    static bool IsPieOpen();
    // ImGuizmo::Manipulate に渡すスナップ値。スナップ無効なら nullptr
    static const float* GizmoSnap(int gizmoOperation);
    static void SetViewDirection(float forwardX, float forwardY, float forwardZ);

    // ---- 追加ウィンドウ
    static void DrawHistoryWindow();
    static void DrawPreferencesWindow();

  private:
    static void SaveSceneAndReferences(bool saveAs);
};
