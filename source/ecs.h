#pragma once
#include <iostream>
#include <vector>
#include <bitset>
#include <queue>
#include <unordered_map>
#include <string>
#include <typeindex>
#include <memory>
#include <cstdint>
#include <windows.h>
#include <DirectXMath.h>
#include <wrl.h>

static constexpr uint32_t g_kMAX_ENTITIES = 2048;
static constexpr uint32_t g_kMAX_COMPONENTS = 32;
static constexpr uint32_t g_kINVALID_ENTITY = UINT32_MAX;

using EntityID = uint32_t;
using ComponentMask = std::bitset<g_kMAX_COMPONENTS>;
using ComponentTypeID = uint32_t;
using CachedEntityList = std::shared_ptr<const std::vector<EntityID>>;

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
	static const ComponentType SUN;
	static const ComponentType INSTANCING;
	static const ComponentType LOD;

	ComponentTypeID Value = MAX;

	constexpr ComponentType() = default;
	constexpr ComponentType(ComponentTypeID value) : Value(value) {}
	constexpr operator ComponentTypeID() const { return Value; }
};

class ComponentTypeRegistry
{
private:
	static std::unordered_map<std::type_index, ComponentTypeID>& TypeIds();
	static std::vector<void(*)(EntityID)>& ClearCallbacks();
	static ComponentTypeID& NextTypeId();

public:
	template<typename T>
	static ComponentType GetType();

	static void ClearComponent(EntityID entity, ComponentType type);
	static ComponentTypeID GetRegisteredCount();
};

// Retained for source compatibility with code that explicitly uses the old
// iterator. EntityView itself now iterates a cached, tightly packed result list.
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
	CachedEntityList m_Entities;
public:
	explicit EntityView(CachedEntityList entities) : m_Entities(std::move(entities)) {}

	using const_iterator = std::vector<EntityID>::const_iterator;
	const_iterator begin() const { return m_Entities ? m_Entities->begin() : Empty().begin(); }
	const_iterator end() const { return m_Entities ? m_Entities->end() : Empty().end(); }
	size_t size() const { return m_Entities ? m_Entities->size() : 0; }
	bool empty() const { return size() == 0; }

private:
	static const std::vector<EntityID>& Empty()
	{
		static const std::vector<EntityID> empty;
		return empty;
	}
};

struct EntityData
{
	bool IsAlive = false;
	ComponentMask Mask;
};

class Registry
{
private:
	static std::vector<EntityData> m_Entities;
	static std::queue<EntityID> m_FreeList;
	static uint32_t m_NextEntityId;
	static std::vector<EntityID> m_ActiveEntities[g_kMAX_COMPONENTS];
	static std::vector<int32_t> m_EntityToIndex[g_kMAX_COMPONENTS];
	static uint64_t m_StructureVersion;

	static void TouchStructure()
	{
		++m_StructureVersion;
		if (m_StructureVersion == 0)
		{
			m_StructureVersion = 1;
		}
	}

	static CachedEntityList GetCachedEntities(const ComponentMask& requiredMask, ComponentType baseType)
	{
		struct QueryCacheEntry
		{
			uint64_t Version = 0;
			CachedEntityList Entities;
		};

		static std::unordered_map<uint64_t, QueryCacheEntry> queryCache;
		const uint64_t key =
			(requiredMask.to_ullong() << 6) ^ static_cast<uint64_t>(baseType.Value);
		QueryCacheEntry& entry = queryCache[key];

		if (entry.Version == m_StructureVersion && entry.Entities)
		{
			return entry.Entities;
		}

		auto result = std::make_shared<std::vector<EntityID>>();
		if (baseType.Value < g_kMAX_COMPONENTS)
		{
			const auto& activeList = m_ActiveEntities[baseType.Value];
			result->reserve(activeList.size());
			for (EntityID entity : activeList)
			{
				if (entity >= m_Entities.size())
				{
					continue;
				}

				const EntityData& data = m_Entities[entity];
				if (data.IsAlive && (data.Mask & requiredMask) == requiredMask)
				{
					result->push_back(entity);
				}
			}
		}

		entry.Version = m_StructureVersion;
		entry.Entities = result;
		return entry.Entities;
	}

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

	static const std::vector<EntityData>& GetEntities()
	{
		return m_Entities;
	}
	static const std::vector<EntityID>& GetActiveEntities(ComponentType type)
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
		return EntityView(GetCachedEntities(requiredMask, bestType));
	}

	static void AddComponent(EntityID entity, ComponentType type)
	{
		if (entity < g_kMAX_ENTITIES && type.Value < g_kMAX_COMPONENTS && !m_Entities[entity].Mask.test(type))
		{
			m_Entities[entity].Mask.set(type);
			m_ActiveEntities[type].push_back(entity);
			m_EntityToIndex[type][entity] = static_cast<int32_t>(m_ActiveEntities[type].size() - 1);
			TouchStructure();
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
			TouchStructure();
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
