#pragma once
#include "rendergraph.h"
#include "gpuscene/gpuscenebuffer.h"
namespace MeshShaderRenderer
{
struct Settings
{
    bool Enabled = false, Frustum = true, Cone = true, Occlusion = true, Clusters = true;
    bool Colors = false, DebugReadback = false;
    bool MeshletBounds = false, ClusterBounds = false, ConeAxes = false;
    float Bias = 0.001f;
    uint32_t MaxMeshlets = 2000000;
};
Settings &GetSettings();
void QuerySupport(ID3D12Device *device);
bool Supported();
bool IsActive();
void Prepare(RenderGraph &graph, RenderGraph::ResourceHandle instances, RenderGraph::ResourceHandle objects,
             RenderGraph::ResourceHandle count, RenderGraph::ResourceHandle commands,
             ID3D12Resource *objectResource, ID3D12Resource *countResource, ID3D12Resource *commandsResource,
             uint32_t capacity, uint32_t meshes);
bool Draw(ID3D12GraphicsCommandList *commands, GpuSceneBuffer &globalVertices);
bool DrawVelocity(ID3D12GraphicsCommandList *commands, GpuSceneBuffer &globalVertices);
void DrawDebug();
void EndFrame();
ID3D12Resource *VisibleBuffer();
ID3D12Resource *StatisticsBuffer();
void Shutdown();
} // namespace MeshShaderRenderer
