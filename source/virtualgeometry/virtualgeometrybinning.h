#pragma once
#include "virtualgeometrytraversal.h"
#include <array>
#include <string>

namespace VirtualGeometryBinning
{
    struct IndirectCommand
    {
        uint32_t FirstCluster = 0;
        uint32_t ClusterCount = 0;
        D3D12_DISPATCH_MESH_ARGUMENTS Dispatch{};
    };

    static_assert(sizeof(IndirectCommand) == 20);

    struct Settings
    {
        bool Enabled = true;
        uint32_t MaxMaterialBins = 4096;
    };

    struct PhaseStatistics
    {
        uint32_t ActiveBins = 0;
        uint32_t Clusters = 0;
        uint32_t OverflowClusters = 0;
        uint32_t InvalidMaterials = 0;
        uint32_t RejectedClusters = 0;
        bool HasSample = false;
    };

    struct Statistics
    {
        std::array<PhaseStatistics, 3> Phases;
        uint64_t BufferBytes = 0;
    };

    Settings& GetSettings();
    const Statistics& GetStatistics();
    const std::string& UnavailableReason();
    void BeginFrame(bool rasterAvailable);
    bool Active();
    void Prepare(RenderGraph& graph, VirtualGeometryTraversal::RasterPhase phase);
    GpuSceneBuffer& VisibleBuffer();
    GpuSceneBuffer& ArgumentBuffer();
    GpuSceneBuffer& CounterBuffer();
    uint32_t MaximumCommands();
    void Shutdown();
} // 名前空間 VirtualGeometryBinning の終わり。
