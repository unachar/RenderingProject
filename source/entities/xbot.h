#pragma once
#include "entitybase.h"
#include "game.h"
class XBot : public EntityBase
{
private:
	inline static const EntityRegistration<XBot> registration;

public:
	void Create() override;
};

