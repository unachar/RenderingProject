#pragma once
#include "systembase.h"
#include "ecs.h"

class CameraControlSystem : public SystemBase
{
public:
    void Update() override;
};
