#pragma once
// ============================================================================
// BackgroundTasks（#11）：裏で動いている処理の数を集計し、エディターのステータスバーに出す
//  ・ワーカースレッドから使える（mutex で保護）
//  ・Scope：処理の間だけ数える（std::async のラムダの先頭に置くだけ）
//  ・SetPolled：毎フレーム数を問い合わせるもの（サムネイル生成の待ちなど）
// ============================================================================
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace BackgroundTasks
{
    struct Entry
    {
        std::string Name;
        int Count = 0;
    };

    inline std::mutex g_Mutex;
    inline std::map<std::string, int> g_Running; // 名前 → 実行中の数
    inline std::map<std::string, int> g_Polled;  // 名前 → 毎フレーム更新される数

    inline void Begin(const char* name)
    {
        std::lock_guard lock(g_Mutex);
        ++g_Running[name];
    }

    inline void End(const char* name)
    {
        std::lock_guard lock(g_Mutex);
        auto found = g_Running.find(name);
        if (found != g_Running.end() && --found->second <= 0)
        {
            g_Running.erase(found);
        }
    }

    // 0 で消える
    inline void SetPolled(const char* name, int count)
    {
        std::lock_guard lock(g_Mutex);
        if (count > 0)
        {
            g_Polled[name] = count;
        }
        else
        {
            g_Polled.erase(name);
        }
    }

    inline std::vector<Entry> Snapshot()
    {
        std::lock_guard lock(g_Mutex);
        std::map<std::string, int> merged = g_Polled;
        for (const auto& [name, count] : g_Running)
        {
            merged[name] += count;
        }
        std::vector<Entry> entries;
        for (const auto& [name, count] : merged)
        {
            if (count > 0)
            {
                entries.push_back({name, count});
            }
        }
        return entries;
    }

    class Scope
    {
      public:
        explicit Scope(const char* name) : m_Name(name)
        {
            Begin(m_Name);
        }
        ~Scope()
        {
            End(m_Name);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

      private:
        const char* m_Name;
    };
} // namespace BackgroundTasks
