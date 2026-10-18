#include "pch.h"
#include "world.h"
#include "renderbenchmark.h"

float World::m_DeltaTime = 0.0f;
float World::m_RawDeltaTime = 0.0f;
float World::m_SmoothedDeltaTime = 0.0f;
float World::m_FrameRate = 0.0f;
float World::m_FrameTimeMs = 0.0f;
double World::m_FrameStatsAccumulator = 0.0;
int World::m_FrameStatsCount = 0;
long long World::m_LastTime = 0;
long long World::m_Frequency = 0;
bool World::m_VSyncEnabled = false;
bool World::m_FixedFrameRateEnabled = true;
int World::m_TargetFrameRate = 120;
unordered_map<string, EntityID> World::m_NameMap;
unordered_map<EntityID, string> World::m_IdToNameMap;

void World::RegisterName(EntityID entity, const string& name)
{
    if (entity >= g_kMAX_ENTITIES || name.empty())
    {
        return;
    }

    auto oldIt = m_IdToNameMap.find(entity);
    if (oldIt != m_IdToNameMap.end())
    {
        m_NameMap.erase(oldIt->second);
    }

    auto sameNameIt = m_NameMap.find(name);
    if (sameNameIt != m_NameMap.end())
    {
        m_IdToNameMap.erase(sameNameIt->second);
    }

    m_NameMap[name] = entity;
    m_IdToNameMap[entity] = name;
}

void World::UnregisterName(EntityID entity)
{
    auto it = m_IdToNameMap.find(entity);
    if (it == m_IdToNameMap.end())
    {
        return;
    }

    m_NameMap.erase(it->second);
    m_IdToNameMap.erase(it);
}

void World::Update()
{
    constexpr float kMaxDeltaTime = 0.1f;
    constexpr float kSmoothingResetDelta = 1.0f / 20.0f;
    constexpr float kFastFrameDelta = 1.0f / 120.0f;

    if (m_Frequency == 0)
    {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        m_Frequency = li.QuadPart;
        QueryPerformanceCounter(&li);
        m_LastTime = li.QuadPart;
        m_DeltaTime = 1.0f / 60.0f;
        m_RawDeltaTime = m_DeltaTime;
        m_SmoothedDeltaTime = m_DeltaTime;
        m_FrameTimeMs = m_DeltaTime * 1000.0f;
        return;
    }

    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    long long currentTime = li.QuadPart;
    m_RawDeltaTime = static_cast<float>(currentTime - m_LastTime) / m_Frequency;
    m_LastTime = currentTime;

    if (m_RawDeltaTime > kMaxDeltaTime)
    {
        m_RawDeltaTime = kMaxDeltaTime;
    }
    else if (m_RawDeltaTime < 0.0f)
    {
        m_RawDeltaTime = 0.0f;
    }

    if (m_SmoothedDeltaTime <= 0.0f || m_RawDeltaTime >= kSmoothingResetDelta)
    {
        m_SmoothedDeltaTime = m_RawDeltaTime;
    }
    else
    {
        float smoothRate;
        if ((m_RawDeltaTime <= kFastFrameDelta))
        {
            smoothRate = 0.18f;
        }
        else
        {
            smoothRate = 0.32f;
        }
        m_SmoothedDeltaTime += (m_RawDeltaTime - m_SmoothedDeltaTime) * smoothRate;
    }

    m_DeltaTime = m_SmoothedDeltaTime;
    m_FrameTimeMs = m_RawDeltaTime * 1000.0f;
    m_FrameStatsAccumulator += m_RawDeltaTime;
    m_FrameStatsCount++;
    if (m_FrameStatsAccumulator >= 0.5)
    {
        if (m_FrameStatsAccumulator > 0.0)
        {
            m_FrameRate = static_cast<float>(m_FrameStatsCount / m_FrameStatsAccumulator);
        }
        else
        {
            m_FrameRate = 0.0f;
        }
        m_FrameStatsAccumulator = 0.0;
        m_FrameStatsCount = 0;
    }
}

void World::WaitForFrameLimit()
{
    // 目標フレームレートに合わせて待機する。
    // ・締め切りを前回の締め切りから積み上げるので、誤差が蓄積しない（120fpsで8.333ms刻みを維持）
    // ・高精度ウェイタブルタイマーで大部分を眠り、最後の0.6msだけスピンする（Sleepの15.6ms粒度を回避）
    // ・大きく遅れたフレームでは締め切りを現在時刻に合わせ直し、追いつこうとして連続で詰めない
    static HANDLE s_Timer = nullptr;
    static bool s_TimerCreated = false;
    static long long s_NextDeadline = 0;
    if (!m_FixedFrameRateEnabled || m_TargetFrameRate <= 0 || m_Frequency == 0)
    {
        s_NextDeadline = 0;
        return;
    }
    if (!s_TimerCreated)
    {
        s_TimerCreated = true;
#ifdef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        s_Timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
        if (!s_Timer)
        {
            s_Timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
    }

    const long long period =
        static_cast<long long>(static_cast<double>(m_Frequency) / static_cast<double>(m_TargetFrameRate));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (s_NextDeadline == 0 || now.QuadPart - s_NextDeadline > period)
    {
        // 初回、または1フレーム以上遅れた場合は基準を取り直す。
        s_NextDeadline = now.QuadPart + period;
        if (m_LastTime > 0 && m_LastTime + period > now.QuadPart)
        {
            s_NextDeadline = m_LastTime + period;
        }
    }

    const long long spinTicks = m_Frequency * 6 / 10000; // 0.6ms
    for (;;)
    {
        QueryPerformanceCounter(&now);
        const long long remaining = s_NextDeadline - now.QuadPart;
        if (remaining <= 0)
        {
            break;
        }
        if (remaining > spinTicks && s_Timer)
        {
            const long long sleepTicks = remaining - spinTicks;
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<long long>(static_cast<double>(sleepTicks) * 10000000.0 /
                                                   static_cast<double>(m_Frequency));
            if (due.QuadPart < 0 && SetWaitableTimer(s_Timer, &due, 0, nullptr, nullptr, FALSE))
            {
                WaitForSingleObject(s_Timer, 1000);
                continue;
            }
        }
        if (remaining > spinTicks)
        {
            Sleep(remaining > spinTicks * 4 ? 1 : 0);
        }
        else
        {
            YieldProcessor();
        }
    }
    s_NextDeadline += period;
}

float World::GetDeltaTime()
{
    if (RenderBenchmark::IsEnabled())
    {
        return 1.0f / 60.0f;
    }
    return m_DeltaTime;
}

float World::GetRawDeltaTime()
{
    if (RenderBenchmark::IsEnabled())
    {
        return 1.0f / 60.0f;
    }
    return m_RawDeltaTime;
}

float World::GetFrameRate()
{
    return m_FrameRate;
}

float World::GetFrameTimeMs()
{
    return m_FrameTimeMs;
}

bool World::IsVSyncEnabled()
{
    return m_VSyncEnabled;
}

void World::SetVSyncEnabled(bool enabled)
{
    m_VSyncEnabled = enabled;
}

bool World::IsFixedFrameRateEnabled()
{
    return m_FixedFrameRateEnabled;
}

void World::SetFixedFrameRateEnabled(bool enabled)
{
    m_FixedFrameRateEnabled = enabled;
}

int World::GetTargetFrameRate()
{
    return m_TargetFrameRate;
}

void World::SetTargetFrameRate(int fps)
{
    m_TargetFrameRate = max(1, min(fps, 360));
}
