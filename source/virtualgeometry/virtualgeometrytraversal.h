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
        bool ShadowTraversal = false;
        bool LevelColors = false, DebugReadback = false;
        DebugView View = DebugView::None;
        bool AutoGrowBuffers = true;
        bool PredictiveStreaming = true;
        float PrefetchSeconds = 0.25f;
        float MaxPrefetchDistance = 50.0f;
        uint32_t MaxSelectedClusters = 2000000;
        uint32_t MaxGroupStates = 2000000;
    };

    struct FrameInputs
    {
        RenderGraph::ResourceHandle Instances, Objects, Count;
        ID3D12Resource* ObjectResource = nullptr;
        ID3D12Resource* CountResource = nullptr;
        uint32_t ObjectCapacity = 0;
    };

    // One view's traversal inputs. The camera path fills this from the active
    // camera; a shadow view fills it from the light's view/projection, so the
    // traversal no longer assumes the camera is the only possible view.
    struct ViewInput
    {
        XMFLOAT4X4 View{};
        XMFLOAT4X4 Projection{};
        XMFLOAT4X4 PreviousView{};
        XMFLOAT4X4 PreviousProjection{};
        float ViewportHeight = 1.0f;
        bool PredictiveStreaming = false;
    };

    Settings& GetSettings();
    void Prepare(RenderGraph& graph, const FrameInputs& inputs);
    // viewId selects the per-view shadow state slot
    // (viewId = lightIndex * 8 + faceOrCascade); ids beyond the pool
    // return false so the caller falls back to the legacy submission
    // for that view.
    bool PrepareShadowView(RenderGraph& graph, const FrameInputs& inputs, const ViewInput& view, uint32_t viewId = 0);
    bool ShadowViewActive();
    bool ShadowViewActive(uint32_t viewId);
    bool ShadowViewStillValid(const XMFLOAT4X4& view, const XMFLOAT4X4& projection, uint32_t viewId = 0);
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
        uint32_t FirstVisible = 0;
        uint32_t Candidates = 0;
        uint32_t SecondVisible = 0;
        uint32_t LodTransitions = 0;
        uint32_t CapacityFallbacks = 0;
        uint64_t HistoryBytes = 0;
        uint64_t CandidateBytes = 0;
        bool HasSample = false;
    };

    const TemporalStatistics& GetTemporalStatistics();
    GpuSceneBuffer& VisibleBuffer();
    GpuSceneBuffer& CounterBuffer();
    GpuSceneBuffer& ArgumentBuffer();
    GpuSceneBuffer& SuccessBuffer();
    GpuSceneBuffer& ShadowVisibleBuffer();
    GpuSceneBuffer& ShadowVisibleBuffer(uint32_t viewId);
    GpuSceneBuffer& ShadowCounterBuffer();
    GpuSceneBuffer& ShadowArgumentBuffer();
    const std::array<uint32_t, 64>& DebugCounters();
    void EndFrame();
    void Shutdown();
} // 名前空間 VirtualGeometryTraversal の終わり。
