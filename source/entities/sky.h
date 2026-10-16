#pragma once
#include "entitybase.h"
#include "game.h"
// The physical atmosphere supplies the startup sky. Legacy environment meshes may still be loaded explicitly.
class Sky : public EntityBase
{
public:
	void Create() override;
};

