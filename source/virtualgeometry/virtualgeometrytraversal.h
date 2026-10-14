#pragma once
#include "virtualgeometryresources.h"

namespace VirtualGeometryTraversal
{
struct Settings
{
    bool Enabled = true;
    float TargetErrorPixels = 1;
    int ForceHierarchyLevel = -1;
    bool FrustumCulling = true, ConeCulling = true, OcclusionCulling = true;
    bool LevelColors = false, DebugReadback = false;
    uint32_t MaxSelectedClusters = 2000000, MaxGroupStates = 2000000;
};
struct FrameInputs
{
    RenderGraph::ResourceHandle Instances, Objects, Count;
    ID3D12Resource* ObjectResource = nullptr;
    ID3D12Resource* CountResource = nullptr;
    uint32_t ObjectCapacity = 0;
};
Settings& GetSettings();
void Prepare(RenderGraph& graph, const FrameInputs& inputs);
bool Active();
GpuSceneBuffer& VisibleBuffer();
GpuSceneBuffer& CounterBuffer();
GpuSceneBuffer& ArgumentBuffer();
GpuSceneBuffer& SuccessBuffer();
const std::array<uint32_t, 64>& DebugCounters();
void EndFrame();
void Shutdown();
}
