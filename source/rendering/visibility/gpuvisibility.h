#pragma once
#include "rendergraph.h"
#include "visibilitytypes.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>

struct StaticMeshData;

namespace GPUVisibility
{
    struct Settings
    {
        bool Frustum = true, Occlusion = true, LOD = true;
        bool DebugReadback = false, Bounds = false, ScreenRectangles = false;
        bool FreezeCamera = false;
        float DepthBias = 0.001f, LODBias = 0;
        int ForceLOD = -1, DebugMode = 0, HiZMip = 0;
    };

    Settings& GetSettings();
    DirectX::XMFLOAT4X4 CullingViewProjection();
    vector<uint32_t> LODMeshes(uint32_t group);
    uint32_t RegisterLODGroup(const array<uint32_t, Visibility::MaxLODs>& meshes,
                              const Visibility::LODMetadata& metadata);
    void Invalidate(const char* reason);
    void PrepareCamera();
    RenderGraph::ResourceHandle Prepare(RenderGraph& graph, RenderGraph::ResourceHandle instances,
                                        RenderGraph::ResourceHandle frustum, RenderGraph::ResourceHandle geometry,
                                        ID3D12Resource* geometryResource, uint32_t slots, uint32_t capacity,
                                        uint32_t meshCount);
    ID3D12Resource* ResultBuffer();
    ID3D12Resource* StatisticsBuffer();
    const Visibility::Statistics& LatestStatistics();
    bool HasStatisticsSample();
    bool HistoryValid();
    bool CameraCut();
    void DrawDebug();
    void Shutdown();
} // 名前空間 GPUVisibility
