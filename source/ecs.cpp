#include "pch.h"
#include "ecs.h"
#include "rendererdraw.h"
#include "componentmanager.h"
#include "world.h"


vector<EntityData> Registry::m_Entities;
vector<EntityID> Registry::m_ActiveEntities[g_kMAX_COMPONENTS];
vector<int32_t> Registry::m_EntityToIndex[g_kMAX_COMPONENTS];
queue<EntityID> Registry::m_FreeList;
uint32_t Registry::m_NextEntityId = 0;
uint64_t Registry::m_StructureVersion = 1;

unordered_map<type_index, ComponentTypeID>& ComponentTypeRegistry::TypeIds()
{
    static unordered_map<type_index, ComponentTypeID> typeIds;
    return typeIds;
}

vector<void(*)(EntityID)>& ComponentTypeRegistry::ClearCallbacks()
{
    static vector<void(*)(EntityID)> clearCallbacks;
    return clearCallbacks;
}

ComponentTypeID& ComponentTypeRegistry::NextTypeId()
{
    static ComponentTypeID nextTypeId = 0;
    return nextTypeId;
}

void ComponentTypeRegistry::ClearComponent(EntityID entity, ComponentType type)
{
    auto& clearCallbacks = ClearCallbacks();
    if (type.Value < clearCallbacks.size())
    {
        clearCallbacks[type.Value](entity);
    }
}

ComponentTypeID ComponentTypeRegistry::GetRegisteredCount()
{
    return NextTypeId();
}

void Registry::Init()
{
    m_NextEntityId = 0;
    ComponentManager::Init();
    for (ComponentTypeID i = 0; i < g_kMAX_COMPONENTS; ++i)
    {
        m_ActiveEntities[i].clear();
        m_EntityToIndex[i].assign(g_kMAX_ENTITIES, -1);
    }

    if (m_Entities.empty())
    {
        m_Entities.resize(g_kMAX_ENTITIES);
    }

    TouchStructure();
}

EntityID Registry::CreateEntity()
{
    EntityID id = g_kINVALID_ENTITY;

    if (!m_FreeList.empty())
    {
        id = m_FreeList.front();
        m_FreeList.pop();
    }
    else
    {
        if (m_NextEntityId >= g_kMAX_ENTITIES)
        {
            Debug::Log("ERROR: Entity limit reached!\n");
            return g_kINVALID_ENTITY;
        }
        id = m_NextEntityId++;
    }

    m_Entities[id].IsAlive = true;
    m_Entities[id].Mask.reset();
    TouchStructure();

    return id;
}

bool Registry::RestoreEntity(EntityID entity)
{
    if (entity >= g_kMAX_ENTITIES || m_Entities[entity].IsAlive)
    {
        return false;
    }

    queue<EntityID> remaining;
    while (!m_FreeList.empty())
    {
        EntityID freeEntity = m_FreeList.front();
        m_FreeList.pop();
        if (freeEntity != entity)
        {
            remaining.push(freeEntity);
        }
    }
    m_FreeList = remaining;

    m_Entities[entity].IsAlive = true;
    m_Entities[entity].Mask.reset();
    TouchStructure();
    return true;
}

void Registry::DestroyEntity(EntityID entity)
{
    if (entity >= g_kMAX_ENTITIES || !m_Entities[entity].IsAlive)
    {
        return;
    }

    for (ComponentTypeID i = 0; i < ComponentTypeRegistry::GetRegisteredCount(); ++i)
    {
        if (m_Entities[entity].Mask.test(i))
        {
            int32_t targetIdx = m_EntityToIndex[i][entity];
            if (targetIdx != -1)
            {
                EntityID lastEntity = m_ActiveEntities[i].back();
                m_ActiveEntities[i][targetIdx] = lastEntity;
                m_EntityToIndex[i][lastEntity] = targetIdx;
                m_ActiveEntities[i].pop_back();
                m_EntityToIndex[i][entity] = -1;
            }
        }
    }

    ComponentManager::ClearEntity(entity);
    m_Entities[entity].IsAlive = false;
    m_Entities[entity].Mask.reset();
    m_FreeList.push(entity);
    TouchStructure();

    World::UnregisterName(entity);
}
