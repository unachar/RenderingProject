#pragma once
#include <iostream>
#include <vector>
#include <bitset>
#include <queue>
#include <unordered_map>
#include <string>
#include <typeindex>
#include <cstdint>
#include <algorithm>
#include <intrin.h>
#include "rendering/gpuscene/gpuscenechanges.h"
#include <windows.h>
#include <DirectXMath.h>
#include <wrl.h>

static constexpr uint32_t g_kMAX_ENTITIES = 16384;
static constexpr uint32_t g_kMAX_COMPONENTS = 32;
static constexpr uint32_t g_kINVALID_ENTITY = UINT32_MAX;

using EntityID = uint32_t;
using ComponentMask = bitset<g_kMAX_COMPONENTS>;
using ComponentTypeID = uint32_t;
using CachedEntityList = const vector<EntityID>*;

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
	static const ComponentType TIMELINE;
	static const ComponentType PROCEDURAL_TERRAIN;
	static const ComponentType PROCEDURAL_MOTION;

	ComponentTypeID Value = MAX;

	constexpr ComponentType() = default;
	constexpr ComponentType(ComponentTypeID value) : Value(value) {}
	constexpr operator ComponentTypeID() const { return Value; }
};

class ComponentTypeRegistry
{
private:
	static unordered_map<type_index, ComponentTypeID>& TypeIds();
	static vector<void(*)(EntityID)>& CreateCallbacks();
	static vector<void(*)(EntityID)>& ClearCallbacks();
	static vector<void(*)()>& ResetCallbacks();
	static ComponentTypeID& NextTypeId();

public:
	template<typename T>
	static ComponentType GetType();

	static void CreateComponent(EntityID entity, ComponentType type);
	static void ClearComponent(EntityID entity, ComponentType type);
	static void ResetComponents();
};


class EntityView
{
private:
	CachedEntityList m_Entities;
public:
	explicit EntityView(CachedEntityList entities) : m_Entities(entities) {}

	using const_iterator = vector<EntityID>::const_iterator;
	const_iterator begin() const
	{
		if (m_Entities)
		{
			return m_Entities->begin();
		}
		return Empty().begin();
	}
	const_iterator end() const
	{
		if (m_Entities)
		{
			return m_Entities->end();
		}
		return Empty().end();
	}
	size_t size() const
	{
		if (m_Entities)
		{
			return m_Entities->size();
		}
		return 0;
	}
	bool empty() const { return size() == 0; }

private:
	static const vector<EntityID>& Empty()
	{
		static const vector<EntityID> empty;
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
	static vector<EntityData> m_Entities;
	static queue<EntityID> m_FreeList;
	static vector<uint8_t> m_IsFree;          // 1 = FreeListに入っていて再利用可能（RestoreEntityでO(1)取り消し）
	static uint32_t m_NextEntityId;
	static vector<EntityID> m_ActiveEntities[g_kMAX_COMPONENTS];
	static vector<int32_t> m_EntityToIndex[g_kMAX_COMPONENTS];
	static uint64_t m_StructureVersion;       // 単調増加カウンター
	static uint64_t m_TypeVersion[g_kMAX_COMPONENTS]; // 型ごとの最終変更番号

	static void TouchType(ComponentTypeID type)
	{
		m_TypeVersion[type] = ++m_StructureVersion;
	}

	// マスクに含まれる型の最終変更番号の最大値。どれか1つでも変わればこの値も変わる。
	static uint64_t MaskStamp(uint32_t mask)
	{
		uint64_t stamp = 0;
		while (mask != 0)
		{
			unsigned long type = 0;
			_BitScanForward(&type, mask);
			mask &= mask - 1;
			stamp = (std::max)(stamp, m_TypeVersion[type]);
		}
		return stamp;
	}

	// 複数型のクエリ結果。
	// ・関係する型が変わった時だけ作り直す（無関係なコンポーネントの追加・削除では作り直さない）
	// ・結果はEntityIDの昇順。追加・削除の履歴（swap-remove）に依存しない決定論的な反復順になる
	// ・二重バッファ：反復中に同じクエリが作り直されても、直前に返した配列は壊さない
	static CachedEntityList GetCachedEntities(uint32_t requiredMask)
	{
		struct QueryCacheEntry
		{
			uint64_t Stamp = UINT64_MAX;
			uint32_t Current = 0;
			vector<EntityID> Buffers[2];
		};
		static unordered_map<uint32_t, QueryCacheEntry> queryCache;
		if (queryCache.empty())
		{
			queryCache.reserve(64);
		}

		QueryCacheEntry& entry = queryCache[requiredMask];
		const uint64_t stamp = MaskStamp(requiredMask);
		if (entry.Stamp == stamp)
		{
			return &entry.Buffers[entry.Current];
		}

		// 最も要素数の少ない型の一覧を起点にする
		uint32_t baseType = g_kMAX_COMPONENTS;
		size_t baseSize = SIZE_MAX;
		for (uint32_t bits = requiredMask; bits != 0; bits &= bits - 1)
		{
			unsigned long type = 0;
			_BitScanForward(&type, bits);
			if (m_ActiveEntities[type].size() < baseSize)
			{
				baseSize = m_ActiveEntities[type].size();
				baseType = type;
			}
		}

		entry.Current ^= 1u;
		auto& result = entry.Buffers[entry.Current];
		result.clear();
		if (baseType < g_kMAX_COMPONENTS)
		{
			const auto& activeList = m_ActiveEntities[baseType];
			result.reserve(activeList.size());
			const ComponentMask required(requiredMask);
			for (EntityID entity : activeList)
			{
				const EntityData& data = m_Entities[entity];
				if (data.IsAlive && (data.Mask & required) == required)
				{
					result.push_back(entity);
				}
			}
			sort(result.begin(), result.end());
		}
		entry.Stamp = stamp;
		return &result;
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
		uint32_t requiredMask = 0;
		((requiredMask |= (types.Value < g_kMAX_COMPONENTS ? (1u << types.Value) : 0u)), ...);
		if (requiredMask == 0)
		{
			return EntityView(nullptr);
		}
		return EntityView(GetCachedEntities(requiredMask));
	}

	static void AddComponent(EntityID entity, ComponentType type)
	{
		if (entity < g_kMAX_ENTITIES && type.Value < g_kMAX_COMPONENTS &&
			m_Entities[entity].IsAlive && !m_Entities[entity].Mask.test(type))
		{
			ComponentTypeRegistry::CreateComponent(entity, type);
			m_Entities[entity].Mask.set(type);
			m_ActiveEntities[type].push_back(entity);
			m_EntityToIndex[type][entity] = static_cast<int32_t>(m_ActiveEntities[type].size() - 1);
			TouchType(type);
			GpuSceneChanges::Notify(entity);
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
			TouchType(type);
			GpuSceneChanges::Notify(entity);
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

