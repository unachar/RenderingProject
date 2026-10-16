#pragma once
#include <d3d12.h>
#include <cstdint>
#include <vector>
#include "rendergraph.h"

struct StaticMeshData;
class RenderGraph;
namespace Meshlets { struct Asset; }

class GPUDrivenRenderer
{
public:
    struct OverflowStatistics
    {
        uint64_t VisibleInstanceOverflow = 0, InvalidIndirectCommands = 0;
        uint32_t VisibleInstances = 0;
        bool HasSample = false;
    };
    static const OverflowStatistics& GetOverflowStatistics();
    static void RegisterGeometry(uint32_t meshID, const StaticMeshData& mesh);
    static void RegisterPrimitiveGeometry(uint32_t meshID, const StaticMeshData& mesh,
                                          uint32_t lod = 0, uint32_t baseMeshID = UINT32_MAX);
    static void ReleasePrimitiveGeometry(uint32_t meshID);
    static uint64_t GeometryArenaBytes();
    static uint64_t GeometryUploadBytes();
    static void RegisterLODGeometry(uint32_t meshID, uint32_t baseMeshID, const vector<uint32_t>& indices, const Meshlets::Asset* meshlets = nullptr);
    static void UpdateGeometry(const StaticMeshData& mesh);
    static bool GeometryReady(uint32_t meshID);
    static bool SupportsEntity(uint32_t entity);
    static bool HandlesEntity(uint32_t entity);
    static void Prepare(RenderGraph& graph, RenderGraph::ResourceHandle instances, RenderGraph::ResourceHandle visibility,
                        uint32_t slots, uint32_t capacity, uint32_t meshes);
    static void Draw(ID3D12GraphicsCommandList* commands);
    static void DrawSecondPass(ID3D12GraphicsCommandList* commands);
    static void DrawVelocity(ID3D12GraphicsCommandList* commands);
    static void BuildHiZ(ID3D12GraphicsCommandList* commands);
    static bool IsEnabled();
    static bool GetDebugReadback();
    static void SetDebugReadback(bool enabled);
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
    static void TrackEntityTextures(uint32_t entity, const vector<uint32_t>& descriptors);
};

