#pragma once

#include "componentmanager.h"

class Sun
{
public:
	static EntityID CreateDefault();
	static EntityID Create(const XMFLOAT3& position, const XMFLOAT3& target);
	static void SyncAll();
	static void Sync(EntityID entity);
};
