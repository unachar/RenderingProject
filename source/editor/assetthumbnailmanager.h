#pragma once
#include "assets/thumbnailcache.h"
#include "thumbnailrenderer.h"

struct ThumbnailHandle
{
    AssetHandle Guid = 0;
    ThumbnailState State = ThumbnailState::Unloaded;
    uint64_t Revision = 0;
    std::shared_ptr<ThumbnailGpuImage> Image;
    D3D12_GPU_DESCRIPTOR_HANDLE GetImGuiTextureID() const { return Image ? Image->Srv() : D3D12_GPU_DESCRIPTOR_HANDLE{}; }
};

struct ThumbnailSettings
{
    UINT Size = 128;
    UINT MaxGenerationsPerFrame = 2;
    uint64_t MemoryBudget = 64ull * 1024 * 1024;
    bool DiskCache = true;
};

class AssetThumbnailManager
{
public:
    static ThumbnailHandle Request(AssetHandle guid, ThumbnailPriority priority = ThumbnailPriority::Visible);
    static void MarkDirty(AssetHandle guid);
    static void Remove(AssetHandle guid);
    static void BeginFrame(uint64_t frameIndex);
    static void ProcessGenerationBudget();
    static void Refresh();
    static void Shutdown();
    static ThumbnailSettings& Settings();
    static uint64_t MemoryBytes();
    static uint64_t CacheHits();
    static uint64_t LastCacheKey(AssetHandle guid);
};

