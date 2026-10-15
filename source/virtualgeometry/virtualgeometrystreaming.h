#pragma once
#include "virtualgeometrypagecache.h"
#include "rendering/gpuscene/gpuscenebuffer.h"
#include "rendering/rendergraph.h"

namespace VirtualGeometryStreaming
{
inline constexpr uint32_t MaximumSlots = 16384;
inline constexpr uint32_t RequestCapacity = 65536;
struct Settings
{
    uint32_t BudgetMiB = 256;
    uint32_t UploadMiBPerFrame = 8;
};
struct Statistics
{
    uint64_t Resident = 0, Requested = 0, Loading = 0, Evicted = 0, Pinned = 0;
    uint64_t Evictions = 0, IoErrors = 0, RequestOverflow = 0;
    uint64_t GpuRequests = 0, CpuRequests = 0, DiskBytes = 0;
    uint64_t SubmittedBytes = 0, CompletedBytes = 0, ReservedBytes = 0, BudgetBytes = 0, StagingBytes = 0;
    uint64_t PoolBytes = 0, UploadBytes = 0;
    uint64_t MetadataBytes = 0;
    double CpuMilliseconds = 0, IoMilliseconds = 0, CopyMilliseconds = 0;
};
Settings& GetSettings();
const Statistics& GetStatistics();
// Returns a stable global Page base, or UINT32_MAX if pinned admission fails.
uint32_t Register(std::shared_ptr<const VirtualGeometryPageCache::PageManifest> manifest);
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
}
