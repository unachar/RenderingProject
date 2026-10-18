#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <imgui.h>

#include <array>
#include <chrono>
#include <functional>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "virtualgeometry/virtualgeometryconfig.h"
#include "rendermeasurement.h"
#include "raytelemetry.h"


class RenderProfiler final
{
public:
    static constexpr UINT kFrameCount = 3;
    static constexpr UINT kMaxTimestampQueriesPerFrame = 1024; // 128 では内訳の計測で枠が尽き、後半のパスの GPU 時間が 0 と表示されていた
    static constexpr UINT kInvalidGpuToken = UINT_MAX;
    static constexpr size_t kHistoryLength = 180;

    enum class ValidationFault : uint32_t { None, ExhaustQueries, UnclosedScope, OutOfOrderEnd };

    struct PassTiming
    {
        string Name;
        double CpuMs = 0.0;
        double GpuMs = 0.0;
        uint64_t DrawCalls = 0;
        double GpuExclusiveMs = 0.0;
        uint32_t GpuSamples = 0;
        uint32_t GpuExclusiveSamples = 0;
        uint32_t GpuEvents = 0;
    };

    struct GpuEventTiming
    {
        string Name;
        RenderMeasurement::EventInterval Interval;
        uint32_t Depth = 0;
        std::optional<double> InclusiveMs;
        std::optional<double> ExclusiveMs;
    };

    struct Counters
    {
        uint64_t DrawCalls = 0;
        uint64_t IndexedDrawCalls = 0;
        uint64_t DrawnInstances = 0;
        uint64_t DispatchCalls = 0;
        uint64_t DispatchThreadGroups = 0;
        uint64_t ExecuteIndirectCalls = 0;
        uint64_t IndirectMaxCommands = 0;
        uint64_t PipelineStateBinds = 0;
        uint64_t GraphicsDescriptorTableBinds = 0;
        uint64_t ComputeDescriptorTableBinds = 0;
        uint64_t ResourceBarrierCalls = 0;
        uint64_t ResourceBarrierCount = 0;
        uint64_t CullCandidates = 0;
        uint64_t CullVisible = 0;
        uint64_t CullDispatches = 0;
    };

    struct GeometryStreamingStatistics
    {
        uint64_t GpuRequests = 0, CpuRequests = 0, RequestOverflow = 0;
        uint64_t DiskBytes = 0, SubmittedBytes = 0, CompletedBytes = 0;
        uint64_t ResidentPages = 0, LoadingPages = 0, RequestedPages = 0, EvictedPages = 0;
        uint64_t PoolBytes = 0, BudgetBytes = 0, StagingBytes = 0;
        uint64_t MetadataBytes = 0, UploadBytes = 0;
        double CpuMilliseconds = 0, IoMilliseconds = 0, CopyMilliseconds = 0;
    };
    struct GeometryTemporalStatistics
    {
        uint64_t FirstVisible = 0, Candidates = 0, SecondVisible = 0, LodTransitions = 0, CapacityFallbacks = 0;
        uint64_t HistoryBytes = 0, CandidateBytes = 0;
        bool HasSample = false;
    };
    struct GeometryBinningStatistics
    {
        std::array<uint32_t,3> ActiveBins{}, Clusters{}, OverflowClusters{}, InvalidMaterials{};
        std::array<bool,3> HasSample{};
        uint64_t BufferBytes = 0;
    };
    struct FrameSnapshot
    {
        uint64_t Serial = 0;
        uint32_t GeometryMode = 0;
        double CpuRenderMs = 0.0;
        double CpuFrameMs = std::numeric_limits<double>::quiet_NaN();
        double FrameIntervalMs = std::numeric_limits<double>::quiet_NaN();
        double GpuRenderMs = 0.0;
        bool GpuTimingValid = false;
        bool PipelineStatisticsValid = false;
        Counters Calls{};
        GeometryStreamingStatistics Streaming{};
        GeometryTemporalStatistics Temporal{};
        GeometryBinningStatistics Binning{};
        D3D12_QUERY_DATA_PIPELINE_STATISTICS Pipeline{};
        vector<PassTiming> Passes;
        vector<GpuEventTiming> GpuEvents;
        uint32_t DroppedGpuEvents = 0;
        ValidationFault InjectedFault = ValidationFault::None;
        RayTelemetry::Counts Rays;
    };

private:
    struct GpuEvent
    {
        string Name;
        UINT BeginLocalQuery = 0;
        UINT EndLocalQuery = 0;
        bool Ended = false;
        UINT ParentId = RenderMeasurement::NoParent;
        UINT Depth = 0;
        bool ScopeCompleted = false;
    };

    struct FrameSlot
    {
        bool Pending = false;
        bool PipelineQueryClosed = false;
        bool PipelineQueryStarted = false;
        UINT TimestampQueryCount = 0;
        vector<GpuEvent> GpuEvents;
        FrameSnapshot Snapshot{};
    };

