#pragma once
#include <string>
// ============================================================================
// EditorIcons（#13）：エディターのアイコンを一か所で管理する
//  ・Windows のアイコンフォント（Segoe Fluent Icons → Segoe MDL2 Assets）を私用領域 U+E700〜U+F8FF に
//    結合して使う。どちらも無い環境では Unicode 記号（Segoe UI Symbol から補完）に戻す
//  ・Unicode 記号は日本語フォント側の字形を除外し、すべて Segoe UI Symbol から取る（見た目を統一）
//  ・コードの中でアイコン文字を直書きせず、ここの関数を使う
// ============================================================================
namespace EditorIcons
{
    // EditorPreferences::LoadFonts が、アイコンフォントを結合できたときに true にする
    inline bool g_IconFont = false;

    // アイコンフォントの候補（上から順に使う）
    inline const char* const kIconFontPaths[] = {
        "C:\\Windows\\Fonts\\SegoeIcons.ttf", // Segoe Fluent Icons（Windows 11）
        "C:\\Windows\\Fonts\\segmdl2.ttf",    // Segoe MDL2 Assets（Windows 10）
    };

    // 表示（U+E890）
    inline const char* Visible()
    {
        return g_IconFont ? "\xEE\xA2\x90" : "●";
    }

    // 非表示（U+ED1A）
    inline const char* Hidden()
    {
        return g_IconFont ? "\xEE\xB4\x9A" : "◌";
    }

    // ロック（U+E72E）
    inline const char* Lock()
    {
        return g_IconFont ? "\xEE\x9C\xAE" : "■";
    }

    // ロック解除（U+E785）
    inline const char* Unlock()
    {
        return g_IconFont ? "\xEE\x9E\x85" : "□";
    }

    // フォルダ（U+E8B7）
    inline const char* Folder()
    {
        return g_IconFont ? "\xEE\xA2\xB7" : "▤";
    }

    // 再生（U+E768）
    inline const char* Play()
    {
        return g_IconFont ? "\xEE\x9D\xA8" : "▶";
    }

    // 一時停止（U+E769）
    inline const char* Pause()
    {
        return g_IconFont ? "\xEE\x9D\xA9" : "❚❚";
    }

    // 停止（U+E71A）
    inline const char* Stop()
    {
        return g_IconFont ? "\xEE\x9C\x9A" : "■";
    }

    // 保存（U+E74E）
    inline const char* Save()
    {
        return g_IconFont ? "\xEE\x9D\x8E" : "保存";
    }

    // 元に戻す（U+E7A7）
    inline const char* Undo()
    {
        return g_IconFont ? "\xEE\x9E\xA7" : "↶";
    }

    // やり直し（U+E7A6）
    inline const char* Redo()
    {
        return g_IconFont ? "\xEE\x9E\xA6" : "↷";
    }

    // 検索（U+E721）
    inline const char* Search()
    {
        return g_IconFont ? "\xEE\x9C\xA1" : "⌕";
    }

    // 設定（U+E713）
    inline const char* Settings()
    {
        return g_IconFont ? "\xEE\x9C\x93" : "⚙";
    }

    // 追加（U+E710）
    inline const char* Add()
    {
        return g_IconFont ? "\xEE\x9C\x90" : "✚";
    }

    // 再読み込み（U+E72C）
    inline const char* Refresh()
    {
        return g_IconFont ? "\xEE\x9C\xAC" : "↻";
    }

    // お気に入り（U+E734）
    inline const char* Star()
    {
        return g_IconFont ? "\xEE\x9C\xB4" : "☆";
    }

    // お気に入り（登録済み）（U+E735）
    inline const char* StarFill()
    {
        return g_IconFont ? "\xEE\x9C\xB5" : "★";
    }

    // 履歴（U+E81C）
    inline const char* History()
    {
        return g_IconFont ? "\xEE\xA0\x9C" : "◷";
    }

    // 一覧表示（U+E8FD）
    inline const char* ListView()
    {
        return g_IconFont ? "\xEE\xA3\xBD" : "☰";
    }

    // サムネイル表示（U+ECA5）
    inline const char* GridView()
    {
        return g_IconFont ? "\xEE\xB2\xA5" : "▦";
    }

    // 上へ（U+E74A）
    inline const char* Up()
    {
        return g_IconFont ? "\xEE\x9D\x8A" : "↑";
    }

    // ルート（U+E80F）
    inline const char* Home()
    {
        return g_IconFont ? "\xEE\xA0\x8F" : "⌂";
    }

    // フィルター（U+E71C）
    inline const char* Filter()
    {
        return g_IconFont ? "\xEE\x9C\x9C" : "⏷";
    }

    // カメラ（U+E722）
    inline const char* Camera()
    {
        return g_IconFont ? "\xEE\x9C\xA2" : "◉";
    }

    // ライト（U+E706）
    inline const char* Sun()
    {
        return g_IconFont ? "\xEE\x9C\x86" : "☀";
    }

    // 回転（U+E7AD）
    inline const char* Rotate()
    {
        return g_IconFont ? "\xEE\x9E\xAD" : "↻";
    }

    // スケール（U+E740）
    inline const char* Scale()
    {
        return g_IconFont ? "\xEE\x9D\x80" : "⤢";
    }

    // 展開（U+E70D）
    inline const char* ChevronDown()
    {
        return g_IconFont ? "\xEE\x9C\x8D" : "▾";
    }

    // ラベル付きボタン用："<アイコン> テキスト"
    inline std::string Label(const char* icon, const char* text)
    {
        return std::string(icon) + " " + text;
    }
} // namespace EditorIcons
