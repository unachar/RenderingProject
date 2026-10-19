#pragma once
#include "virtualgeometrypagecache.h"
#include "rendering/gpuscene/gpuscenebuffer.h"
#include "rendering/rendergraph.h"

struct VGStreamingBudget
{
    uint64_t MaxResidentMemory = 256ull * 1024 * 1024;
    uint64_t MaxUploadBytesPerFrame = 8ull * 1024 * 1024;
    uint32_t MaxPageRequestsPerFrame = 128;
};

namespace VirtualGeometryStreaming
{
    inline constexpr uint32_t MaximumSlots = 16384;
    inline constexpr uint32_t RequestCapacity = 65536;

    struct Settings
    {
        uint32_t BudgetMiB = 256;
        uint32_t UploadMiBPerFrame = 8;
        uint32_t MaxPageRequestsPerFrame = 128;
        uint32_t RecentlyUsedProtectionFrames = 3;
    };

    struct Statistics
    {
        uint64_t Resident = 0;
        uint64_t Requested = 0;
        uint64_t Loading = 0;
        uint64_t Evicted = 0;
        uint64_t Pinned = 0;
        uint64_t Evictions = 0;
        uint64_t IoErrors = 0;
        uint64_t RequestOverflow = 0;
        uint64_t GpuRequests = 0;
        uint64_t CpuRequests = 0;
        uint64_t DiskBytes = 0;
        uint64_t PrefetchRequests = 0;
        uint64_t SubmittedBytes = 0;
        uint64_t CompletedBytes = 0;
        uint64_t ReservedBytes = 0;
        uint64_t BudgetBytes = 0;
        uint64_t StagingBytes = 0;
        uint64_t PoolBytes = 0;
        uint64_t UploadBytes = 0;
        uint64_t MetadataBytes = 0;
        double CpuMilliseconds = 0;
        double IoMilliseconds = 0;
        double CopyMilliseconds = 0;
    };

    struct PageInfo
    {
        uint32_t State = 0;
        uint32_t Slot = UINT32_MAX;
        uint32_t Priority = 1;
        uint64_t LastUsed = 0;
        bool Pinned = false, Retiring = false;
    };

    bool InspectPage(uint32_t page, PageInfo& info);
    Settings& GetSettings();
    VGStreamingBudget GetBudget();
    // 現在のプールは常駐 1～1024 MiB、アップロード 0～64 MiB に対応する。単位は整数 MiB。
    bool SetBudget(const VGStreamingBudget& budget, std::string& error);
    const Statistics& GetStatistics();
    // 安定したグローバル Page 先頭位置を返す。常駐固定の受付に失敗したら UINT32_MAX。
    uint32_t Register(std::shared_ptr<const VirtualGeometryPageCache::PageManifest> manifest,
                      uint32_t priority = UINT32_MAX);
    bool CanRegister(const VirtualGeometryPageCache::PageManifest& manifest);
    void Prepare(RenderGraph& graph);
    void RecordFeedbackCopy(RenderGraph& graph);
    bool RootsReady(uint32_t pageBase, const VirtualGeometryPageCache::PageManifest& manifest);
    bool Supported();
    uint32_t PageCount();
    GpuSceneBuffer& ResidencyBuffer();
    GpuSceneBuffer& RequestBitsBuffer();
    GpuSceneBuffer& RequestBuffer();
    GpuSceneBuffer& UsageBuffer();
    D3D12_GPU_DESCRIPTOR_HANDLE PageDescriptors();
    std::vector<RenderGraph::ResourceHandle> PageHandles(RenderGraph& graph);
    void Shutdown();
    void RecordMetadataBytes(uint64_t bytes);
} // 名前空間 VirtualGeometryStreaming の終わり。
