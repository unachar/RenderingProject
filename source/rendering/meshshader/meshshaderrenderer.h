#pragma once
#include "rendergraph.h"
#include "gpuscene/gpuscenebuffer.h"
#include <array>
#include <vector>

namespace MeshShaderRenderer
{
    struct Settings
    {
        bool Enabled = true, Frustum = true, Cone = true, Occlusion = true, Clusters = true;
        bool Colors = false, DebugReadback = false;
        bool MeshletBounds = false, ClusterBounds = false, ConeAxes = false;
        float Bias = 0.001f;
        uint32_t MaxMeshlets = 2000000;
    };

    Settings& GetSettings();
    const std::vector<std::array<uint32_t, 4>>& DebugRecords();
    void QuerySupport(ID3D12Device* device);
    bool Supported();
    bool IsActive();
    bool NativeReady();
    void Prepare(RenderGraph& graph, RenderGraph::ResourceHandle instances, RenderGraph::ResourceHandle objects,
                 RenderGraph::ResourceHandle count, RenderGraph::ResourceHandle commands,
                 ID3D12Resource* objectResource, ID3D12Resource* countResource, ID3D12Resource* commandsResource,
                 uint32_t capacity, uint32_t meshes);
    bool Draw(ID3D12GraphicsCommandList* commands, GpuSceneBuffer& globalVertices);
    bool DrawSecondPass(ID3D12GraphicsCommandList* commands, GpuSceneBuffer& globalVertices);
    bool DrawVelocity(ID3D12GraphicsCommandList* commands, GpuSceneBuffer& globalVertices);
    // Draws the previous frame's VG shadow traversal result into the currently
    // bound shadow map (depth only). Call inside a shadow pass; no-op unless
    // the VG shadow traversal is enabled and produced a view.
    bool DrawShadow(ID3D12GraphicsCommandList* commands);
    void DrawDebug();
    void EndFrame();
    ID3D12Resource* VisibleBuffer();
    ID3D12Resource* StatisticsBuffer();

    struct NativeRoutingSample
    {
        uint32_t SelectedClusters = 0, LeafClusters = 0, SuccessfulInstances = 0;
        std::string Error;
    };

    // 同期的な検証キャプチャ。通常の描画では呼ばない。
    NativeRoutingSample CaptureNativeRouting();
    void Shutdown();
} // 名前空間 MeshShaderRenderer
