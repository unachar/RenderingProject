#pragma once

#include "componentmanager.h"

class Sun
{
public:
	static EntityID CreateDefault();
	static EntityID Create(const XMFLOAT3& position, const XMFLOAT3& target);
	// Call before recording a frame, never from an Inspector or a draw callback.
	static bool MigrateLegacyGeometryBeforeFrame();
	static void SyncAll();
	static void Sync(EntityID entity);
};
