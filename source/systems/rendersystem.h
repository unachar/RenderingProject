#pragma once
#include "systembase.h"
#include "ecs.h"
#include <vector>
#include <d3d12.h>

struct MaterialComponent;

class RenderSystem : public SystemBase
{
private:
    struct DrawCall
    {
        EntityID EntityID;
        ID3D12PipelineState* pso;
        int srvIndex;
        int normalSrvIndex;
        D3D12_VERTEX_BUFFER_VIEW vbv;
        UINT vertexCount;
        bool is3D;
        const MaterialComponent* material;
        float cameraDepth;
    };
    vector<DrawCall> m_SpriteDrawCalls;
    vector<DrawCall> m_ModelDrawCalls;

public:
    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override;
};


