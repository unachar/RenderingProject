#pragma once
#include "systembase.h"

class PostProcessSystem : public SystemBase
{
public:
    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override;
};


