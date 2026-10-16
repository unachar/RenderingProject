#pragma once
#include "entitybase.h"
#include "game.h"
class KacchattaHone : public EntityBase
{
private:
	inline static const EntityRegistration<KacchattaHone> registration;

public:
	void Create() override;
};

