#pragma once
#include "systembase.h"
#include "ecs.h"

class LightSystem : public SystemBase
{
  public:
    void Init() override;
    void Update() override;
};