    using Clock = chrono::steady_clock;
    static inline Clock::time_point s_LastCpuFrameEnd{};
    static inline uint64_t s_LastCpuFrameSerial = 0;

    static inline Microsoft::WRL::ComPtr<ID3D12QueryHeap> s_TimestampHeap{};
    static inline Microsoft::WRL::ComPtr<ID3D12Resource> s_TimestampReadback{};
    static inline Microsoft::WRL::ComPtr<ID3D12QueryHeap> s_PipelineStatisticsHeap{};
    static inline Microsoft::WRL::ComPtr<ID3D12Resource> s_PipelineStatisticsReadback{};
    static inline UINT64 s_TimestampFrequency = 0;
    static inline bool s_GpuInitializationAttempted = false;
    static inline bool s_GpuResourcesReady = false;

    static inline array<FrameSlot, kFrameCount> s_FrameSlots{};
    static inline FrameSlot* s_ActiveSlot = nullptr;
    static inline UINT s_ActiveFrameIndex = 0;
    static inline ID3D12GraphicsCommandList* s_ActiveCommandList = nullptr;
    static inline UINT s_FrameGpuToken = kInvalidGpuToken;
    static inline vector<UINT> s_OpenGpuEvents;
    static inline ValidationFault s_ValidationFault = ValidationFault::None;
    static inline Clock::time_point s_CpuFrameBegin{};
    static inline uint64_t s_FrameSerial = 0;

    static inline FrameSnapshot s_Latest{};
    static inline bool s_HasLatest = false;
    static inline bool s_Enabled = true;
    static inline bool s_ForcePipelineStatistics = false;
    static inline bool s_ShowWindow = false; // 既定は非表示（F9で表示）。表示中だけパイプライン統計を取る
    static inline array<float, kHistoryLength> s_CpuHistory{};
    static inline array<float, kHistoryLength> s_GpuHistory{};

