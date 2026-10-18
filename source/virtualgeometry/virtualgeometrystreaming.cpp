#include "pch.h"
#include "virtualgeometrystreaming.h"
#include "virtualgeometryresidency.h"
#include "virtualgeometrypageio.h"
#include "virtualgeometrypagepool.h"
#include "graphicsdevice.h"
#include "frameconstants.h"
#include "graphicslog.h"
#include "renderprofiler.h"
#include <chrono>
#include <numeric>
#include <atomic>

namespace VirtualGeometryStreaming
{
namespace
{
using Clock = std::chrono::steady_clock;

struct Source
{
    std::shared_ptr<const VirtualGeometryPageCache::PageManifest> Manifest;
    uint32_t LocalPage = 0;
    uint32_t Failures = 0;
    Clock::time_point RetryAfter{};
    Clock::time_point SubmittedAt{};
};

struct Frame
{
    GpuSceneBuffer Residency, RequestBits, Requests, Usage;
    ComPtr<ID3D12Resource> Readback;
    uint64_t ReadbackBytes = 0, Fence = 0, Serial = 0;
    uint32_t Words = 0, Pages = 0;
};

struct State
{
    Settings Options;
    Statistics Stats;
    ResidencyManager Manager;
    std::unique_ptr<PageIo> Io;
    std::unique_ptr<PagePool> Pool;
    std::vector<Source> Sources;
    std::deque<PageIoCompletion> Ready;
    std::array<Frame, 3> Frames;
    uint64_t Serial = 0;
    uint32_t Outstanding = 0;
    bool Initialized = false, Available = false;
};

State& Data()
{
    static State state;
    return state;
}

Frame& Current()
{
    return Data().Frames.at(GraphicsDevice::GetFrameIndex());
}

void Initialize()
{
    auto& state = Data();
    if (state.Initialized)
    {
        return;
    }
    state.Initialized = true;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    auto* device = GraphicsDevice::GetDevice();
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
        options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
    {
        WriteGraphicsLog("Geometry streaming unavailable: resource binding tier 2 required\n");
        return;
    }
    auto* heap = FrameConstants::GetCbvHeap();
    uint32_t first = heap->GetDesc().NumDescriptors - MaximumSlots;
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    uint32_t increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    descriptor.ptr += SIZE_T(first) * increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC nullView{};
    nullView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    nullView.Format = DXGI_FORMAT_R32_TYPELESS;
    nullView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullView.Buffer.NumElements = 65536 / 4;
    nullView.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    for (uint32_t slot = 0; slot < MaximumSlots; ++slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE target{descriptor.ptr + SIZE_T(slot) * increment};
        device->CreateShaderResourceView(nullptr, &nullView, target);
    }
    state.Pool = std::make_unique<PagePool>();
    state.Pool->Initialize(device, descriptor, increment);
    char delayText[16]{}, failureText[8]{};
    GetEnvironmentVariableA("DX12_VG_STREAMING_IO_DELAY_MS", delayText, sizeof(delayText));
    GetEnvironmentVariableA("DX12_VG_STREAMING_IO_FAIL_ONCE", failureText, sizeof(failureText));
    uint32_t delay = uint32_t(std::clamp(atoi(delayText), 0, 1000));
    bool injectFailure = atoi(failureText) != 0;
    auto failed = std::make_shared<std::atomic<bool>>(false);
    state.Io = std::make_unique<PageIo>([delay, injectFailure, failed](const auto& manifest, uint32_t page,
                                                                       std::span<uint8_t> bytes, std::string& error) {
        if (delay)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        if (injectFailure && !failed->exchange(true))
        {
            error = "Injected transient Page I/O failure";
            return false;
        }
        return VirtualGeometryPageCache::ReadPage(manifest, page, bytes, error);
    });
    state.Available = true;
}

RenderGraph::ResourceHandle Import(RenderGraph& graph, GpuSceneBuffer& buffer, const char* name,
                                   D3D12_RESOURCE_STATES final)
{
    auto found = graph.FindImportedResource(buffer.Resource());
    if (found.IsValid())
    {
        return found;
    }
    RenderGraph::ImportedResource input{};
    input.Name = name;
    input.Resource = buffer.Resource();
    input.InitialState = buffer.State;
    input.FinalState = final;
    input.HasFinalState = true;
    buffer.State = final;
    return graph.ImportResource(input);
}

void CollectFeedback()
{
    auto& state = Data();
    uint64_t completed = GraphicsDevice::GetCompletedFence();
    for (auto& frame : state.Frames)
    {
        if (!frame.Fence || frame.Fence > completed)
        {
            continue;
        }
        D3D12_RANGE range{0, SIZE_T(frame.ReadbackBytes)};
        void* mapped = nullptr;
        if (FAILED(frame.Readback->Map(0, &range, &mapped)))
        {
            throw std::runtime_error("Geometry feedback readback Map failed");
        }
        const auto* values = static_cast<const uint32_t*>(mapped);
        uint32_t count = std::min(values[0], RequestCapacity);
        const auto* usage = values + 4 + RequestCapacity;
        state.Stats.GpuRequests += values[0];
        state.Stats.RequestOverflow += values[1];
        for (uint32_t request = 0; request < count; ++request)
        {
            const uint32_t packed = values[4 + request];
            uint32_t page = packed & 0x7fffffffu;
            if (page < frame.Pages)
            {
                const bool predicted = (packed & 0x80000000u) != 0 && (usage[page / 32] & (1u << (page % 32))) == 0;
                if (predicted)
                {
                    ++state.Stats.PrefetchRequests;
                }
                const auto& record = state.Manager.Pages()[page];
                if (!predicted || (frame.Serial >= record.LastUsed &&
                                   (record.State == PageState::Evicted || record.State == PageState::Requested)))
                {
                    state.Manager.MarkPredicted(page, predicted);
                }
                if (state.Manager.Pages()[page].State == PageState::Evicted)
                {
                    ++state.Stats.CpuRequests;
                }
                state.Manager.Request(page, frame.Serial);
            }
        }
        for (uint32_t page = 0; page < frame.Pages; ++page)
        {
            if ((usage[page / 32] & (1u << (page % 32))) != 0)
            {
                state.Manager.MarkUsed(page, frame.Serial);
                state.Manager.MarkPredicted(page, false);
            }
        }
        D3D12_RANGE noWrite{0, 0};
        frame.Readback->Unmap(0, &noWrite);
        frame.Fence = 0;
    }
}
} // namespace

Settings& GetSettings()
{
    return Data().Options;
}

VGStreamingBudget GetBudget()
{
    const auto& settings = GetSettings();
    return {uint64_t(settings.BudgetMiB) * 1024 * 1024, uint64_t(settings.UploadMiBPerFrame) * 1024 * 1024,
            settings.MaxPageRequestsPerFrame};
}

bool InspectPage(uint32_t page, PageInfo& info)
{
    const auto& pages = Data().Manager.Pages();
    if (page >= pages.size())
    {
        return false;
    }
    const auto& record = pages[page];
    info = {static_cast<uint32_t>(record.State),
            record.Slot,
            record.Priority,
            record.LastUsed,
            record.Pinned,
            record.Retiring};
    return true;
}

bool SetBudget(const VGStreamingBudget& budget, std::string& error)
{
    constexpr uint64_t mib = 1024 * 1024;
    error.clear();
    if (budget.MaxResidentMemory < mib || budget.MaxResidentMemory > 1024 * mib || budget.MaxResidentMemory % mib ||
        budget.MaxUploadBytesPerFrame > 64 * mib || budget.MaxUploadBytesPerFrame % mib ||
        budget.MaxPageRequestsPerFrame > RequestCapacity)
    {
        error = "VG budget exceeds page-pool limits or is not aligned to one MiB";
        return false;
    }
    auto& settings = GetSettings();
    settings.BudgetMiB = static_cast<uint32_t>(budget.MaxResidentMemory / mib);
    settings.UploadMiBPerFrame = static_cast<uint32_t>(budget.MaxUploadBytesPerFrame / mib);
    settings.MaxPageRequestsPerFrame = budget.MaxPageRequestsPerFrame;
    return true;
}

const Statistics& GetStatistics()
{
    return Data().Stats;
}

bool Supported()
{
    Initialize();
    return Data().Available;
}

uint32_t PageCount()
{
    return Data().Manager.PageCount();
}

GpuSceneBuffer& ResidencyBuffer()
{
    return Current().Residency;
}

GpuSceneBuffer& RequestBitsBuffer()
{
    return Current().RequestBits;
}

GpuSceneBuffer& RequestBuffer()
{
    return Current().Requests;
}

GpuSceneBuffer& UsageBuffer()
{
    return Current().Usage;
}

uint32_t Register(std::shared_ptr<const VirtualGeometryPageCache::PageManifest> manifest, uint32_t priority)
{
    auto& state = Data();
    if (!manifest || !Supported())
    {
        return UINT32_MAX;
    }
    uint32_t base = state.Manager.PageCount();
    state.Sources.reserve(state.Sources.size() + manifest->Pages.size());
    const uint32_t effectivePriority = priority == UINT32_MAX ? manifest->StreamingPriority : priority;
    if (!state.Manager.AddPages(manifest->PinnedPages, uint64_t(state.Options.BudgetMiB) * 1024 * 1024,
                                effectivePriority))
    {
        return UINT32_MAX;
    }
    for (uint32_t page = 0; page < manifest->Pages.size(); ++page)
    {
        state.Sources.push_back({manifest, page});
    }
    return base;
}

bool CanRegister(const VirtualGeometryPageCache::PageManifest& manifest)
{
    auto& state = Data();
    return Supported() && manifest.Pages.size() == manifest.PinnedPages.size() &&
           state.Manager.CanAddPages(manifest.PinnedPages, uint64_t(state.Options.BudgetMiB) * 1024 * 1024);
}

bool RootsReady(uint32_t pageBase, const VirtualGeometryPageCache::PageManifest& manifest)
{
    const auto& snapshot = Data().Manager.Snapshot();
    for (uint32_t page = 0; page < manifest.Pages.size(); ++page)
    {
        if (manifest.PinnedPages[page] &&
            (uint64_t(pageBase) + page >= snapshot.size() || !snapshot[pageBase + page].Flags))
        {
            return false;
        }
    }
    return true;
}

void Prepare(RenderGraph& graph)
{
    auto& state = Data();
    auto begin = Clock::now();
    Initialize();
    ++state.Serial;
    CollectFeedback();
    if (state.Available)
    {
        uint64_t completedCopy = state.Pool->CompletedCopyFence();
        state.Pool->Collect();
        // 毎フレームの一時配列はstaticで使い回す（Prepareはメインスレッドからのみ呼ばれる）。
        static std::vector<uint32_t> released;
        released.clear();
        for (const auto& page : state.Manager.Pages())
        {
            if (page.Retiring && page.GraphicsFence <= GraphicsDevice::GetCompletedFence() &&
                page.CopyFence <= completedCopy)
            {
                released.push_back(page.Slot);
            }
        }
        state.Manager.Collect(GraphicsDevice::GetCompletedFence(), completedCopy);
        for (uint32_t slot : released)
        {
            state.Pool->Release(slot);
        }
        state.Options.BudgetMiB = std::clamp(state.Options.BudgetMiB, 1u, 1024u);
        state.Options.UploadMiBPerFrame = std::min(state.Options.UploadMiBPerFrame, 64u);
        state.Manager.SetBudget(uint64_t(state.Options.BudgetMiB) * 1024 * 1024);
        for (auto& result : state.Io->Poll())
        {
            --state.Outstanding;
            state.Stats.DiskBytes += result.ReadBytes;
            state.Stats.IoMilliseconds += result.Milliseconds;
            if (!state.Manager.Matches(result.Ticket))
            {
                continue;
            }
            if (!result.Error.empty())
            {
                ++state.Stats.IoErrors;
                auto& source = state.Sources[result.Ticket.Page];
                source.Failures = std::min(source.Failures + 1, 6u);
                source.RetryAfter = Clock::now() + std::chrono::seconds(std::min(30u, 1u << (source.Failures - 1)));
                state.Manager.Cancel(result.Ticket);
            }
            else
            {
                state.Ready.push_back(std::move(result));
            }
        }
        uint64_t transferred = 0;
        uint64_t limit = uint64_t(state.Options.UploadMiBPerFrame) * 1024 * 1024;
        while (!state.Ready.empty() && transferred + 65536 <= limit)
        {
            auto& result = state.Ready.front();
            if (state.Manager.Matches(result.Ticket))
            {
                uint64_t fence = state.Pool->Submit(result.Ticket, result.Payload);
                state.Manager.CompleteCopy(result.Ticket, fence);
                state.Sources[result.Ticket.Page].SubmittedAt = Clock::now();
                state.Stats.SubmittedBytes += 65536;
                transferred += 65536;
            }
            state.Ready.pop_front();
        }
        uint64_t previouslyResident = 0;
        for (uint32_t pageId = 0; pageId < state.Manager.PageCount(); ++pageId)
        {
            const auto& page = state.Manager.Pages()[pageId];
            if (page.State == PageState::Loading && page.CopyFence && page.CopyFence <= completedCopy)
            {
                previouslyResident += 65536;
                auto& source = state.Sources[pageId];
                state.Stats.CopyMilliseconds =
                    std::chrono::duration<double, std::milli>(Clock::now() - source.SubmittedAt).count();
                source.Failures = 0;
            }
        }
        state.Manager.Publish(completedCopy, state.Serial);
        state.Stats.CompletedBytes += previouslyResident;
        bool waiting = state.Manager.ReservedBytes() > state.Manager.Budget();
        uint32_t submittedRequests = 0;
        const uint32_t requestLimit = std::min(state.Options.MaxPageRequestsPerFrame, RequestCapacity);
        static std::vector<uint32_t> requestedPages;
        requestedPages.clear();
        const auto requestNow = Clock::now(); // ページごとのClock::now()呼び出しをやめる
        for (uint32_t page = 0; page < state.Manager.PageCount(); ++page)
        {
            const auto& source = state.Sources[page];
            const auto& record = state.Manager.Pages()[page];
            if (record.Pinned && record.State == PageState::Evicted && source.RetryAfter <= requestNow)
            {
                state.Manager.Request(page, state.Serial);
            }
            if (record.State == PageState::Requested && source.RetryAfter <= requestNow)
            {
                requestedPages.push_back(page);
            }
        }
        // 1フレームに発行できるのは requestLimit 件だけなので、全件ソートせず上位だけ並べる。
        const size_t orderedCount = std::min<size_t>(requestedPages.size(), size_t(requestLimit) + 64);
        std::partial_sort(requestedPages.begin(), requestedPages.begin() + orderedCount, requestedPages.end(),
                          [&](uint32_t left, uint32_t right) {
                              const auto& a = state.Manager.Pages()[left];
                              const auto& b = state.Manager.Pages()[right];
                              if (a.Pinned != b.Pinned)
                              {
                                  return a.Pinned;
                              }
                              if (a.Predicted != b.Predicted)
                              {
                                  return !a.Predicted;
                              }
                              if (a.LastUsed != b.LastUsed)
                              {
                                  return a.LastUsed > b.LastUsed;
                              }
                              if (a.Priority != b.Priority)
                              {
                                  return a.Priority > b.Priority;
                              }
                              return left < right;
                          });
        for (const uint32_t page : requestedPages)
        {
            const auto& source = state.Sources[page];
            if (submittedRequests >= requestLimit || state.Outstanding >= 128 || !state.Ready.empty())
            {
                break;
            }
            if (state.Manager.ReservedBytes() + 65536 > state.Manager.Budget())
            {
                waiting = true;
                break;
            }
            auto ticket = state.Manager.BeginLoad(page);
            if (!ticket)
            {
                waiting = true;
                continue;
            }
            if (state.Io->Submit(source.Manifest, *ticket, source.LocalPage))
            {
                ++state.Outstanding;
                ++submittedRequests;
            }
            else
            {
                state.Manager.Cancel(*ticket);
            }
        }
        if (waiting)
        {
            const uint64_t protection = std::max(1u, state.Options.RecentlyUsedProtectionFrames);
            auto oldest = state.Manager.OldestEvictable(state.Serial > protection ? state.Serial - protection : 0);
            if (oldest && state.Manager.Retire(*oldest, 0))
            {
                ++state.Stats.Evictions;
            }
        }
    }
    uint32_t frameIndex = GraphicsDevice::GetFrameIndex();
    auto& frame = Current();
    // The engine has already waited for reuse of this swap-chain frame slot.
    if (frame.Fence)
    {
        throw std::runtime_error("Geometry feedback slot reused before completion");
    }
    frame.Pages = state.Manager.PageCount();
    frame.Words = std::max(1u, (frame.Pages + 31) / 32);
    frame.Serial = state.Serial;
    auto ensure = [&](GpuSceneBuffer& buffer, uint32_t count, uint32_t stride, bool uav) {
        buffer.BeginFrame(frameIndex);
        buffer.Ensure(GraphicsDevice::GetDevice(), count, stride, frameIndex, L"Geometry streaming frame", uav);
    };
    ensure(frame.Residency, std::max(1u, frame.Pages), 16, false);
    ensure(frame.RequestBits, frame.Words, 4, true);
    ensure(frame.Usage, frame.Words, 4, true);
    ensure(frame.Requests, 4 + RequestCapacity, 4, true);
    std::vector<uint32_t> indices(frame.Pages);
    std::iota(indices.begin(), indices.end(), 0u);
    uint64_t bytes = frame.Residency.Stage(GraphicsDevice::GetDevice(), frameIndex, state.Manager.Snapshot().data(),
                                           frame.Pages, indices);
    auto snapshot =
        Import(graph, frame.Residency, "Geometry Residency Snapshot", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (bytes)
    {
        auto* target = &frame.Residency;
        graph.AddPass(
            "Geometry Residency Upload",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(snapshot, D3D12_RESOURCE_STATE_COPY_DEST);
            },
            [=](ID3D12GraphicsCommandList* commands) {
                target->Copy(commands, frameIndex);
            });
    }
    state.Manager.MarkSnapshotUsed(GraphicsDevice::GetPendingSubmissionFence());
    auto& stats = state.Stats;
    stats.Resident = stats.Requested = stats.Loading = stats.Evicted = stats.Pinned = 0;
    for (const auto& page : state.Manager.Pages())
    {
        stats.Pinned += page.Pinned;
        switch (page.State)
        {
        case PageState::Resident:
            ++stats.Resident;
            break;
        case PageState::Requested:
            ++stats.Requested;
            break;
        case PageState::Loading:
            ++stats.Loading;
            break;
        case PageState::Evicted:
            ++stats.Evicted;
            break;
        }
    }
    stats.ReservedBytes = state.Manager.ReservedBytes();
    stats.BudgetBytes = state.Manager.Budget();
    stats.PoolBytes = state.Available ? state.Pool->MemoryBytes() : 0;
    stats.UploadBytes = state.Available ? state.Pool->UploadBytes() : 0;
    stats.StagingBytes = state.Available ? state.Io->StagingBytes() + state.Ready.size() * 65536ull : 0;
    stats.CpuMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

void RecordMetadataBytes(uint64_t bytes)
{
    auto& stats = Data().Stats;
    for (const auto& frame : Data().Frames)
    {
        bytes += frame.Residency.MemoryBytes() + frame.RequestBits.MemoryBytes() + frame.Requests.MemoryBytes() +
                 frame.Usage.MemoryBytes() + frame.ReadbackBytes;
    }
    stats.MetadataBytes = bytes;
    RenderProfiler::GeometryStreamingStatistics snapshot;
    snapshot.GpuRequests = stats.GpuRequests;
    snapshot.CpuRequests = stats.CpuRequests;
    snapshot.RequestOverflow = stats.RequestOverflow;
    snapshot.DiskBytes = stats.DiskBytes;
    snapshot.SubmittedBytes = stats.SubmittedBytes;
    snapshot.CompletedBytes = stats.CompletedBytes;
    snapshot.ResidentPages = stats.Resident;
    snapshot.LoadingPages = stats.Loading;
    snapshot.RequestedPages = stats.Requested;
    snapshot.EvictedPages = stats.Evicted;
    snapshot.PoolBytes = stats.PoolBytes;
    snapshot.BudgetBytes = stats.BudgetBytes;
    snapshot.StagingBytes = stats.StagingBytes;
    snapshot.MetadataBytes = stats.MetadataBytes;
    snapshot.UploadBytes = stats.UploadBytes;
    snapshot.CpuMilliseconds = stats.CpuMilliseconds;
    snapshot.IoMilliseconds = stats.IoMilliseconds;
    snapshot.CopyMilliseconds = stats.CopyMilliseconds;
    RenderProfiler::RecordGeometryStreaming(snapshot);
}

void RecordFeedbackCopy(RenderGraph& graph)
{
    auto& frame = Current();
    uint64_t bytes = (4ull + RequestCapacity + frame.Words) * 4;
    if (frame.ReadbackBytes != bytes)
    {
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        frame.Readback.Reset();
        if (FAILED(GraphicsDevice::GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                        IID_PPV_ARGS(&frame.Readback))))
        {
            throw std::runtime_error("Geometry feedback readback allocation failed");
        }
        frame.ReadbackBytes = bytes;
    }
    auto requests = Import(graph, frame.Requests, "Geometry Page Requests", D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto usage = Import(graph, frame.Usage, "Geometry Page Usage", D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto* target = &frame;
    graph.AddPass(
        "Geometry Streaming Feedback",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(requests, D3D12_RESOURCE_STATE_COPY_SOURCE).Read(usage, D3D12_RESOURCE_STATE_COPY_SOURCE);
        },
        [=](ID3D12GraphicsCommandList* commands) {
            commands->CopyBufferRegion(target->Readback.Get(), 0, target->Requests.Resource(), 0,
                                       (4ull + RequestCapacity) * 4);
            commands->CopyBufferRegion(target->Readback.Get(), (4ull + RequestCapacity) * 4, target->Usage.Resource(),
                                       0, uint64_t(target->Words) * 4);
            target->Fence = GraphicsDevice::GetPendingSubmissionFence();
        });
}

D3D12_GPU_DESCRIPTOR_HANDLE PageDescriptors()
{
    auto* heap = FrameConstants::GetCbvHeap();
    auto handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += uint64_t(heap->GetDesc().NumDescriptors - MaximumSlots) * FrameConstants::GetCbvIncrementSize();
    return handle;
}

std::vector<RenderGraph::ResourceHandle> PageHandles(RenderGraph& graph)
{
    std::vector<RenderGraph::ResourceHandle> handles;
    auto& state = Data();
    for (const auto& page : state.Manager.Pages())
    {
        if (page.State != PageState::Resident)
        {
            continue;
        }
        auto* resource = state.Pool->Resource(page.Slot);
        auto found = graph.FindImportedResource(resource);
        if (!found.IsValid())
        {
            RenderGraph::ImportedResource input{};
            input.Name = "Geometry Resident Page";
            input.Resource = resource;
            input.InitialState = input.FinalState = D3D12_RESOURCE_STATE_COMMON;
            input.HasFinalState = true;
            found = graph.ImportResource(input);
        }
        handles.push_back(found);
    }
    return handles;
}

void Shutdown()
{
    auto& state = Data();
    if (state.Io)
    {
        state.Io->Shutdown();
    }
    if (state.Pool)
    {
        state.Pool->Shutdown();
    }
    state = {};
}
} // namespace VirtualGeometryStreaming
