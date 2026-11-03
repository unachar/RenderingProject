#pragma once
// ============================================================================
// エディター設定（テーマ / フォント / UI スケール / 表示オプション / スナップ）
//  ・Unreal Engine 5 / Blender / Unity 風のテーマプリセットとアクセントカラー
//  ・Save/editor_preferences.cfg（key=value のテキスト）に保存
//  ・スタイル変更は ApplyPending() で NewFrame の前にまとめて反映する
// ============================================================================
#include <imgui.h>
#include <string>
#include <vector>

namespace EditorPreferences
{
    enum class ThemePreset : int
    {
        Unreal = 0,
        Blender,
        Unity,
        Midnight,
        Light, // #14 ライトテーマ
        Count
    };

    // #2 ビューポートの表示モード（Lit 以外は G-Buffer をそのまま表示する）
    enum class ViewMode : int
    {
        Lit = 0,
        Unlit,    // ベースカラー
        Normal,
        Depth,
        Material, // メタリック / ラフネス / フレネル
        Emission,
        AO,
        Velocity,
        Count
    };

    // #8 コンテンツブラウザの種類フィルター
    enum class AssetFilter : int
    {
        All = 0,
        Folder,
        Material,
        Texture,
        Mesh,
        Scene,
        Other,
        Count
    };

    enum class FontChoice : int
    {
        Auto = 0, // Yu Gothic → Meiryo → MS Gothic
        YuGothic,
        Meiryo,
        MsGothic,
        Count
    };

    struct SnapSettings
    {
        bool Translate = false;
        bool Rotate = false;
        bool Scale = false;
        float TranslateStep = 0.5f; // m
        float RotateStep = 15.0f;   // 度
        float ScaleStep = 0.1f;
    };

    struct Settings
    {
        ThemePreset Theme = ThemePreset::Unreal;
        bool CustomAccent = false;
        ImVec4 Accent{0.000f, 0.439f, 0.878f, 1.0f};
        float UiScale = 1.0f;
        float Rounding = 3.0f;
        bool Compact = false;
        FontChoice Font = FontChoice::Auto;
        float FontSize = 16.0f;

        bool ShowToolbar = true;
        bool ShowStatusBar = true;
        bool ShowAreaHeaders = false; // #1 既定はオフ（タブの右クリックで切り替え / 最大化）
        bool ShowToasts = true;
        bool ToastOnSuccess = true;
        bool ShowNavigationGizmo = true;
        bool ShowViewportStats = false; // #4 既定はオフ（FPS / GPU / 品質 / 解像度はステータスバーに表示）
        bool ShowOutlinerTypeColumn = true;
        bool ShowViewportAxis = true;     // #3 左下の XYZ 軸
        bool ShowViewportSpeed = true;    // #3 カメラ速度ボタン
        ViewMode ViewportViewMode = ViewMode::Lit; // #2
        float EditorCameraSpeed = 1.0f;   // #3 フライ速度の倍率（右ドラッグ + ホイールでも変更）

        // ---- コンテンツブラウザ（#7〜#9）
        bool AssetFolderTree = true;
        float AssetTreeWidth = 200.0f;
        bool AssetListView = false;
        AssetFilter AssetTypeFilter = AssetFilter::All;
        bool AssetHideCompanions = true; // .graph.json などの付属ファイルを隠す
        std::string AssetFavorites;      // '|' 区切り（asset からの相対パス）
        std::string AssetRecent;         // '|' 区切り、新しい順

        int UndoLimit = 30;
        SnapSettings Snap;
    };

    Settings& Get();
    const char* ThemeName(ThemePreset preset);
    const char* ViewModeName(ViewMode mode);
    const char* AssetFilterName(AssetFilter filter);
    // '|' 区切りリストの操作（お気に入り / 最近使ったもの）
    std::vector<std::string> SplitList(const std::string& list);
    std::string JoinList(const std::vector<std::string>& items);
    const char* FontName(FontChoice font);

    // Init（バックエンド初期化前）に一度だけ呼ぶ
    void Load();
    void LoadFonts();
    // 変更を予約し、次の ApplyPending（NewFrame 前）でスタイルを再構築する
    void MarkStyleDirty();
    void ApplyPending();
    void ApplyNow();
    // 2 秒ごと程度で呼ぶ。内容が変わったときだけ書き込む
    void SaveIfChanged();
    void Save();

    // セマンティックカラー
    ImVec4 Accent();
    ImU32 AccentU32(float alpha = 1.0f);
    ImVec4 AccentHovered();
    ImVec4 AccentActive();
    ImVec4 TextColor();
    ImVec4 TextDim();
    ImVec4 PanelDark();
    ImVec4 Success();
    ImVec4 Warning();
    ImVec4 Error();
    ImU32 AxisColor(int axis, float alpha = 1.0f); // 0:X(赤) 1:Y(緑) 2:Z(青)
    // #14 これまで各所に直書きしていた色をここに集約
    bool IsLightTheme();
    ImVec4 OverlayBg();      // ビューポート上に重ねる UI の背景（半透明）
    ImVec4 OverlayText();
    ImVec4 Selection();      // 選択中の強調色
    ImVec4 PrefabColor();
    ImVec4 Favorite();

    // 設定ウィンドウの中身（BeginEditorWindow の内側から呼ぶ）
    void DrawPreferencesContent();
} // namespace EditorPreferences

