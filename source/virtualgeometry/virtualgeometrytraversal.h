#pragma once
#include "virtualgeometryresources.h"

namespace VirtualGeometryTraversal
{
enum class DebugView : uint32_t
{
    None,
    LOD,
    Cluster,
    Hierarchy,
    MaterialBin,
    Compression,
    PageResidency,
    StreamingPriority,
    Occlusion,
    Overdraw,
    GeometryError,
    ObjectID
};

struct Settings
{
    bool Enabled = true;
    float TargetErrorPixels = 1;
    bool HysteresisEnabled = true, TwoPassOcclusion = true;
    float HysteresisFraction = 0.1f;
    int ForceHierarchyLevel = -1;
    bool FrustumCulling = true, ConeCulling = true, OcclusionCulling = true;
    bool LevelColors = false, DebugReadback = false;
    DebugView View = DebugView::None;
    bool AutoGrowBuffers = true;
    bool PredictiveStreaming = true;
    float PrefetchSeconds = 0.25f, MaxPrefetchDistance = 50.0f;
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
enum class RasterPhase
{
    First,
    Second,
    Combined
};
bool TwoPassActive();
void InvalidateHistory();
GpuSceneBuffer& VisibleBuffer(RasterPhase phase);
GpuSceneBuffer& CounterBuffer(RasterPhase phase);
GpuSceneBuffer& ArgumentBuffer(RasterPhase phase);
uint64_t CounterOffset(RasterPhase phase);
uint64_t ArgumentOffset(RasterPhase phase);
void Retest(ID3D12GraphicsCommandList* commands);
void RecordFeedback(RenderGraph& graph);

struct TemporalStatistics
{
    uint64_t SampleSerial = 0;
    uint32_t FirstVisible = 0, Candidates = 0, SecondVisible = 0, LodTransitions = 0, CapacityFallbacks = 0;
    uint64_t HistoryBytes = 0, CandidateBytes = 0;
    bool HasSample = false;
};

const TemporalStatistics& GetTemporalStatistics();
GpuSceneBuffer& VisibleBuffer();
GpuSceneBuffer& CounterBuffer();
GpuSceneBuffer& ArgumentBuffer();
GpuSceneBuffer& SuccessBuffer();
const std::array<uint32_t, 64>& DebugCounters();
void EndFrame();
void Shutdown();
} // namespace VirtualGeometryTraversal
