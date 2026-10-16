#pragma once

#include <vector>
#include <unordered_map>
#include <typeindex>
#include <typeinfo>
#include <memory>
#include "ecs.h"
#include "systembase.h"

class SystemManager
{
private:
	inline static vector<unique_ptr<SystemBase>> m_Systems;
	inline static unordered_map<type_index, SystemBase*> m_SystemMap;
	inline static vector<uint8_t> m_PlayOnly; // m_Systemsと同じ並び。Init時に1回だけ判定

public:
	static bool Init();
	static void Uninit();

	template<typename T>
	static T* GetSystem()
	{
		auto it = m_SystemMap.find(type_index(typeid(T)));
		if (it == m_SystemMap.end())
		{
			return nullptr;
		}
		return static_cast<T*>(it->second);
	}

	static void UpdateSystem();
	static void DrawSystem(RenderPass renderPass, bool receivingPostProcessOnly);
	static void RenderFlow();
};
