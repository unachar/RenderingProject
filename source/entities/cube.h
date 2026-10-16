#pragma once
#include "entitybase.h"
#include "game.h"
class Cube : public EntityBase
{
private:
	inline static const EntityRegistration<Cube> registration;

public:
	void Create() override;
};
