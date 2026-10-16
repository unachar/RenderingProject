#include "pch.h"
#include "ecs.h"
#include "componentmanager.h"
#include "world.h"


vector<EntityData> Registry::m_Entities;
vector<EntityID> Registry::m_ActiveEntities[g_kMAX_COMPONENTS];
vector<int32_t> Registry::m_EntityToIndex[g_kMAX_COMPONENTS];
queue<EntityID> Registry::m_FreeList;
uint32_t Registry::m_NextEntityId = 0;
uint64_t Registry::m_StructureVersion = 1;
uint64_t Registry::m_TypeVersion[g_kMAX_COMPONENTS]{};
vector<uint8_t> Registry::m_IsFree;

unordered_map<type_index, ComponentTypeID>& ComponentTypeRegistry::TypeIds()
{
    static unordered_map<type_index, ComponentTypeID> typeIds;
    return typeIds;
}

vector<void(*)(EntityID)>& ComponentTypeRegistry::CreateCallbacks()
{
    static vector<void(*)(EntityID)> createCallbacks;
    return createCallbacks;
}

vector<void(*)(EntityID)>& ComponentTypeRegistry::ClearCallbacks()
{
    static vector<void(*)(EntityID)> clearCallbacks;
    return clearCallbacks;
}

vector<void(*)()>& ComponentTypeRegistry::ResetCallbacks()
{
    static vector<void(*)()> resetCallbacks;
    return resetCallbacks;
}

ComponentTypeID& ComponentTypeRegistry::NextTypeId()
{
    static ComponentTypeID nextTypeId = 0;
    return nextTypeId;
}

void ComponentTypeRegistry::CreateComponent(EntityID entity, ComponentType type)
{
    auto& createCallbacks = CreateCallbacks();
    if (type.Value < createCallbacks.size())
    {
        createCallbacks[type.Value](entity);
    }
}

void ComponentTypeRegistry::ClearComponent(EntityID entity, ComponentType type)
{
    auto& clearCallbacks = ClearCallbacks();
    if (type.Value < clearCallbacks.size())
    {
        clearCallbacks[type.Value](entity);
    }
}

void ComponentTypeRegistry::ResetComponents()
{
    for (auto reset : ResetCallbacks())
    {
        reset();
    }
}


void Registry::Init()
{
    GpuSceneChanges::Reset();
    m_NextEntityId = 0;
    m_FreeList = {};
    ComponentTypeRegistry::ResetComponents();
    ComponentManager::Init();
    for (ComponentTypeID i = 0; i < g_kMAX_COMPONENTS; ++i)
    {
        m_ActiveEntities[i].clear();
        m_EntityToIndex[i].assign(g_kMAX_ENTITIES, -1);
    }

    m_Entities.assign(g_kMAX_ENTITIES, EntityData{});
    m_IsFree.assign(g_kMAX_ENTITIES, 0);

    // すべてのクエリキャッシュを無効化
    for (ComponentTypeID i = 0; i < g_kMAX_COMPONENTS; ++i)
    {
        TouchType(i);
    }
}

EntityID Registry::CreateEntity()
{
    EntityID id = g_kINVALID_ENTITY;

    // FIFO（従来と同じ払い出し順）。RestoreEntityで取り消された番号は読み飛ばす。
    while (!m_FreeList.empty())
    {
        const EntityID candidate = m_FreeList.front();
        m_FreeList.pop();
        if (m_IsFree[candidate])
        {
            m_IsFree[candidate] = 0;
            id = candidate;
            break;
        }
    }
    if (id == g_kINVALID_ENTITY)
    {
        if (m_NextEntityId >= g_kMAX_ENTITIES)
        {
            Debug::Log("ERROR: Entity limit reached!\n");
            return g_kINVALID_ENTITY;
        }
        id = m_NextEntityId++;
    }

    // コンポーネントを持たないエンティティはどのクエリにも現れないため、キャッシュ無効化は不要
    m_Entities[id].IsAlive = true;
    m_Entities[id].Mask.reset();
    return id;
}

bool Registry::RestoreEntity(EntityID entity)
{
    if (entity >= g_kMAX_ENTITIES || m_Entities[entity].IsAlive)
    {
        return false;
    }

    // 以前はFreeList全体を作り直していた（O(n)）。印を外すだけにする（O(1)）。
    m_IsFree[entity] = 0;
    m_Entities[entity].IsAlive = true;
    m_Entities[entity].Mask.reset();
    return true;
}

void Registry::DestroyEntity(EntityID entity)
{
    if (entity >= g_kMAX_ENTITIES || !m_Entities[entity].IsAlive)
    {
        return;
    }

    // 立っているビットだけを1回走査し、一覧からの除去とストレージの解放を同時に行う
    unsigned long bits = m_Entities[entity].Mask.to_ulong();
    while (bits != 0)
    {
        unsigned long type = 0;
        _BitScanForward(&type, bits);
        bits &= bits - 1;
        const int32_t targetIdx = m_EntityToIndex[type][entity];
        if (targetIdx != -1)
        {
            const EntityID lastEntity = m_ActiveEntities[type].back();
            m_ActiveEntities[type][targetIdx] = lastEntity;
            m_EntityToIndex[type][lastEntity] = targetIdx;
            m_ActiveEntities[type].pop_back();
            m_EntityToIndex[type][entity] = -1;
        }
        ComponentTypeRegistry::ClearComponent(entity, ComponentType(static_cast<ComponentTypeID>(type)));
        TouchType(type);
    }

    GpuSceneChanges::Destroy(entity);
    m_Entities[entity].IsAlive = false;
    m_Entities[entity].Mask.reset();
    m_FreeList.push(entity);
    m_IsFree[entity] = 1;

    World::UnregisterName(entity);
}

