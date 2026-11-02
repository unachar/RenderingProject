#include "pch.h"
#include "editorpreferences.h"
#include <imgui_internal.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace EditorPreferences
{
    namespace
    {
        constexpr const char* kPreferencesPath = "Save/editor_preferences.cfg";

        Settings g_Settings;
        bool g_StyleDirty = true;
        std::string g_LastSaved;

        struct Palette
        {
            ImVec4 Menu;        // メニューバー / タイトル / 非選択タブ
            ImVec4 Window;      // パネル背景
            ImVec4 Child;       // 子領域
            ImVec4 Popup;       // ポップアップ
            ImVec4 Frame;       // 入力欄
            ImVec4 FrameHover;  // 入力欄（ホバー）
            ImVec4 Button;      // ボタン
            ImVec4 ButtonHover; // ボタン（ホバー）
            ImVec4 RowHover;    // 行のホバー
            ImVec4 Border;
            ImVec4 Text;
            ImVec4 TextDim;
            ImVec4 Accent;
            ImVec4 DockEmpty;
            float Rounding = 3.0f;
        };

        ImVec4 Rgb(int r, int g, int b, float a = 1.0f)
        {
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
        }

        ImVec4 Lerp(const ImVec4& a, const ImVec4& b, float t)
        {
            return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
        }

        ImVec4 WithAlpha(ImVec4 color, float alpha)
        {
            color.w = alpha;
            return color;
        }

        Palette MakePalette(ThemePreset preset)
        {
            Palette p;
            switch (preset)
            {
                case ThemePreset::Blender:
                    p.Menu = Rgb(35, 35, 35);
                    p.Window = Rgb(48, 48, 48);
                    p.Child = Rgb(42, 42, 42);
                    p.Popup = Rgb(24, 24, 24, 0.98f);
                    p.Frame = Rgb(72, 72, 72);
                    p.FrameHover = Rgb(88, 88, 88);
                    p.Button = Rgb(84, 84, 84);
                    p.ButtonHover = Rgb(101, 101, 101);
                    p.RowHover = Rgb(64, 64, 64);
                    p.Border = Rgb(26, 26, 26);
                    p.Text = Rgb(230, 230, 230);
                    p.TextDim = Rgb(140, 140, 140);
                    p.Accent = Rgb(71, 114, 179);
                    p.DockEmpty = Rgb(29, 29, 29);
                    p.Rounding = 4.0f;
                    break;
                case ThemePreset::Unity:
                    p.Menu = Rgb(25, 25, 25);
                    p.Window = Rgb(56, 56, 56);
                    p.Child = Rgb(56, 56, 56);
                    p.Popup = Rgb(51, 51, 51, 0.98f);
                    p.Frame = Rgb(42, 42, 42);
                    p.FrameHover = Rgb(51, 51, 51);
                    p.Button = Rgb(88, 88, 88);
                    p.ButtonHover = Rgb(103, 103, 103);
                    p.RowHover = Rgb(69, 69, 69);
                    p.Border = Rgb(25, 25, 25);
                    p.Text = Rgb(210, 210, 210);
                    p.TextDim = Rgb(128, 128, 128);
                    p.Accent = Rgb(60, 136, 206);
                    p.DockEmpty = Rgb(40, 40, 40);
                    p.Rounding = 2.0f;
                    break;
                case ThemePreset::Midnight:
                    p.Menu = ImVec4(0.050f, 0.055f, 0.064f, 1.0f);
                    p.Window = ImVec4(0.075f, 0.080f, 0.090f, 1.0f);
                    p.Child = ImVec4(0.060f, 0.065f, 0.074f, 1.0f);
                    p.Popup = ImVec4(0.080f, 0.086f, 0.098f, 0.98f);
                    p.Frame = ImVec4(0.115f, 0.125f, 0.142f, 1.0f);
                    p.FrameHover = ImVec4(0.155f, 0.175f, 0.205f, 1.0f);
                    p.Button = ImVec4(0.13f, 0.16f, 0.19f, 1.0f);
                    p.ButtonHover = ImVec4(0.18f, 0.23f, 0.27f, 1.0f);
                    p.RowHover = ImVec4(0.14f, 0.18f, 0.22f, 1.0f);
                    p.Border = ImVec4(0.20f, 0.23f, 0.27f, 0.72f);
                    p.Text = ImVec4(0.86f, 0.89f, 0.92f, 1.0f);
                    p.TextDim = ImVec4(0.55f, 0.60f, 0.66f, 1.0f);
                    p.Accent = ImVec4(0.18f, 0.68f, 0.82f, 1.0f);
                    p.DockEmpty = ImVec4(0.045f, 0.050f, 0.058f, 1.0f);
                    p.Rounding = 3.0f;
                    break;
                case ThemePreset::Unreal:
                default:
                    p.Menu = Rgb(21, 21, 21);
                    p.Window = Rgb(36, 36, 36);
                    p.Child = Rgb(30, 30, 30);
                    p.Popup = Rgb(26, 26, 26, 0.98f);
                    p.Frame = Rgb(15, 15, 15);
                    p.FrameHover = Rgb(26, 26, 26);
                    p.Button = Rgb(56, 56, 56);
                    p.ButtonHover = Rgb(72, 72, 72);
                    p.RowHover = Rgb(52, 52, 52);
                    p.Border = Rgb(10, 10, 10);
                    p.Text = Rgb(204, 204, 204);
                    p.TextDim = Rgb(118, 118, 118);
                    p.Accent = Rgb(0, 112, 224);
                    p.DockEmpty = Rgb(14, 14, 14);
                    p.Rounding = 3.0f;
                    break;
            }
            return p;
        }

        Palette g_Palette = MakePalette(ThemePreset::Unreal);

        const char* FontPath(FontChoice font, int& faceIndex)
        {
            faceIndex = 0;
            switch (font)
            {
                case FontChoice::YuGothic:
                    return "C:\\Windows\\Fonts\\YuGothM.ttc";
                case FontChoice::Meiryo:
                    return "C:\\Windows\\Fonts\\meiryo.ttc";
                case FontChoice::MsGothic:
                    return "C:\\Windows\\Fonts\\msgothic.ttc";
                default:
                    return nullptr;
            }
        }

        std::string Serialize()
        {
            const auto& s = g_Settings;
            std::ostringstream out;
            out << "theme=" << static_cast<int>(s.Theme) << "\n";
            out << "custom_accent=" << (s.CustomAccent ? 1 : 0) << "\n";
            out << "accent=" << s.Accent.x << "," << s.Accent.y << "," << s.Accent.z << "\n";
            out << "ui_scale=" << s.UiScale << "\n";
            out << "rounding=" << s.Rounding << "\n";
            out << "compact=" << (s.Compact ? 1 : 0) << "\n";
            out << "font=" << static_cast<int>(s.Font) << "\n";
            out << "font_size=" << s.FontSize << "\n";
            out << "toolbar=" << (s.ShowToolbar ? 1 : 0) << "\n";
            out << "statusbar=" << (s.ShowStatusBar ? 1 : 0) << "\n";
            out << "area_headers=" << (s.ShowAreaHeaders ? 1 : 0) << "\n";
            out << "toasts=" << (s.ShowToasts ? 1 : 0) << "\n";
            out << "toast_success=" << (s.ToastOnSuccess ? 1 : 0) << "\n";
            out << "nav_gizmo=" << (s.ShowNavigationGizmo ? 1 : 0) << "\n";
            out << "viewport_stats=" << (s.ShowViewportStats ? 1 : 0) << "\n";
            out << "outliner_type=" << (s.ShowOutlinerTypeColumn ? 1 : 0) << "\n";
            out << "undo_limit=" << s.UndoLimit << "\n";
            out << "snap_t=" << (s.Snap.Translate ? 1 : 0) << "\n";
            out << "snap_r=" << (s.Snap.Rotate ? 1 : 0) << "\n";
            out << "snap_s=" << (s.Snap.Scale ? 1 : 0) << "\n";
            out << "snap_t_step=" << s.Snap.TranslateStep << "\n";
            out << "snap_r_step=" << s.Snap.RotateStep << "\n";
            out << "snap_s_step=" << s.Snap.ScaleStep << "\n";
            return out.str();
        }

        float ParseFloat(const std::string& value, float fallback)
        {
            char* end = nullptr;
            const float parsed = strtof(value.c_str(), &end);
            return (end != value.c_str() && std::isfinite(parsed)) ? parsed : fallback;
        }

        bool ParseBool(const std::string& value)
        {
            return value == "1" || value == "true";
        }

        void Sanitize()
        {
            auto& s = g_Settings;
            if (static_cast<int>(s.Theme) < 0 || s.Theme >= ThemePreset::Count)
            {
                s.Theme = ThemePreset::Unreal;
            }
            if (static_cast<int>(s.Font) < 0 || s.Font >= FontChoice::Count)
            {
                s.Font = FontChoice::Auto;
            }
            s.UiScale = std::clamp(s.UiScale, 0.75f, 2.0f);
            s.Rounding = std::clamp(s.Rounding, 0.0f, 8.0f);
            s.FontSize = std::clamp(s.FontSize, 12.0f, 24.0f);
            s.UndoLimit = std::clamp(s.UndoLimit, 5, 200);
            s.Snap.TranslateStep = std::clamp(s.Snap.TranslateStep, 0.001f, 1000.0f);
            s.Snap.RotateStep = std::clamp(s.Snap.RotateStep, 0.1f, 180.0f);
            s.Snap.ScaleStep = std::clamp(s.Snap.ScaleStep, 0.001f, 10.0f);
            for (float* channel : {&s.Accent.x, &s.Accent.y, &s.Accent.z})
            {
                *channel = std::clamp(*channel, 0.0f, 1.0f);
            }
            s.Accent.w = 1.0f;
        }

        bool SnapStepCombo(const char* label, float& value, const float* steps, int count, const char* format)
        {
            char preview[32];
            snprintf(preview, sizeof(preview), format, value);
            bool changed = false;
            if (ImGui::BeginCombo(label, preview))
            {
                for (int i = 0; i < count; ++i)
                {
                    char item[32];
                    snprintf(item, sizeof(item), format, steps[i]);
                    if (ImGui::Selectable(item, fabsf(steps[i] - value) < 1e-5f))
                    {
                        value = steps[i];
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            return changed;
        }
    } // namespace

    Settings& Get()
    {
        return g_Settings;
    }

    const char* ThemeName(ThemePreset preset)
    {
        switch (preset)
        {
            case ThemePreset::Unreal:
                return "Unreal（UE5 ダーク）";
            case ThemePreset::Blender:
                return "Blender（4.x ダーク）";
            case ThemePreset::Unity:
                return "Unity（Pro スキン）";
            case ThemePreset::Midnight:
                return "Midnight（シアン）";
            default:
                return "?";
        }
    }

    const char* FontName(FontChoice font)
    {
        switch (font)
        {
            case FontChoice::Auto:
                return "自動（游ゴシック → メイリオ → MS ゴシック）";
            case FontChoice::YuGothic:
                return "游ゴシック";
            case FontChoice::Meiryo:
                return "メイリオ";
            case FontChoice::MsGothic:
                return "MS ゴシック";
            default:
                return "?";
        }
    }

    void Load()
    {
        g_Settings = {};
        std::ifstream input(kPreferencesPath);
        if (input)
        {
            std::string line;
            auto& s = g_Settings;
            while (std::getline(input, line))
            {
                const auto separator = line.find('=');
                if (separator == std::string::npos)
                {
                    continue;
                }
                const std::string key = line.substr(0, separator);
                const std::string value = line.substr(separator + 1);
                if (key == "theme")
                    s.Theme = static_cast<ThemePreset>(atoi(value.c_str()));
                else if (key == "custom_accent")
                    s.CustomAccent = ParseBool(value);
                else if (key == "accent")
                {
                    float r = s.Accent.x, g = s.Accent.y, b = s.Accent.z;
                    if (sscanf_s(value.c_str(), "%f,%f,%f", &r, &g, &b) == 3)
                    {
                        s.Accent = ImVec4(r, g, b, 1.0f);
                    }
                }
                else if (key == "ui_scale")
                    s.UiScale = ParseFloat(value, s.UiScale);
                else if (key == "rounding")
                    s.Rounding = ParseFloat(value, s.Rounding);
                else if (key == "compact")
                    s.Compact = ParseBool(value);
                else if (key == "font")
                    s.Font = static_cast<FontChoice>(atoi(value.c_str()));
                else if (key == "font_size")
                    s.FontSize = ParseFloat(value, s.FontSize);
                else if (key == "toolbar")
                    s.ShowToolbar = ParseBool(value);
                else if (key == "statusbar")
                    s.ShowStatusBar = ParseBool(value);
                else if (key == "area_headers")
                    s.ShowAreaHeaders = ParseBool(value);
                else if (key == "toasts")
                    s.ShowToasts = ParseBool(value);
                else if (key == "toast_success")
                    s.ToastOnSuccess = ParseBool(value);
                else if (key == "nav_gizmo")
                    s.ShowNavigationGizmo = ParseBool(value);
                else if (key == "viewport_stats")
                    s.ShowViewportStats = ParseBool(value);
                else if (key == "outliner_type")
                    s.ShowOutlinerTypeColumn = ParseBool(value);
                else if (key == "undo_limit")
                    s.UndoLimit = atoi(value.c_str());
                else if (key == "snap_t")
                    s.Snap.Translate = ParseBool(value);
                else if (key == "snap_r")
                    s.Snap.Rotate = ParseBool(value);
                else if (key == "snap_s")
                    s.Snap.Scale = ParseBool(value);
                else if (key == "snap_t_step")
                    s.Snap.TranslateStep = ParseFloat(value, s.Snap.TranslateStep);
                else if (key == "snap_r_step")
                    s.Snap.RotateStep = ParseFloat(value, s.Snap.RotateStep);
                else if (key == "snap_s_step")
                    s.Snap.ScaleStep = ParseFloat(value, s.Snap.ScaleStep);
            }
        }
        Sanitize();
        g_LastSaved = Serialize();
        g_StyleDirty = true;
    }

    void LoadFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
        const float size = g_Settings.FontSize;
        std::error_code error;

        const FontChoice order[] = {FontChoice::YuGothic, FontChoice::Meiryo, FontChoice::MsGothic};
        ImFont* primary = nullptr;
        auto tryLoad = [&](FontChoice choice) {
            int faceIndex = 0;
            const char* path = FontPath(choice, faceIndex);
            if (!path || !std::filesystem::exists(path, error))
            {
                return false;
            }
            ImFontConfig config;
            config.FontNo = faceIndex;
            config.PixelSnapH = true;
            primary = io.Fonts->AddFontFromFileTTF(path, size, &config, io.Fonts->GetGlyphRangesJapanese());
            return primary != nullptr;
        };
        if (g_Settings.Font != FontChoice::Auto)
        {
            tryLoad(g_Settings.Font);
        }
        for (FontChoice choice : order)
        {
            if (primary)
            {
                break;
            }
            tryLoad(choice);
        }
        if (!primary)
        {
            io.Fonts->AddFontDefault();
        }

        // アイコン用のグリフ（矢印 / 図形 / 記号）を Segoe UI Symbol から補完する。
        // ImGui 1.92 以降は不足グリフを動的に読み込むため範囲指定は従来版のためのもの。
        static const ImWchar kSymbolRanges[] = {
            0x2000, 0x206F, // 一般句読点
            0x2190, 0x21FF, // 矢印
            0x2200, 0x22FF, // 数学記号
            0x2300, 0x23FF, // その他の技術記号（⏸ ⏹ ⏭ ⌕）
            0x2460, 0x24FF, // 囲み英数字
            0x2500, 0x257F, // 罫線
            0x25A0, 0x25FF, // 幾何学模様（▶ ■ ● ◆）
            0x2600, 0x26FF, // その他の記号（☀ ⚙）
            0x2700, 0x27BF, // 装飾記号（✓ ✕ ✥）
            0x2900, 0x297F, // 補助矢印（⤢）
            0,
        };
        const char* symbolPath = "C:\\Windows\\Fonts\\seguisym.ttf";
        if (primary && std::filesystem::exists(symbolPath, error))
        {
            ImFontConfig symbols;
            symbols.MergeMode = true;
            symbols.PixelSnapH = true;
            io.Fonts->AddFontFromFileTTF(symbolPath, size, &symbols, kSymbolRanges);
        }
    }

    void MarkStyleDirty()
    {
        g_StyleDirty = true;
    }

    void ApplyPending()
    {
        if (g_StyleDirty)
        {
            ApplyNow();
        }
    }

    void ApplyNow()
    {
        g_StyleDirty = false;
        Sanitize();
        const auto& s = g_Settings;
        g_Palette = MakePalette(s.Theme);
        if (s.CustomAccent)
        {
            g_Palette.Accent = s.Accent;
        }
        const Palette& p = g_Palette;

        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        ImVec4* c = style.Colors;
        const ImVec4 accent = p.Accent;
        const ImVec4 accentHover = Lerp(accent, ImVec4(1, 1, 1, 1), 0.15f);
        const ImVec4 accentActive = Lerp(accent, ImVec4(0, 0, 0, 1), 0.15f);

        c[ImGuiCol_Text] = p.Text;
        c[ImGuiCol_TextDisabled] = p.TextDim;
        c[ImGuiCol_WindowBg] = p.Window;
        c[ImGuiCol_ChildBg] = WithAlpha(p.Child, 0.0f); // 子ウィンドウはパネル色を透過させる
        c[ImGuiCol_PopupBg] = p.Popup;
        c[ImGuiCol_Border] = p.Border;
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = p.Frame;
        c[ImGuiCol_FrameBgHovered] = p.FrameHover;
        c[ImGuiCol_FrameBgActive] = Lerp(p.Frame, accent, 0.25f);
        c[ImGuiCol_TitleBg] = p.Menu;
        c[ImGuiCol_TitleBgActive] = p.Menu;
        c[ImGuiCol_TitleBgCollapsed] = p.Menu;
        c[ImGuiCol_MenuBarBg] = p.Menu;
        c[ImGuiCol_ScrollbarBg] = WithAlpha(p.Menu, 0.0f);
        c[ImGuiCol_ScrollbarGrab] = Lerp(p.Button, p.Window, 0.2f);
        c[ImGuiCol_ScrollbarGrabHovered] = p.ButtonHover;
        c[ImGuiCol_ScrollbarGrabActive] = accent;
        c[ImGuiCol_CheckMark] = accentHover;
        c[ImGuiCol_SliderGrab] = Lerp(accent, p.Button, 0.25f);
        c[ImGuiCol_SliderGrabActive] = accentHover;
        c[ImGuiCol_Button] = p.Button;
        c[ImGuiCol_ButtonHovered] = p.ButtonHover;
        c[ImGuiCol_ButtonActive] = accentActive;
        c[ImGuiCol_Header] = WithAlpha(accent, 0.55f);
        c[ImGuiCol_HeaderHovered] = p.RowHover;
        c[ImGuiCol_HeaderActive] = WithAlpha(accent, 0.75f);
        c[ImGuiCol_Separator] = p.Border;
        c[ImGuiCol_SeparatorHovered] = accent;
        c[ImGuiCol_SeparatorActive] = accentHover;
        c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = WithAlpha(accent, 0.6f);
        c[ImGuiCol_ResizeGripActive] = accent;
        c[ImGuiCol_Tab] = p.Menu;
        c[ImGuiCol_TabHovered] = p.RowHover;
        c[ImGuiCol_TabSelected] = p.Window;
        c[ImGuiCol_TabSelectedOverline] = accent;
        c[ImGuiCol_TabDimmed] = p.Menu;
        c[ImGuiCol_TabDimmedSelected] = Lerp(p.Menu, p.Window, 0.6f);
        c[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(accent, 0.0f);
        c[ImGuiCol_DockingPreview] = WithAlpha(accent, 0.55f);
        c[ImGuiCol_DockingEmptyBg] = p.DockEmpty;
        c[ImGuiCol_PlotLines] = p.TextDim;
        c[ImGuiCol_PlotLinesHovered] = accentHover;
        c[ImGuiCol_PlotHistogram] = accent;
        c[ImGuiCol_PlotHistogramHovered] = accentHover;
        c[ImGuiCol_TableHeaderBg] = Lerp(p.Menu, p.Window, 0.5f);
        c[ImGuiCol_TableBorderStrong] = p.Border;
        c[ImGuiCol_TableBorderLight] = Lerp(p.Border, p.Window, 0.5f);
        c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
        c[ImGuiCol_TextSelectedBg] = WithAlpha(accent, 0.40f);
        c[ImGuiCol_DragDropTarget] = accentHover;
        c[ImGuiCol_NavCursor] = accent;
        c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.7f);
        c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.45f);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);

        const float rounding = s.Rounding;
        style.WindowPadding = s.Compact ? ImVec2(6.0f, 6.0f) : ImVec2(8.0f, 8.0f);
        style.FramePadding = s.Compact ? ImVec2(6.0f, 2.0f) : ImVec2(8.0f, 4.0f);
        style.CellPadding = s.Compact ? ImVec2(4.0f, 2.0f) : ImVec2(5.0f, 3.0f);
        style.ItemSpacing = s.Compact ? ImVec2(6.0f, 3.0f) : ImVec2(8.0f, 5.0f);
        style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
        style.IndentSpacing = 14.0f;
        style.ScrollbarSize = 12.0f;
        style.GrabMinSize = 10.0f;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;
        style.TabBarBorderSize = 1.0f;
        style.TabBarOverlineSize = 2.0f;
        style.DockingSeparatorSize = 2.0f;
        style.WindowRounding = rounding + 1.0f;
        style.ChildRounding = rounding;
        style.FrameRounding = rounding;
        style.PopupRounding = rounding + 1.0f;
        style.GrabRounding = rounding;
        style.TabRounding = rounding;
        style.ScrollbarRounding = 9.0f;
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        style.WindowMenuButtonPosition = ImGuiDir_None;
        style.SeparatorTextBorderSize = 1.0f;
        style.SeparatorTextPadding = ImVec2(12.0f, 4.0f);
        style.DisabledAlpha = 0.45f;
        style.ScaleAllSizes(s.UiScale);

#if IMGUI_VERSION_NUM >= 19200
        style.FontScaleMain = s.UiScale;
#else
        ImGui::GetIO().FontGlobalScale = s.UiScale;
#endif
    }

    void Save()
    {
        const std::string contents = Serialize();
        std::error_code error;
        std::filesystem::create_directories("Save", error);
        const std::string temporary = std::string(kPreferencesPath) + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return;
            }
            output << contents;
            if (!output)
            {
                return;
            }
        }
        std::filesystem::rename(temporary, kPreferencesPath, error);
        if (error)
        {
            std::filesystem::remove(kPreferencesPath, error);
            std::filesystem::rename(temporary, kPreferencesPath, error);
        }
        if (!error)
        {
            g_LastSaved = contents;
        }
    }

    void SaveIfChanged()
    {
        if (Serialize() != g_LastSaved)
        {
            Save();
        }
    }

    ImVec4 Accent()
    {
        return g_Palette.Accent;
    }

    ImU32 AccentU32(float alpha)
    {
        return ImGui::GetColorU32(WithAlpha(g_Palette.Accent, alpha));
    }

    ImVec4 AccentHovered()
    {
        return Lerp(g_Palette.Accent, ImVec4(1, 1, 1, 1), 0.15f);
    }

    ImVec4 AccentActive()
    {
        return Lerp(g_Palette.Accent, ImVec4(0, 0, 0, 1), 0.15f);
    }

    ImVec4 TextColor()
    {
        return g_Palette.Text;
    }

    ImVec4 TextDim()
    {
        return g_Palette.TextDim;
    }

    ImVec4 PanelDark()
    {
        return g_Palette.Menu;
    }

    ImVec4 Success()
    {
        return ImVec4(0.36f, 0.80f, 0.45f, 1.0f);
    }

    ImVec4 Warning()
    {
        return ImVec4(0.98f, 0.72f, 0.20f, 1.0f);
    }

    ImVec4 Error()
    {
        return ImVec4(0.96f, 0.36f, 0.33f, 1.0f);
    }

    ImU32 AxisColor(int axis, float alpha)
    {
        // Blender / UE と同じ X=赤, Y=緑, Z=青
        static const ImVec4 colors[] = {ImVec4(0.93f, 0.27f, 0.32f, 1.0f), ImVec4(0.53f, 0.82f, 0.17f, 1.0f),
                                        ImVec4(0.20f, 0.52f, 0.96f, 1.0f)};
        return ImGui::GetColorU32(WithAlpha(colors[std::clamp(axis, 0, 2)], alpha));
    }

    void DrawPreferencesContent()
    {
        auto& s = g_Settings;
        bool styleChanged = false;

        if (ImGui::BeginTabBar("PreferencesTabs"))
        {
            if (ImGui::BeginTabItem("外観"))
            {
                ImGui::SeparatorText("テーマ");
                int theme = static_cast<int>(s.Theme);
                const int themeCount = static_cast<int>(ThemePreset::Count);
                for (int i = 0; i < themeCount; ++i)
                {
                    const Palette preview = MakePalette(static_cast<ThemePreset>(i));
                    ImGui::PushID(i);
                    const ImVec2 start = ImGui::GetCursorScreenPos();
                    const float swatch = ImGui::GetFrameHeight();
                    if (ImGui::Selectable("##Theme", theme == i, 0, ImVec2(0.0f, swatch)))
                    {
                        s.Theme = static_cast<ThemePreset>(i);
                        styleChanged = true;
                    }
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    const ImVec4 swatches[] = {preview.Menu, preview.Window, preview.Button, preview.Accent};
                    for (int k = 0; k < 4; ++k)
                    {
                        const ImVec2 a(start.x + 4.0f + k * (swatch * 0.8f), start.y + 3.0f);
                        draw->AddRectFilled(a, ImVec2(a.x + swatch * 0.8f - 2.0f, a.y + swatch - 6.0f),
                                            ImGui::GetColorU32(swatches[k]), 2.0f);
                    }
                    draw->AddText(ImVec2(start.x + 12.0f + 4 * swatch * 0.8f,
                                         start.y + (swatch - ImGui::GetTextLineHeight()) * 0.5f),
                                  ImGui::GetColorU32(ImGuiCol_Text), ThemeName(static_cast<ThemePreset>(i)));
                    ImGui::PopID();
                }

                ImGui::SeparatorText("アクセントカラー");
                styleChanged |= ImGui::Checkbox("カスタムアクセントを使う", &s.CustomAccent);
                ImGui::BeginDisabled(!s.CustomAccent);
                ImGui::SameLine();
                float accent[3] = {s.Accent.x, s.Accent.y, s.Accent.z};
                if (ImGui::ColorEdit3("##Accent", accent, ImGuiColorEditFlags_NoInputs))
                {
                    s.Accent = ImVec4(accent[0], accent[1], accent[2], 1.0f);
                    styleChanged = true;
                }
                ImGui::SameLine();
                static const ImVec4 quickAccents[] = {Rgb(0, 112, 224), Rgb(255, 140, 0), Rgb(71, 114, 179),
                                                      Rgb(46, 174, 209), Rgb(124, 92, 255), Rgb(60, 180, 110)};
                for (int i = 0; i < IM_ARRAYSIZE(quickAccents); ++i)
                {
                    ImGui::PushID(100 + i);
                    if (ImGui::ColorButton("##Quick", quickAccents[i], ImGuiColorEditFlags_NoTooltip,
                                           ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
                    {
                        s.Accent = quickAccents[i];
                        styleChanged = true;
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                ImGui::NewLine();
                ImGui::EndDisabled();

                ImGui::SeparatorText("サイズ");
                ImGui::SetNextItemWidth(220.0f);
                ImGui::SliderFloat("UI スケール", &s.UiScale, 0.75f, 2.0f, "%.2fx");
                styleChanged |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SetNextItemWidth(220.0f);
                ImGui::SliderFloat("角の丸み", &s.Rounding, 0.0f, 8.0f, "%.1f px");
                styleChanged |= ImGui::IsItemDeactivatedAfterEdit();
                styleChanged |= ImGui::Checkbox("コンパクト表示（余白を詰める）", &s.Compact);

                ImGui::SeparatorText("フォント（再起動後に反映）");
                int font = static_cast<int>(s.Font);
                ImGui::SetNextItemWidth(320.0f);
                if (ImGui::BeginCombo("フォント", FontName(s.Font)))
                {
                    for (int i = 0; i < static_cast<int>(FontChoice::Count); ++i)
                    {
                        if (ImGui::Selectable(FontName(static_cast<FontChoice>(i)), font == i))
                        {
                            s.Font = static_cast<FontChoice>(i);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetNextItemWidth(220.0f);
                ImGui::SliderFloat("フォントサイズ", &s.FontSize, 12.0f, 24.0f, "%.0f px");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("インターフェース"))
            {
                ImGui::Checkbox("メインツールバー", &s.ShowToolbar);
                ImGui::Checkbox("ステータスバー", &s.ShowStatusBar);
                ImGui::Checkbox("パネルヘッダー（エディタータイプ切替 / 最大化）", &s.ShowAreaHeaders);
                ImGui::Checkbox("通知トースト", &s.ShowToasts);
                ImGui::BeginDisabled(!s.ShowToasts);
                ImGui::Indent();
                ImGui::Checkbox("成功メッセージも通知する", &s.ToastOnSuccess);
                ImGui::Unindent();
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::Checkbox("ビューポート: ナビゲーションギズモ", &s.ShowNavigationGizmo);
                ImGui::Checkbox("ビューポート: 統計オーバーレイ", &s.ShowViewportStats);
                ImGui::Checkbox("アウトライナー: 種類の列", &s.ShowOutlinerTypeColumn);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("編集"))
            {
                ImGui::SetNextItemWidth(220.0f);
                ImGui::SliderInt("元に戻す回数", &s.UndoLimit, 5, 200, "%d 回", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip("多くするとメモリ使用量が増えます（各ステップはコンポーネントのスナップショット）");
                ImGui::SeparatorText("スナップ（Ctrl を押している間は反転）");
                static const float translateSteps[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f};
                static const float rotateSteps[] = {1.0f, 5.0f, 10.0f, 15.0f, 22.5f, 30.0f, 45.0f, 90.0f};
                static const float scaleSteps[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f};
                ImGui::Checkbox("##SnapT", &s.Snap.Translate);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0f);
                SnapStepCombo("移動", s.Snap.TranslateStep, translateSteps, IM_ARRAYSIZE(translateSteps), "%.2f m");
                ImGui::Checkbox("##SnapR", &s.Snap.Rotate);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0f);
                SnapStepCombo("回転", s.Snap.RotateStep, rotateSteps, IM_ARRAYSIZE(rotateSteps), "%.1f°");
                ImGui::Checkbox("##SnapS", &s.Snap.Scale);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0f);
                SnapStepCombo("スケール", s.Snap.ScaleStep, scaleSteps, IM_ARRAYSIZE(scaleSteps), "%.2f");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("既定値に戻す"))
        {
            const FontChoice font = s.Font;
            const float fontSize = s.FontSize;
            s = Settings{};
            s.Font = font; // フォントは再起動が必要なので維持する
            s.FontSize = fontSize;
            styleChanged = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("設定は Save/editor_preferences.cfg に自動保存されます");
        if (styleChanged)
        {
            MarkStyleDirty();
        }
    }
} // namespace EditorPreferences
