#include "pch.h"
#include "world.h"

float World::m_DeltaTime = 0.0f;
long long World::m_LastTime = 0;
long long World::m_Frequency = 0;
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
}

float World::GetDeltaTime()
{
    return m_DeltaTime;
}