    static D3D12_RESOURCE_DESC BufferDescription(UINT64 sizeInBytes)
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Alignment = 0;
        description.Width = sizeInBytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        return description;
    }

    static D3D12_HEAP_PROPERTIES ReadbackHeapProperties()
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_READBACK;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    static bool CreateReadbackBuffer(
        ID3D12Device* device,
        UINT64 sizeInBytes,
        Microsoft::WRL::ComPtr<ID3D12Resource>& output)
    {
        const D3D12_HEAP_PROPERTIES heap = ReadbackHeapProperties();
        const D3D12_RESOURCE_DESC description = BufferDescription(sizeInBytes);
        return SUCCEEDED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&output)));
    }

    static bool EnsureGpuResources(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        if (s_GpuResourcesReady)
        {
            return true;
        }
        if (s_GpuInitializationAttempted)
        {
            return false;
        }
        s_GpuInitializationAttempted = true;

        if (!device || !queue || FAILED(queue->GetTimestampFrequency(&s_TimestampFrequency)) ||
            s_TimestampFrequency == 0)
        {
            return false;
        }

        D3D12_QUERY_HEAP_DESC timestampDescription{};
        timestampDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        timestampDescription.Count = kFrameCount * kMaxTimestampQueriesPerFrame;
        timestampDescription.NodeMask = 0;
        if (FAILED(device->CreateQueryHeap(
            &timestampDescription,
            IID_PPV_ARGS(&s_TimestampHeap))))
        {
            return false;
        }

        const UINT64 timestampBytes =
            static_cast<UINT64>(timestampDescription.Count) * sizeof(UINT64);
        if (!CreateReadbackBuffer(device, timestampBytes, s_TimestampReadback))
        {
            return false;
        }

        D3D12_QUERY_HEAP_DESC pipelineDescription{};
        pipelineDescription.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        pipelineDescription.Count = kFrameCount;
        pipelineDescription.NodeMask = 0;
        if (FAILED(device->CreateQueryHeap(
            &pipelineDescription,
            IID_PPV_ARGS(&s_PipelineStatisticsHeap))))
        {
            return false;
        }

        const UINT64 pipelineBytes =
            static_cast<UINT64>(kFrameCount) * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
        if (!CreateReadbackBuffer(device, pipelineBytes, s_PipelineStatisticsReadback))
        {
            return false;
        }

        s_TimestampHeap->SetName(L"RenderProfiler Timestamp Heap");
        s_TimestampReadback->SetName(L"RenderProfiler Timestamp Readback");
        s_PipelineStatisticsHeap->SetName(L"RenderProfiler Pipeline Statistics Heap");
        s_PipelineStatisticsReadback->SetName(L"RenderProfiler Pipeline Statistics Readback");
        s_GpuResourcesReady = true;
        return true;
    }

    static PassTiming& FindOrCreatePass(FrameSnapshot& snapshot, const string& name)
    {
        for (PassTiming& pass : snapshot.Passes)
        {
            if (pass.Name == name)
            {
                return pass;
            }
        }
        snapshot.Passes.push_back({ name });
        return snapshot.Passes.back();
    }

    static void PushHistory(array<float, kHistoryLength>& history, float value)
    {
        for (size_t index = 1; index < history.size(); ++index)
        {
            history[index - 1] = history[index];
        }
        history.back() = value;
    }

    static void ConsumeCompletedSlot(UINT frameIndex)
    {
        FrameSlot& slot = s_FrameSlots[frameIndex];
        if (!slot.Pending)
        {
            return;
        }

        FrameSnapshot snapshot = slot.Snapshot;
        snapshot.Rays = RayTelemetry::Data().Consume(frameIndex, snapshot.Serial);

        if (s_GpuResourcesReady && slot.TimestampQueryCount > 0)
        {
            const UINT64 firstQuery =
                static_cast<UINT64>(frameIndex) * kMaxTimestampQueriesPerFrame;
            const UINT64 byteOffset = firstQuery * sizeof(UINT64);
            const UINT64 byteSize =
                static_cast<UINT64>(slot.TimestampQueryCount) * sizeof(UINT64);
            D3D12_RANGE readRange{ byteOffset, byteOffset + byteSize };
            void* mapped = nullptr;
            if (SUCCEEDED(s_TimestampReadback->Map(0, &readRange, &mapped)) && mapped)
            {
                const auto* timestamps = reinterpret_cast<const UINT64*>(
                    static_cast<const uint8_t*>(mapped) + byteOffset);
                vector<RenderMeasurement::EventInterval> intervals;
                intervals.reserve(slot.GpuEvents.size());
                for (UINT index = 0; index < slot.GpuEvents.size(); ++index)
                {
                    const GpuEvent& event = slot.GpuEvents[index];
                    RenderMeasurement::EventInterval interval;
                    interval.Id = index;
                    interval.ParentId = event.ParentId;
                    // This profiler currently timestamps the ordered direct queue only.
                    interval.QueueId = 0;
                    if (event.Ended && event.ScopeCompleted &&
                        event.BeginLocalQuery < slot.TimestampQueryCount &&
                        event.EndLocalQuery < slot.TimestampQueryCount)
                    {
                        interval.Begin = timestamps[event.BeginLocalQuery];
                        interval.End = timestamps[event.EndLocalQuery];
                        interval.Valid = interval.End >= interval.Begin;
                    }
                    intervals.push_back(interval);
                }
                for (UINT index = 0; index < intervals.size(); ++index)
                {
                    const GpuEvent& event = slot.GpuEvents[index];
                    const auto& interval = intervals[index];
                    GpuEventTiming timing{event.Name, interval, event.Depth, {}, {}};
                    PassTiming& pass = FindOrCreatePass(snapshot, event.Name);
                    ++pass.GpuEvents;
                    if (!interval.Valid)
                    {
                        snapshot.GpuEvents.push_back(move(timing));
                        continue;
                    }
                    const double milliseconds =
                        static_cast<double>(interval.End - interval.Begin) * 1000.0 /
                        static_cast<double>(s_TimestampFrequency);
                    timing.InclusiveMs = milliseconds;
                    pass.GpuMs += milliseconds;
                    ++pass.GpuSamples;
                    const double exclusiveTicks = RenderMeasurement::ExclusiveTicks(interval, intervals);
                    if (std::isfinite(exclusiveTicks) && snapshot.DroppedGpuEvents == 0)
                    {
                        timing.ExclusiveMs = exclusiveTicks * 1000.0 / s_TimestampFrequency;
                        pass.GpuExclusiveMs += *timing.ExclusiveMs;
                        ++pass.GpuExclusiveSamples;
                    }
                    snapshot.GpuEvents.push_back(move(timing));
                    if (event.Name == "Frame")
                    {
                        snapshot.GpuRenderMs = milliseconds;
                        snapshot.GpuTimingValid = true;
                    }
                }
                const D3D12_RANGE noWrites{ 0, 0 };
                s_TimestampReadback->Unmap(0, &noWrites);
            }

            const UINT64 pipelineOffset =
                static_cast<UINT64>(frameIndex) * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
            D3D12_RANGE pipelineReadRange{
                pipelineOffset,
                pipelineOffset + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)
            };
            mapped = nullptr;
            if (slot.PipelineQueryStarted && SUCCEEDED(s_PipelineStatisticsReadback->Map(
                0,
                &pipelineReadRange,
                &mapped)) && mapped)
            {
                snapshot.Pipeline = *reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(
                    static_cast<const uint8_t*>(mapped) + pipelineOffset);
                snapshot.PipelineStatisticsValid = !slot.PipelineQueryClosed;
                const D3D12_RANGE noWrites{ 0, 0 };
                s_PipelineStatisticsReadback->Unmap(0, &noWrites);
            }
        }

        s_Latest = move(snapshot);
        s_HasLatest = true;
        PushHistory(s_CpuHistory, static_cast<float>(s_Latest.CpuRenderMs));
        if (s_Latest.GpuTimingValid)
            PushHistory(s_GpuHistory, static_cast<float>(s_Latest.GpuRenderMs));
        slot.Pending = false;
    }

    static FrameSnapshot* ActiveSnapshot()
    {
        if (s_ActiveSlot)
        {
            return &s_ActiveSlot->Snapshot;
        }
        return nullptr;
    }

