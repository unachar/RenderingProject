#pragma once
#include "systembase.h"
#include "ecs.h"

class MovementSystem : public SystemBase
{
public:
    void Update() override;
};
