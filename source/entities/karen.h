#pragma once
#include "entitybase.h"
#include "game.h"
class Karen : public EntityBase
{
private:
	inline static const EntityRegistration<Karen> registration;

public:
	void Create() override;
};
