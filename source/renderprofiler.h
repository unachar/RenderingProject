#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <imgui.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Phase 0 measurement foundation for the GPU-driven renderer.
//
// GPU timings use D3D12 timestamp queries and are consumed only when the
// corresponding back-buffer slot is known to be fence-safe. Pipeline statistics
// are collected with a second query heap. CPU timings use steady_clock.
class RenderProfiler final
{
public:
    static constexpr UINT kFrameCount = 3;
    static constexpr UINT kMaxTimestampQueriesPerFrame = 128;
    static constexpr UINT kInvalidGpuToken = UINT_MAX;
    static constexpr size_t kHistoryLength = 180;

    struct PassTiming
    {
        std::string Name;
        double CpuMs = 0.0;
        double GpuMs = 0.0;
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

    struct FrameSnapshot
    {
        uint64_t Serial = 0;
        double CpuRenderMs = 0.0;
        double GpuRenderMs = 0.0;
        bool GpuTimingValid = false;
        bool PipelineStatisticsValid = false;
        Counters Calls{};
        D3D12_QUERY_DATA_PIPELINE_STATISTICS Pipeline{};
        std::vector<PassTiming> Passes;
    };

private:
    struct GpuEvent
    {
        std::string Name;
        UINT BeginLocalQuery = 0;
        UINT EndLocalQuery = 0;
        bool Ended = false;
    };

    struct FrameSlot
    {
        bool Pending = false;
        UINT TimestampQueryCount = 0;
        std::vector<GpuEvent> GpuEvents;
        FrameSnapshot Snapshot{};
    };

    using Clock = std::chrono::steady_clock;

    static inline Microsoft::WRL::ComPtr<ID3D12QueryHeap> s_TimestampHeap{};
    static inline Microsoft::WRL::ComPtr<ID3D12Resource> s_TimestampReadback{};
    static inline Microsoft::WRL::ComPtr<ID3D12QueryHeap> s_PipelineStatisticsHeap{};
    static inline Microsoft::WRL::ComPtr<ID3D12Resource> s_PipelineStatisticsReadback{};
    static inline UINT64 s_TimestampFrequency = 0;
    static inline bool s_GpuInitializationAttempted = false;
    static inline bool s_GpuResourcesReady = false;

    static inline std::array<FrameSlot, kFrameCount> s_FrameSlots{};
    static inline FrameSlot* s_ActiveSlot = nullptr;
    static inline UINT s_ActiveFrameIndex = 0;
    static inline ID3D12GraphicsCommandList* s_ActiveCommandList = nullptr;
    static inline UINT s_FrameGpuToken = kInvalidGpuToken;
    static inline Clock::time_point s_CpuFrameBegin{};
    static inline uint64_t s_FrameSerial = 0;

    static inline FrameSnapshot s_Latest{};
    static inline bool s_HasLatest = false;
    static inline bool s_Enabled = true;
    static inline bool s_ShowWindow = true;
    static inline std::array<float, kHistoryLength> s_CpuHistory{};
    static inline std::array<float, kHistoryLength> s_GpuHistory{};

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

    static PassTiming& FindOrCreatePass(FrameSnapshot& snapshot, const std::string& name)
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

    static void PushHistory(std::array<float, kHistoryLength>& history, float value)
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
                for (const GpuEvent& event : slot.GpuEvents)
                {
                    if (!event.Ended ||
                        event.BeginLocalQuery >= slot.TimestampQueryCount ||
                        event.EndLocalQuery >= slot.TimestampQueryCount)
                    {
                        continue;
                    }
                    const UINT64 begin = timestamps[event.BeginLocalQuery];
                    const UINT64 end = timestamps[event.EndLocalQuery];
                    if (end < begin)
                    {
                        continue;
                    }
                    const double milliseconds =
                        static_cast<double>(end - begin) * 1000.0 /
                        static_cast<double>(s_TimestampFrequency);
                    PassTiming& pass = FindOrCreatePass(snapshot, event.Name);
                    pass.GpuMs += milliseconds;
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
            if (SUCCEEDED(s_PipelineStatisticsReadback->Map(
                0,
                &pipelineReadRange,
                &mapped)) && mapped)
            {
                snapshot.Pipeline = *reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(
                    static_cast<const uint8_t*>(mapped) + pipelineOffset);
                snapshot.PipelineStatisticsValid = true;
                const D3D12_RANGE noWrites{ 0, 0 };
                s_PipelineStatisticsReadback->Unmap(0, &noWrites);
            }
        }

