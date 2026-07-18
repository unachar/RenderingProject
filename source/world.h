#pragma once
#include "componentmanager.h"
#include <type_traits>

class Entity
{
private:
	EntityID m_Id;

public:
	Entity(EntityID id) : m_Id(id) {}

	EntityID GetID() const { return m_Id; }
	bool IsValid() const { return m_Id < g_kMAX_ENTITIES && Registry::IsAlive(m_Id); }

	template<typename T>
	Entity& Add()
	{
		ComponentManager::AddComponent(m_Id, ComponentTypeTraits<T>::value());
		return *this;
	}

    template<typename T>
	T& Get()
	{
		return ComponentManager::GetComponent<T>(m_Id);
	}

	template<typename T>
	const T& Get() const
	{
		return ComponentManager::GetComponent<T>(m_Id);
	}

	template<typename T>
	bool Has() const
	{
		return ComponentManager::HasComponent(m_Id, ComponentTypeTraits<T>::value());
	}

	Entity& SetName(const string& name);

	bool operator==(const Entity& other) const { return m_Id == other.m_Id; }
	bool operator!=(const Entity& other) const { return m_Id != other.m_Id; }
};

class World
{
private:
	static float m_DeltaTime;
	static float m_RawDeltaTime;
	static float m_SmoothedDeltaTime;
	static float m_FrameRate;
	static float m_FrameTimeMs;
	static double m_FrameStatsAccumulator;
	static int m_FrameStatsCount;
	static long long m_LastTime;
	static long long m_Frequency;
	static bool m_VSyncEnabled;
	static bool m_FixedFrameRateEnabled;
	static int m_TargetFrameRate;
	static unordered_map<string, EntityID> m_NameMap;
	static unordered_map<EntityID, string> m_IdToNameMap;

public:
	static void Init()
	{
		Registry::Init();
	}

	static void RegisterName(EntityID entity, const string& name);
	static void UnregisterName(EntityID entity);

	static Entity CreateEntity()
	{
		EntityID id = Registry::CreateEntity();
		Entity entity(id);
		return entity;
	}

	static Entity GetEntityByName(const string& name);

	static void DestroyEntity(Entity entity)
	{
		if (!entity.IsValid()) return;
		UnregisterName(entity.GetID());
		Registry::DestroyEntity(entity.GetID());
	}

	template<typename... T>
	static EntityView GetView()
	{
		return Registry::GetEntitiesByType(ComponentTypeTraits<T>::value()...);
	}

	static void Update();
	static void WaitForFrameLimit();
	static float GetDeltaTime();
	static float GetRawDeltaTime();
	static float GetFrameRate();
	static float GetFrameTimeMs();
	static bool IsVSyncEnabled();
	static void SetVSyncEnabled(bool enabled);
	static bool IsFixedFrameRateEnabled();
	static void SetFixedFrameRateEnabled(bool enabled);
	static int GetTargetFrameRate();
	static void SetTargetFrameRate(int fps);
};

inline Entity& Entity::SetName(const string& name)
{
	if (!Has<NameComponent>())
	{
		return *this;
	}
	Get<NameComponent>().Name = name;
	World::RegisterName(m_Id, name);
	return *this;
}

