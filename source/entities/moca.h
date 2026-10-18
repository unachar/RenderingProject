#pragma once
#include "entitybase.h"
#include "game.h"

class Moca : public EntityBase
{
  private:
    inline static const EntityRegistration<Moca> registration;

  public:
    void Create() override;
};