        s_Latest = std::move(snapshot);
        s_HasLatest = true;
        PushHistory(s_CpuHistory, static_cast<float>(s_Latest.CpuRenderMs));
        PushHistory(s_GpuHistory, static_cast<float>(s_Latest.GpuRenderMs));
        slot.Pending = false;
    }

    static FrameSnapshot* ActiveSnapshot()
    {
        return s_ActiveSlot ? &s_ActiveSlot->Snapshot : nullptr;
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

    public:
        ScopedEvent(const char* name, ID3D12GraphicsCommandList* commandList)
            : m_Name(name), m_CommandList(commandList)
        {
            if (!RenderProfiler::IsFrameActive() || !name)
            {
                return;
            }
            m_Active = true;
            m_CpuBegin = Clock::now();
            m_GpuToken = RenderProfiler::BeginGpuEvent(name, commandList);
        }

        ScopedEvent(const ScopedEvent&) = delete;
        ScopedEvent& operator=(const ScopedEvent&) = delete;

        ~ScopedEvent()
        {
            if (!m_Active)
            {
                return;
            }
            RenderProfiler::EndGpuEvent(m_GpuToken, m_CommandList);
            const double cpuMs = std::chrono::duration<double, std::milli>(
                Clock::now() - m_CpuBegin).count();
            RenderProfiler::RecordCpuPass(m_Name, cpuMs);
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

    static const FrameSnapshot& GetLatest()
    {
        return s_Latest;
    }

    static bool HasLatest()
    {
        return s_HasLatest;
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
        slot.Snapshot = {};
        slot.Snapshot.Serial = ++s_FrameSerial;

        s_ActiveSlot = &slot;
        s_ActiveFrameIndex = frameIndex;
        s_ActiveCommandList = commandList;
        s_CpuFrameBegin = Clock::now();

        if (s_GpuResourcesReady)
        {
            commandList->BeginQuery(
                s_PipelineStatisticsHeap.Get(),
                D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                frameIndex);
        }
        s_FrameGpuToken = BeginGpuEvent("Frame", commandList);
    }

    static UINT BeginGpuEvent(const char* name, ID3D12GraphicsCommandList* commandList)
    {
        if (!s_ActiveSlot || !s_GpuResourcesReady || !commandList || !name ||
            s_ActiveSlot->TimestampQueryCount + 2 > kMaxTimestampQueriesPerFrame)
        {
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

        s_ActiveSlot->GpuEvents.push_back({ name, beginLocal, endLocal, false });
        return static_cast<UINT>(s_ActiveSlot->GpuEvents.size() - 1);
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

        EndGpuEvent(s_FrameGpuToken, commandList);
        s_ActiveSlot->Snapshot.CpuRenderMs =
            std::chrono::duration<double, std::milli>(Clock::now() - s_CpuFrameBegin).count();

        if (s_GpuResourcesReady)
        {
            commandList->EndQuery(
                s_PipelineStatisticsHeap.Get(),
                D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                s_ActiveFrameIndex);

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
        if (ImGui::BeginTable(
            "RenderProfilerPasses",
            3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Pass");
            ImGui::TableSetupColumn("CPU ms");
            ImGui::TableSetupColumn("GPU ms");
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
                ImGui::Text("%.3f", pass.GpuMs);
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
