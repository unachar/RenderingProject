#include "pch.h"
#include "world.h"

float World::m_DeltaTime = 0.0f;
float World::m_FrameRate = 0.0f;
float World::m_FrameTimeMs = 0.0f;
double World::m_FrameStatsAccumulator = 0.0;
int World::m_FrameStatsCount = 0;
long long World::m_LastTime = 0;
long long World::m_Frequency = 0;
bool World::m_VSyncEnabled = true;
bool World::m_FixedFrameRateEnabled = false;
int World::m_TargetFrameRate = 60;
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

Entity World::GetEntityByName(const string& name)
{
    auto it = m_NameMap.find(name);
    if (it != m_NameMap.end())
    {
        return Entity(it->second);
    }
    return Entity(g_kINVALID_ENTITY);
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
    if (m_Frequency == 0)
    {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        m_Frequency = li.QuadPart;
        QueryPerformanceCounter(&li);
        m_LastTime = li.QuadPart;
        m_DeltaTime = 1.0f / 60.0f;
        return;
    }

    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    long long currentTime = li.QuadPart;
    m_DeltaTime = static_cast<float>(currentTime - m_LastTime) / m_Frequency;
    m_LastTime = currentTime;

    if (m_DeltaTime > 0.1f)
    {
        m_DeltaTime = 0.1f;
    }
    else if (m_DeltaTime < 0.0f)
    {
        m_DeltaTime = 0.0f;
    }

    m_FrameTimeMs = m_DeltaTime * 1000.0f;
    m_FrameStatsAccumulator += m_DeltaTime;
    m_FrameStatsCount++;
    if (m_FrameStatsAccumulator >= 0.25)
    {
        m_FrameRate = m_FrameStatsAccumulator > 0.0
            ? static_cast<float>(m_FrameStatsCount / m_FrameStatsAccumulator)
            : 0.0f;
        m_FrameStatsAccumulator = 0.0;
        m_FrameStatsCount = 0;
    }
}

void World::WaitForFrameLimit()
{
    if (!m_FixedFrameRateEnabled || m_TargetFrameRate <= 0 || m_Frequency == 0)
    {
        return;
    }

    const double targetSeconds = 1.0 / static_cast<double>(m_TargetFrameRate);
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);

    const long long targetTicks = static_cast<long long>(targetSeconds * static_cast<double>(m_Frequency));
    const long long targetTime = m_LastTime + targetTicks;
    while (li.QuadPart < targetTime)
    {
        const double remainingSeconds = static_cast<double>(targetTime - li.QuadPart) / static_cast<double>(m_Frequency);
        if (remainingSeconds > 0.002)
        {
            Sleep(static_cast<DWORD>((remainingSeconds * 1000.0) - 1.0));
        }
        else
        {
            Sleep(0);
        }
        QueryPerformanceCounter(&li);
    }
}

float World::GetDeltaTime()
{
    return m_DeltaTime;
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

