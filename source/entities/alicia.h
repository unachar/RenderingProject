#pragma once
#include "entitybase.h"
#include "game.h"
class Alicia : public EntityBase
{
private:
	inline static const EntityRegistration<Alicia> registration;

public:
	void Create() override;
};
