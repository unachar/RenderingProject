#pragma once
#include "entitybase.h"
#include "game.h"
class Field : public EntityBase
{
private:
	inline static const EntityRegistration<Field> registration;

public:
	void Create() override;
};
