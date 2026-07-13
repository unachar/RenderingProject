#pragma once
#include "systembase.h"
#include "ecs.h"
#include <vector>
#include <d3d12.h>

struct MaterialComponent;
class GpuDrivenIndirectDrawCache;

class ModelSystem : public SystemBase
{
private:
    struct AnimDrawCall
    {
        EntityID EntityID;
        ID3D12PipelineState* pso;
        int srvIndex;
        int normalSrvIndex;
        class AnimationModelResource* model;
        const MaterialComponent* material;
        float cameraDepth;
    };
    struct StaticDrawCall
    {
        EntityID EntityID;
        ID3D12PipelineState* pso;
        int srvIndex;
        int normalSrvIndex;
        class StaticModelResource* model;
        const MaterialComponent* material;
        float cameraDepth;
    };
    vector<AnimDrawCall> m_AnimDrawCalls;
    vector<StaticDrawCall> m_StaticDrawCalls;
    GpuDrivenIndirectDrawCache* m_IndirectDraws = nullptr;
public:
    void SetIndirectDrawCache(GpuDrivenIndirectDrawCache* cache) { m_IndirectDraws = cache; }
    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override;
};


