#pragma once
#include "systembase.h"

// Runs after Physics/Camera/Transform and before consumers of the skinned vertex buffer.
class LookAtSystem : public SystemBase
{
  public:
    void Update() override;
};
