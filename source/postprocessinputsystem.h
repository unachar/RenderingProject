#pragma once
#include "systembase.h"
#include "ecs.h"

class PostProcessInputSystem : public SystemBase
{
public:
    void Update() override;
};
