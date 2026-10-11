#pragma once
#include "entitybase.h"
#include "game.h"
class Sky : public EntityBase
{
private:
	inline static const EntityRegistration<Sky> registration;

public:
	void Create() override;
};