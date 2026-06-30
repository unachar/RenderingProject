#pragma once
#include "systembase.h"

class GridSystem : public SystemBase
{
public:
    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override;
};



