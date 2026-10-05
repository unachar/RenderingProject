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
