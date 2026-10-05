#include "pch.h"
#include "componentmanager.h"

bool ComponentManager::HasComponent(EntityID EntityID, ComponentType type)
{
	return Registry::HasComponent(EntityID, type);
}

void ComponentManager::Init()
{
}

void ComponentManager::Uninit()
{
}

void ComponentManager::ClearEntity(EntityID entity)
{
	if (entity >= g_kMAX_ENTITIES)
	{
		return;
	}

	for (ComponentTypeID type = 0; type < ComponentTypeRegistry::GetRegisteredCount(); ++type)
	{
		if (Registry::HasComponent(entity, ComponentType(type)))
		{
			ComponentTypeRegistry::ClearComponent(entity, ComponentType(type));
		}
	}
}

void ComponentManager::ReportMissingComponentError(EntityID entity, const char* componentName)
{
	wchar_t buf[1024];
	wstring entityName = L"Unregistered ";

	if (entity < g_kMAX_ENTITIES && HasComponent(entity, ComponentType::NAME))
	{
		string n = ComponentStorage<NameComponent>::Get(entity).Name;
		int len = MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1, NULL, 0);
		if (len > 0)
		{
			vector<wchar_t> w(len);
			MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1, w.data(), len);
			entityName = w.data();
		}
	}

	wchar_t wCompName[256];
	MultiByteToWideChar(CP_UTF8, 0, componentName, -1, wCompName, 256);

	swprintf_s(buf, L"Error: EntityID [%d] (Name: %s) does not have component [%s].\nPlease check whether the target entity has this component.",
		entity, entityName.c_str(), wCompName);

	MessageBoxW(nullptr, buf, L"ECS Component Missing Error", MB_OK | MB_ICONERROR);
}

