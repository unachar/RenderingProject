#pragma once
#include <iostream>
#include <vector>
#include <bitset>
#include <queue>
#include <unordered_map>
#include <string>
#include <typeindex>
#include <windows.h>
#include <DirectXMath.h>
#include <wrl.h>

static constexpr uint32_t g_kMAX_ENTITIES = 2048;
static constexpr uint32_t g_kMAX_COMPONENTS = 32;
static constexpr uint32_t g_kINVALID_ENTITY = UINT32_MAX;

using EntityID = uint32_t;
using ComponentMask = bitset<g_kMAX_COMPONENTS>;
using ComponentTypeID = uint32_t;

class Entity;
class Registry;

struct ComponentType
{
	static constexpr ComponentTypeID MAX = g_kMAX_COMPONENTS;
	static const ComponentType TRANSFORM;
	static const ComponentType MESH;
	static const ComponentType INPUT_CTRL;
	static const ComponentType AABB;
	static const ComponentType OBB;
	static const ComponentType CAMERA;
	static const ComponentType ANIMATION_MODEL;
	static const ComponentType SHADER;
	static const ComponentType STATIC_MODEL;
	static const ComponentType MATERIAL;
	static const ComponentType PHYSICS;
	static const ComponentType POST_PROCESS;
	static const ComponentType NAME;
	static const ComponentType MOVE;
	static const ComponentType SPRITE;
	static const ComponentType LIGHT;

	ComponentTypeID Value = MAX;

	constexpr ComponentType() = default;
	constexpr ComponentType(ComponentTypeID value) : Value(value) {}
	constexpr operator ComponentTypeID() const { return Value; }
};

class ComponentTypeRegistry
{
private:
	static unordered_map<type_index, ComponentTypeID>& TypeIds();
	static vector<void(*)(EntityID)>& ClearCallbacks();
	static ComponentTypeID& NextTypeId();

public:
	template<typename T>
	static ComponentType GetType();

	static void ClearComponent(EntityID entity, ComponentType type);
	static ComponentTypeID GetRegisteredCount();
};

class EntityIterator
{
private:
	uint32_t m_VectorIndex;
	ComponentType m_BaseType;
	ComponentMask m_Mask;
public:
	EntityIterator(uint32_t index, ComponentType baseType, ComponentMask mask);
	void Advance();
	bool operator!=(const EntityIterator& other) const { return m_VectorIndex != other.m_VectorIndex; }
	bool operator==(const EntityIterator& other) const { return m_VectorIndex == other.m_VectorIndex; }
	EntityIterator& operator++();
	EntityID operator*() const;
};

class EntityView
{
private:
	ComponentMask m_Mask;
	ComponentType m_BaseType;
public:
	EntityView(ComponentMask mask, ComponentType baseType) : m_Mask(mask), m_BaseType(baseType) {}
	EntityIterator begin() const;
	EntityIterator end() const;
	size_t size() const;
	bool empty() const { return begin() == end(); }
};

struct EntityData
{
	bool IsAlive = false;
	ComponentMask Mask;
};

class Registry
{
private:
	static vector<EntityData> m_Entities;
	static queue<EntityID> m_FreeList;
	static uint32_t m_NextEntityId;
	static vector<EntityID> m_ActiveEntities[g_kMAX_COMPONENTS];
	static vector<int32_t> m_EntityToIndex[g_kMAX_COMPONENTS];

public:
	static void Init();

	static bool IsAlive(EntityID entity)
	{
		if (entity >= g_kMAX_ENTITIES) return false;
		return m_Entities[entity].IsAlive;
	}

	static EntityID CreateEntity();
	static bool RestoreEntity(EntityID entity);
	static void DestroyEntity(EntityID entity);

	static const vector<EntityData>& GetEntities()
	{
		return m_Entities;
	}
	static const vector<EntityID>& GetActiveEntities(ComponentType type)
	{
		return m_ActiveEntities[type];
	}

