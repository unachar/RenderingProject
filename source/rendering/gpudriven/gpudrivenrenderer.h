#pragma once
#include <d3d12.h>
#include <cstdint>
#include <vector>
#include "rendergraph.h"

struct StaticMeshData;
class RenderGraph;

class GPUDrivenRenderer
{
public:
    static void RegisterGeometry(uint32_t meshID, const StaticMeshData& mesh);
    static void UpdateGeometry(const StaticMeshData& mesh);
    static bool GeometryReady(uint32_t meshID);
    static bool SupportsEntity(uint32_t entity);
    static bool HandlesEntity(uint32_t entity);
    static void Prepare(RenderGraph& graph, RenderGraph::ResourceHandle instances, RenderGraph::ResourceHandle visibility,
                        uint32_t slots, uint32_t capacity, uint32_t meshes);
    static void Draw(ID3D12GraphicsCommandList* commands);
    static bool IsEnabled();
    static bool IsActive();
    static void SetEnabled(bool enabled);
    static void DrawDebug();
    static void EndFrame(); // Optional debug readback, after submission and before frame-index rotation.
    static void Shutdown();
    static ID3D12Resource* CommandBuffer();
    static ID3D12Resource* CountBuffer();
    static ID3D12Resource* VisibleBuffer();
    static ID3D12Resource* GeometryBuffer();
    static uint32_t CommandCapacity();
    static double SubmissionMilliseconds();
    static uint32_t ExecuteCalls();
    static void RecordOpaqueSubmission(double milliseconds);
    static void TrackEntityTextures(uint32_t entity, const std::vector<uint32_t>& descriptors);
};