public:
    class ScopedEvent final
    {
    private:
        const char* m_Name = nullptr;
        ID3D12GraphicsCommandList* m_CommandList = nullptr;
        UINT m_GpuToken = kInvalidGpuToken;
        Clock::time_point m_CpuBegin{};
        bool m_Active = false;
        uint64_t m_DrawCallsBegin = 0;
        uint64_t m_FrameSerial = 0;

    public:
        ScopedEvent(const char* name, ID3D12GraphicsCommandList* commandList)
            : m_Name(name), m_CommandList(commandList)
        {
            if (!RenderProfiler::IsFrameActive() || !name)
            {
                return;
            }
            m_Active = true;
            m_FrameSerial = ActiveSnapshot()->Serial;
            m_DrawCallsBegin = ActiveSnapshot()->Calls.DrawCalls;
            m_CpuBegin = Clock::now();
            m_GpuToken = RenderProfiler::BeginGpuEvent(name, commandList);
        }

        ScopedEvent(const ScopedEvent&) = delete;
        ScopedEvent& operator=(const ScopedEvent&) = delete;

        ~ScopedEvent()
        {
            Finish();
        }

        void Finish()
        {
            if (!m_Active)
            {
                return;
            }
            m_Active = false;
            if (!ActiveSnapshot() || ActiveSnapshot()->Serial != m_FrameSerial)
                return;
            RenderProfiler::EndGpuEvent(m_GpuToken, m_CommandList);
            const double cpuMs = chrono::duration<double, milli>(
                Clock::now() - m_CpuBegin).count();
            RenderProfiler::RecordCpuPass(m_Name, cpuMs);
            FindOrCreatePass(*ActiveSnapshot(), m_Name).DrawCalls +=
                ActiveSnapshot()->Calls.DrawCalls - m_DrawCallsBegin;
        }
    };

    static bool IsFrameActive()
    {
        return s_ActiveSlot != nullptr;
    }

    static void SetEnabled(bool enabled)
    {
        s_Enabled = enabled;
    }

    static bool IsEnabled()
    {
        return s_Enabled;
    }

    static uint64_t GetFrameSerial() { return s_FrameSerial; }

    static void RecordCpuFrameTime(uint64_t serial, double milliseconds)
    {
        const auto now = Clock::now();
        for (auto& slot : s_FrameSlots)
        {
            if (!slot.Pending || slot.Snapshot.Serial != serial) continue;
            slot.Snapshot.CpuFrameMs = milliseconds;
            if (s_LastCpuFrameSerial != 0 && serial == s_LastCpuFrameSerial + 1)
                slot.Snapshot.FrameIntervalMs = chrono::duration<double, milli>(now - s_LastCpuFrameEnd).count();
            s_LastCpuFrameEnd = now;
            s_LastCpuFrameSerial = serial;
            break;
        }
    }

    // Caller must first wait for all direct-queue submissions to finish.
    static void DrainCompletedFrames(const std::function<void(const FrameSnapshot&)>& visitor)
    {
        if (IsFrameActive()) return;
        for (UINT index = 0; index < kFrameCount; ++index)
        {
            if (!s_FrameSlots[index].Pending) continue;
            ConsumeCompletedSlot(index);
            visitor(s_Latest);
        }
    }

    // Used only by the GPU validation fixture, before BeginFrame. Physical query
    // heap capacity is unchanged; the fault is part of the resolved frame record.
    static void SetValidationFault(ValidationFault fault)
    {
        if (!IsFrameActive()) s_ValidationFault = fault;
    }

    static const FrameSnapshot& GetLatest()
    {
        return s_Latest;
    }

    static bool HasLatest()
    {
        return s_HasLatest;
    }

    static void RecordGeometryStreaming(const GeometryStreamingStatistics& value)
    {
        if (s_ActiveSlot)
        {
            s_ActiveSlot->Snapshot.Streaming = value;
        }
    }
    static void RecordGeometryTemporal(const GeometryTemporalStatistics& value)
    {
        if (s_ActiveSlot) s_ActiveSlot->Snapshot.Temporal = value;
    }
    static void RecordGeometryBinning(const GeometryBinningStatistics& value)
    {
        if (s_ActiveSlot) s_ActiveSlot->Snapshot.Binning = value;
    }
    static void BeginFrame(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        ID3D12GraphicsCommandList* commandList,
        UINT frameIndex)
    {
        if (!s_Enabled || !commandList)
        {
            return;
        }

        frameIndex %= kFrameCount;
        EnsureGpuResources(device, queue);
        ConsumeCompletedSlot(frameIndex);

        FrameSlot& slot = s_FrameSlots[frameIndex];
        slot.TimestampQueryCount = 0;
        slot.GpuEvents.clear();
        slot.GpuEvents.reserve(kMaxTimestampQueriesPerFrame / 2);
        s_OpenGpuEvents.clear();
        slot.Snapshot = {};
        slot.Snapshot.InjectedFault = s_ValidationFault;
        // パイプライン統計クエリはフレーム全体を対象にするためGPU負荷が高い。ウィンドウ表示中だけ取る。
        slot.PipelineQueryStarted = s_GpuResourcesReady && (s_ShowWindow || s_ForcePipelineStatistics);
        slot.PipelineQueryClosed = !slot.PipelineQueryStarted;
        slot.Snapshot.Serial = ++s_FrameSerial;
        slot.Snapshot.GeometryMode = static_cast<uint32_t>(GetGeometryRenderingMode());

        s_ActiveSlot = &slot;
        s_ActiveFrameIndex = frameIndex;
        s_ActiveCommandList = commandList;
        s_CpuFrameBegin = Clock::now();

        if (slot.PipelineQueryStarted)
        {
            commandList->BeginQuery(
                s_PipelineStatisticsHeap.Get(),
                D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                frameIndex);
        }
        s_FrameGpuToken = BeginGpuEvent("Frame", commandList);
        if (s_ValidationFault == ValidationFault::UnclosedScope)
            BeginGpuEvent("Validation Unclosed", commandList);
        if (s_ValidationFault == ValidationFault::OutOfOrderEnd)
        {
            const UINT parent = BeginGpuEvent("Validation Parent", commandList);
            BeginGpuEvent("Validation Child", commandList);
            EndGpuEvent(parent, commandList);
        }
        RayTelemetry::Data().BeginFrame(device, commandList, slot.Snapshot.Serial, frameIndex, RayTelemetry::Requested());
    }

    static void PrepareCommandListSplit(ID3D12GraphicsCommandList* commandList)
    {
        if (s_ActiveSlot && s_GpuResourcesReady && !s_ActiveSlot->PipelineQueryClosed)
        {
            commandList->EndQuery(s_PipelineStatisticsHeap.Get(),
                D3D12_QUERY_TYPE_PIPELINE_STATISTICS, s_ActiveFrameIndex);
            // Pipeline statistics cannot span command-list resets. Timestamp pairs
            // remain valid across the GPU-ordered D3D12/CUDA submissions.
            s_ActiveSlot->PipelineQueryClosed = true;
        }
    }

    static UINT BeginGpuEvent(const char* name, ID3D12GraphicsCommandList* commandList)
    {
        const UINT queryLimit = s_ValidationFault == ValidationFault::ExhaustQueries ? 2u : kMaxTimestampQueriesPerFrame;
        if (!s_ActiveSlot || !s_GpuResourcesReady || !commandList || !name ||
            s_ActiveSlot->TimestampQueryCount + 2 > queryLimit)
        {
            if (s_ActiveSlot) ++s_ActiveSlot->Snapshot.DroppedGpuEvents;
            return kInvalidGpuToken;
        }

        const UINT beginLocal = s_ActiveSlot->TimestampQueryCount++;
        const UINT endLocal = s_ActiveSlot->TimestampQueryCount++;
        const UINT absoluteBegin =
            s_ActiveFrameIndex * kMaxTimestampQueriesPerFrame + beginLocal;
		commandList->SetMarker(0, name, static_cast<UINT>(strlen(name)));
        commandList->EndQuery(
            s_TimestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            absoluteBegin);

        const UINT parent = s_OpenGpuEvents.empty() ? RenderMeasurement::NoParent : s_OpenGpuEvents.back();
        const UINT depth = static_cast<UINT>(s_OpenGpuEvents.size());
        const UINT token = static_cast<UINT>(s_ActiveSlot->GpuEvents.size());
        s_ActiveSlot->GpuEvents.push_back({ name, beginLocal, endLocal, false, parent, depth, false });
        s_OpenGpuEvents.push_back(token);
        return token;
    }

    static void EndGpuEvent(UINT token, ID3D12GraphicsCommandList* commandList)
    {
        if (!s_ActiveSlot || !s_GpuResourcesReady || !commandList ||
            token == kInvalidGpuToken || token >= s_ActiveSlot->GpuEvents.size())
        {
            return;
        }

        GpuEvent& event = s_ActiveSlot->GpuEvents[token];
        if (event.Ended)
        {
            return;
        }
        const UINT absoluteEnd =
            s_ActiveFrameIndex * kMaxTimestampQueriesPerFrame + event.EndLocalQuery;
        commandList->EndQuery(
            s_TimestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            absoluteEnd);
        event.Ended = true;
        event.ScopeCompleted = !s_OpenGpuEvents.empty() && s_OpenGpuEvents.back() == token;
        const auto open = find(s_OpenGpuEvents.begin(), s_OpenGpuEvents.end(), token);
        if (open != s_OpenGpuEvents.end()) s_OpenGpuEvents.erase(open);
    }

    static void RecordCpuPass(const char* name, double milliseconds)
    {
        FrameSnapshot* snapshot = ActiveSnapshot();
        if (!snapshot || !name)
        {
            return;
        }
        FindOrCreatePass(*snapshot, name).CpuMs += milliseconds;
    }

    static void EndFrame(ID3D12GraphicsCommandList* commandList)
    {
        if (!s_ActiveSlot || !commandList)
        {
            return;
        }
        RayTelemetry::Data().EndFrame(commandList);

        // ResolveQueryData must never include an unwritten end query. Close leaked
        // scopes for query safety, but keep their measurements explicitly invalid.
        while (!s_OpenGpuEvents.empty() && s_OpenGpuEvents.back() != s_FrameGpuToken)
        {
            const UINT token = s_OpenGpuEvents.back();
            EndGpuEvent(token, commandList);
            s_ActiveSlot->GpuEvents[token].ScopeCompleted = false;
        }
        EndGpuEvent(s_FrameGpuToken, commandList);
        s_OpenGpuEvents.clear();
        s_ActiveSlot->Snapshot.CpuRenderMs =
            chrono::duration<double, milli>(Clock::now() - s_CpuFrameBegin).count();

        if (s_GpuResourcesReady)
        {
            if (!s_ActiveSlot->PipelineQueryClosed)
            {
                commandList->EndQuery(
                    s_PipelineStatisticsHeap.Get(),
                    D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                    s_ActiveFrameIndex);
            }

            const UINT firstQuery =
                s_ActiveFrameIndex * kMaxTimestampQueriesPerFrame;
            const UINT queryCount = s_ActiveSlot->TimestampQueryCount;
            if (queryCount > 0)
            {
                commandList->ResolveQueryData(
                    s_TimestampHeap.Get(),
                    D3D12_QUERY_TYPE_TIMESTAMP,
                    firstQuery,
                    queryCount,
                    s_TimestampReadback.Get(),
                    static_cast<UINT64>(firstQuery) * sizeof(UINT64));
            }

            if (s_ActiveSlot->PipelineQueryStarted)
            commandList->ResolveQueryData(
                s_PipelineStatisticsHeap.Get(),
                D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                s_ActiveFrameIndex,
                1,
                s_PipelineStatisticsReadback.Get(),
                static_cast<UINT64>(s_ActiveFrameIndex) *
                    sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
        }

        s_ActiveSlot->Pending = true;
        s_ActiveSlot = nullptr;
        s_ActiveCommandList = nullptr;
        s_FrameGpuToken = kInvalidGpuToken;
    }

    static void RecordDraw(UINT instanceCount, bool indexed)
    {
        FrameSnapshot* snapshot = ActiveSnapshot();
        if (!snapshot)
        {
            return;
        }
        ++snapshot->Calls.DrawCalls;
        snapshot->Calls.DrawnInstances += instanceCount;
        if (indexed)
        {
            ++snapshot->Calls.IndexedDrawCalls;
        }
    }

    static void RecordDispatch(UINT x, UINT y, UINT z)
    {
        FrameSnapshot* snapshot = ActiveSnapshot();
        if (!snapshot)
        {
            return;
        }
        ++snapshot->Calls.DispatchCalls;
        snapshot->Calls.DispatchThreadGroups +=
            static_cast<uint64_t>(x) * y * z;
    }

    static void RecordExecuteIndirect(UINT maxCommandCount)
    {
        FrameSnapshot* snapshot = ActiveSnapshot();
        if (!snapshot)
        {
            return;
        }
        ++snapshot->Calls.ExecuteIndirectCalls;
        snapshot->Calls.IndirectMaxCommands += maxCommandCount;
    }

    static void RecordPipelineStateBind()
    {
        if (FrameSnapshot* snapshot = ActiveSnapshot())
        {
            ++snapshot->Calls.PipelineStateBinds;
        }
    }

    static void RecordGraphicsDescriptorTableBind()
    {
        if (FrameSnapshot* snapshot = ActiveSnapshot())
        {
            ++snapshot->Calls.GraphicsDescriptorTableBinds;
        }
    }

    static void RecordComputeDescriptorTableBind()
    {
        if (FrameSnapshot* snapshot = ActiveSnapshot())
        {
            ++snapshot->Calls.ComputeDescriptorTableBinds;
        }
    }

    static void RecordResourceBarriers(UINT barrierCount)
    {
        if (FrameSnapshot* snapshot = ActiveSnapshot())
        {
            ++snapshot->Calls.ResourceBarrierCalls;
            snapshot->Calls.ResourceBarrierCount += barrierCount;
        }
    }

    static void RecordCullCandidates(UINT candidateCount)
    {
        if (FrameSnapshot* snapshot = ActiveSnapshot())
        {
            snapshot->Calls.CullCandidates += candidateCount;
            ++snapshot->Calls.CullDispatches;
        }
    }

    static void RecordCullVisible(UINT visibleCount)
    {
        if (FrameSnapshot* snapshot = ActiveSnapshot())
        {
            snapshot->Calls.CullVisible += visibleCount;
        }
    }

    // 自動テスト・ベンチマークではパイプライン統計を常に取る。
    static void SetPipelineStatisticsForced(bool forced)
    {
        s_ForcePipelineStatistics = forced;
    }

    static void SetWindowVisible(bool visible)
    {
        s_ShowWindow = visible;
    }

    static void DrawImGuiWindow()
    {
        if ((GetAsyncKeyState(VK_F9) & 1) != 0)
        {
            s_ShowWindow = !s_ShowWindow;
        }
        if (!s_ShowWindow)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(600.0f, 620.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("レンダリングプロファイラ [F9]", &s_ShowWindow))
        {
            ImGui::End();
            return;
        }

        ImGui::Checkbox("計測を有効化", &s_Enabled);
        ImGui::Checkbox("Count DXR rays (extra GPU work)", &RayTelemetry::Requested());
        if (RayTelemetry::Requested())
        {
            const auto& counts = s_Latest.Rays;
            if (counts.ProviderKnown(RayTelemetry::Radiance))
                ImGui::Text("Reflection %llu / GI %llu / bounce visibility %llu / ReSTIR visibility %llu",
                    static_cast<unsigned long long>(counts.Rays[RAY_TELEMETRY_REFLECTION]),
                    static_cast<unsigned long long>(counts.Rays[RAY_TELEMETRY_GI]),
                    static_cast<unsigned long long>(counts.Rays[RAY_TELEMETRY_RADIANCE_SHADOW]),
                    static_cast<unsigned long long>(counts.Rays[RAY_TELEMETRY_RESTIR_VISIBILITY]));
            else ImGui::TextDisabled("Radiance ray count unavailable");
            if (counts.ProviderKnown(RayTelemetry::ShadowAo))
                ImGui::Text("Shadow %llu / AO %llu", static_cast<unsigned long long>(counts.Rays[RAY_TELEMETRY_SHADOW]),
                    static_cast<unsigned long long>(counts.Rays[RAY_TELEMETRY_AO]));
            else ImGui::TextDisabled("Shadow/AO ray count unavailable");
            ImGui::TextDisabled("Partial DXR source coverage; total ray count unavailable.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("GPU結果はフレームリング分遅延します");

        if (!s_HasLatest)
        {
            ImGui::TextUnformatted("計測結果を待っています...");
            ImGui::End();
            return;
        }

        ImGui::Text("CPU Render: %.3f ms", s_Latest.CpuRenderMs);
        if (s_Latest.GpuTimingValid)
        {
            ImGui::Text("GPU Render: %.3f ms", s_Latest.GpuRenderMs);
        }
        else
        {
            ImGui::TextUnformatted("GPU Render: 利用不可");
        }

        ImGui::PlotLines(
            "CPU ms",
            s_CpuHistory.data(),
            static_cast<int>(s_CpuHistory.size()),
            0,
            nullptr,
            0.0f,
            40.0f,
            ImVec2(0.0f, 55.0f));
        ImGui::PlotLines(
            "GPU ms",
            s_GpuHistory.data(),
            static_cast<int>(s_GpuHistory.size()),
            0,
            nullptr,
            0.0f,
            40.0f,
            ImVec2(0.0f, 55.0f));

        ImGui::SeparatorText("D3D12 API submissions");
        const Counters& calls = s_Latest.Calls;
        const auto& streaming = s_Latest.Streaming;
        const auto& temporal = s_Latest.Temporal;
        const auto& binning = s_Latest.Binning;
        const char* binPhases[] = {"First","Second","Velocity"};
        for (uint32_t phase = 0; phase < 3; ++phase)
        {
            if (!binning.HasSample[phase]) continue;
            ImGui::Text("Native %s material bins %u / clusters %u / shared overflow %u / invalid %u",binPhases[phase],
                binning.ActiveBins[phase],binning.Clusters[phase],binning.OverflowClusters[phase],binning.InvalidMaterials[phase]);
        }
        ImGui::Text("Native material bin buffers %.2f MiB",double(binning.BufferBytes)/(1024*1024));
        if (temporal.HasSample)
        {
            ImGui::Text("Native first / retest / recovered %llu / %llu / %llu, LOD transitions %llu, capacity fallbacks %llu",
                temporal.FirstVisible,temporal.Candidates,temporal.SecondVisible,temporal.LodTransitions,temporal.CapacityFallbacks);
        }
        ImGui::Text("Native LOD history %.2f MiB / phase buffers %.2f MiB",
            double(temporal.HistoryBytes)/(1024*1024),double(temporal.CandidateBytes)/(1024*1024));
        ImGui::Text("Geometry requests GPU/CPU %llu / %llu (cumulative), overflow %llu",
            streaming.GpuRequests,streaming.CpuRequests,streaming.RequestOverflow);
        ImGui::Text("Geometry SSD / Copy submitted / completed %.2f / %.2f / %.2f MiB (cumulative)",
            double(streaming.DiskBytes)/(1024*1024),double(streaming.SubmittedBytes)/(1024*1024),double(streaming.CompletedBytes)/(1024*1024));
        ImGui::Text("Geometry VRAM reserved/budget %.2f / %.2f MiB, staging %.2f MiB",
            double(streaming.PoolBytes)/(1024*1024),double(streaming.BudgetBytes)/(1024*1024),double(streaming.StagingBytes)/(1024*1024));
        ImGui::Text("Geometry metadata buffers incl staging %.2f MiB / Page Copy upload heap %.2f MiB",
            double(streaming.MetadataBytes)/(1024*1024),double(streaming.UploadBytes)/(1024*1024));
        ImGui::Text("Geometry streaming CPU %.3f ms / I/O %.3f ms cumulative / latest Copy completion latency %.3f ms / Copy GPU N/A",
            streaming.CpuMilliseconds,streaming.IoMilliseconds,streaming.CopyMilliseconds);
        if (ImGui::BeginTable("RenderProfilerCalls", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            auto row = [](const char* label, uint64_t value)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%llu", static_cast<unsigned long long>(value));
                };
            row("Draw calls", calls.DrawCalls);
            row("Indexed draw calls", calls.IndexedDrawCalls);
            row("Drawn instances", calls.DrawnInstances);
            row("Dispatch calls", calls.DispatchCalls);
            row("Dispatch thread groups", calls.DispatchThreadGroups);
            row("ExecuteIndirect calls", calls.ExecuteIndirectCalls);
            row("Indirect maximum commands", calls.IndirectMaxCommands);
            row("PSO binds", calls.PipelineStateBinds);
            row("Graphics descriptor-table binds", calls.GraphicsDescriptorTableBinds);
            row("Compute descriptor-table binds", calls.ComputeDescriptorTableBinds);
            row("ResourceBarrier calls", calls.ResourceBarrierCalls);
            row("Resource barriers", calls.ResourceBarrierCount);
            row("Cull candidates", calls.CullCandidates);
            row("Cull visible (readback)", calls.CullVisible);
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Pass timings");
        ImGui::TextDisabled("GPU history contains valid frame samples only.");
        ImGui::TextDisabled("Inclusive rows overlap; GPU self excludes same-queue children.");
        if (s_Latest.DroppedGpuEvents != 0)
            ImGui::TextDisabled("GPU events unavailable: %u", s_Latest.DroppedGpuEvents);
        if (ImGui::BeginTable(
            "RenderProfilerPasses",
            5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Pass");
            ImGui::TableSetupColumn("CPU incl ms");
            ImGui::TableSetupColumn("GPU incl ms");
            ImGui::TableSetupColumn("GPU self ms");
            ImGui::TableSetupColumn("Direct draws");
            ImGui::TableHeadersRow();
            for (const PassTiming& pass : s_Latest.Passes)
            {
                if (pass.Name == "Frame")
                {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(pass.Name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", pass.CpuMs);
                ImGui::TableSetColumnIndex(2);
                if (RenderMeasurement::IsGpuTimingComplete(pass.GpuEvents, pass.GpuSamples, s_Latest.DroppedGpuEvents))
                    ImGui::Text("%.3f", pass.GpuMs);
                else ImGui::TextUnformatted("N/A");
                ImGui::TableSetColumnIndex(3);
                if (RenderMeasurement::IsGpuTimingComplete(pass.GpuEvents, pass.GpuExclusiveSamples, s_Latest.DroppedGpuEvents))
                    ImGui::Text("%.3f", pass.GpuExclusiveMs);
                else ImGui::TextUnformatted("N/A");
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", static_cast<unsigned long long>(pass.DrawCalls));
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Pipeline statistics");
        if (s_Latest.PipelineStatisticsValid)
        {
            const auto& statistics = s_Latest.Pipeline;
            ImGui::Text("IA vertices: %llu", static_cast<unsigned long long>(statistics.IAVertices));
            ImGui::Text("IA primitives: %llu", static_cast<unsigned long long>(statistics.IAPrimitives));
            ImGui::Text("VS invocations: %llu", static_cast<unsigned long long>(statistics.VSInvocations));
            ImGui::Text("PS invocations: %llu", static_cast<unsigned long long>(statistics.PSInvocations));
            ImGui::Text("GS invocations: %llu", static_cast<unsigned long long>(statistics.GSInvocations));
            ImGui::Text("Clipping input/output: %llu / %llu",
                static_cast<unsigned long long>(statistics.CInvocations),
                static_cast<unsigned long long>(statistics.CPrimitives));
            ImGui::Text("CS invocations: %llu", static_cast<unsigned long long>(statistics.CSInvocations));
        }
        else
        {
            ImGui::TextUnformatted("Pipeline statistics: 利用不可");
        }

        ImGui::End();
    }
};

