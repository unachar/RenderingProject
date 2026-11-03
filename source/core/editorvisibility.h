#pragma once
// ============================================================================
// EditorVisibility：アウトライナーの「表示（目）」と「ロック（鍵）」（#5、UE の Hidden in Editor と同じ）
//  ・エディター上だけの状態。シーンファイルには保存しない（再生中は非表示を無視してすべて描く）
//  ・非表示：各描画経路（ラスター / インスタンス / GPU シーン / レイトレーシング）が IsHidden で除外する
//  ・ロック：ビューポートのクリック選択とギズモの対象から外す（アウトライナーからは選択できる）
// ============================================================================
#include <cstdint>
#include <unordered_set>
#include "gpuscene/gpuscenechanges.h"

namespace EditorVisibility
{
    inline std::unordered_set<uint32_t> g_Hidden;
    inline std::unordered_set<uint32_t> g_Locked;
    inline bool g_Enabled = true; // false = 再生中（非表示を無視）
    inline uint64_t g_Revision = 1;

    inline bool IsHidden(uint32_t entity)
    {
        return g_Enabled && !g_Hidden.empty() && g_Hidden.contains(entity);
    }

    inline bool IsHiddenFlag(uint32_t entity)
    {
        return g_Hidden.contains(entity);
    }

    inline bool IsLocked(uint32_t entity)
    {
        return !g_Locked.empty() && g_Locked.contains(entity);
    }

    inline void SetHidden(uint32_t entity, bool hidden)
    {
        const bool changed = hidden ? g_Hidden.insert(entity).second : g_Hidden.erase(entity) != 0;
        if (changed)
        {
            ++g_Revision;
            GpuSceneChanges::Notify(entity); // GPU シーンのインスタンスを作り直す
        }
    }

    inline void SetLocked(uint32_t entity, bool locked)
    {
        if (locked)
        {
            g_Locked.insert(entity);
        }
        else
        {
            g_Locked.erase(entity);
        }
    }

    // 再生の開始 / 終了で切り替える（非表示のエンティティを GPU シーンに出し入れする）
    inline void SetEnabled(bool enabled)
    {
        if (g_Enabled == enabled)
        {
            return;
        }
        g_Enabled = enabled;
        ++g_Revision;
        for (const uint32_t entity : g_Hidden)
        {
            GpuSceneChanges::Notify(entity);
        }
    }

    // エンティティの削除 / シーンの読み替え
    inline void Forget(uint32_t entity)
    {
        g_Hidden.erase(entity);
        g_Locked.erase(entity);
    }

    inline void Clear()
    {
        g_Hidden.clear();
        g_Locked.clear();
        ++g_Revision;
    }
} // namespace EditorVisibility