	template<typename... ComponentTypes>
	static EntityView GetEntitiesByType(ComponentTypes... types)
	{
		ComponentMask requiredMask;
		(requiredMask.set(types), ...);
		ComponentType bestType = ComponentType::MAX;
		size_t bestSize = SIZE_MAX;
		if constexpr (sizeof...(ComponentTypes) > 0)
		{
			ComponentType typeArr[] = { types... };
			for (size_t i = 0; i < sizeof...(ComponentTypes); i++)
			{
				size_t listSize = m_ActiveEntities[typeArr[i]].size();
				if (listSize < bestSize)
				{
					bestSize = listSize;
					bestType = typeArr[i];
				}
			}
		}
		return EntityView(requiredMask, bestType);
	}

	static void AddComponent(EntityID entity, ComponentType type)
	{
		if (entity < g_kMAX_ENTITIES && type.Value < g_kMAX_COMPONENTS && !m_Entities[entity].Mask.test(type))
		{
			m_Entities[entity].Mask.set(type);
			m_ActiveEntities[type].push_back(entity);
			m_EntityToIndex[type][entity] = static_cast<int32_t>(m_ActiveEntities[type].size() - 1);
		}
	}

	template<typename... ComponentTypes>
	static void AddComponent(EntityID entity, ComponentTypes... types)
	{
		(AddComponent(entity, types), ...);
	}

	static void RemoveComponent(EntityID entity, ComponentType type)
	{
		if (entity < g_kMAX_ENTITIES && type.Value < g_kMAX_COMPONENTS && m_Entities[entity].Mask.test(type))
		{
			ComponentTypeRegistry::ClearComponent(entity, type);
			m_Entities[entity].Mask.reset(type);
			int32_t targetIdx = m_EntityToIndex[type][entity];
			if (targetIdx != -1)
			{
				EntityID lastEntity = m_ActiveEntities[type].back();
				m_ActiveEntities[type][targetIdx] = lastEntity;
				m_EntityToIndex[type][lastEntity] = targetIdx;
				m_ActiveEntities[type].pop_back();
				m_EntityToIndex[type][entity] = -1;
			}
		}
	}

	template<typename... ComponentTypes>
	static void RemoveComponent(EntityID entity, ComponentTypes... types)
	{
		(RemoveComponent(entity, types), ...);
	}

	static bool HasComponent(EntityID entity, ComponentType type)
	{
		if (entity >= g_kMAX_ENTITIES || type.Value >= g_kMAX_COMPONENTS) return false;
		return m_Entities[entity].Mask.test(type);
	}
};

inline EntityIterator::EntityIterator(uint32_t index, ComponentType baseType, ComponentMask mask)
	: m_VectorIndex(index), m_BaseType(baseType), m_Mask(mask)
{
	Advance();
}

inline void EntityIterator::Advance()
{
	if (m_BaseType.Value == ComponentType::MAX) return;
	const auto& activeList = Registry::GetActiveEntities(m_BaseType);
	while (m_VectorIndex < activeList.size()) {
		EntityID entity = activeList[m_VectorIndex];
		const auto& entityData = Registry::GetEntities()[entity];
		if (entityData.IsAlive && (entityData.Mask & m_Mask) == m_Mask) break;
		m_VectorIndex++;
	}
}

inline EntityIterator& EntityIterator::operator++()
{
	m_VectorIndex++;
	Advance();
	return *this;
}

inline EntityID EntityIterator::operator*() const
{
	if (m_BaseType.Value == ComponentType::MAX) return g_kINVALID_ENTITY;
	return Registry::GetActiveEntities(m_BaseType)[m_VectorIndex];
}

inline EntityIterator EntityView::begin() const
{
	return EntityIterator(0, m_BaseType, m_Mask);
}

inline EntityIterator EntityView::end() const
{
	if (m_BaseType.Value == ComponentType::MAX)
	{
		return EntityIterator(0, m_BaseType, m_Mask);
	}

	return EntityIterator(static_cast<uint32_t>(Registry::GetActiveEntities(m_BaseType).size()), m_BaseType, m_Mask);
}

inline size_t EntityView::size() const
{
	if (m_BaseType.Value == ComponentType::MAX) return 0;
	return Registry::GetActiveEntities(m_BaseType).size();
}

