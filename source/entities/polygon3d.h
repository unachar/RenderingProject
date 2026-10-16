#pragma once
#include "entitybase.h"
#include "game.h"
class Polygon3D : public EntityBase
{
private:
	inline static const EntityRegistration<Polygon3D> registration;

public: 
	void Create() override;
};
