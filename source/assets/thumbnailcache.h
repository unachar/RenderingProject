#pragma once
#include "materialasset.h"
#include <optional>
#include <unordered_map>

enum class ThumbnailState : uint8_t
{
    Unloaded,
    Queued,
    Generating,
    Ready,
    Dirty,
    Failed
};
enum class ThumbnailPriority : uint8_t
{
    Selected,
    Visible,
    Nearby,
    Background
};
inline constexpr uint32_t ThumbnailGeneratorVersion = 1;

struct ThumbnailTicket
{
    AssetHandle Handle = 0;
    uint64_t Revision = 0;
};

struct ThumbnailRecord
{
    ThumbnailState State = ThumbnailState::Unloaded;
    ThumbnailPriority Priority = ThumbnailPriority::Background;
    uint64_t Revision = 0;
    uint64_t LastUsedFrame = 0;
    uint64_t Sequence = 0;
    uint64_t NextAttemptFrame = 0;
};

struct ThumbnailDimensions
{
    float Width = 0;
    float Height = 0;
};

ThumbnailDimensions FitThumbnail(uint32_t width, uint32_t height, float size);
std::string ThumbnailCacheFileName(AssetHandle handle);

// GPU-independent state machine. Generation tickets reject obsolete and deleted results.
class ThumbnailCache
{
  public:
    void BeginFrame(uint64_t frame)
    {
        m_Frame = frame;
    }

    const ThumbnailRecord& Request(AssetHandle handle, ThumbnailPriority priority = ThumbnailPriority::Visible);
    void MarkDirty(AssetHandle handle);
    void Remove(AssetHandle handle);
    std::optional<ThumbnailTicket> TakeNext();
    bool Complete(const ThumbnailTicket& ticket, bool success);
    bool Defer(const ThumbnailTicket& ticket, uint64_t delayFrames);
    const ThumbnailRecord* Find(AssetHandle handle) const;

  private:
    std::unordered_map<AssetHandle, ThumbnailRecord> m_Records;
    uint64_t m_Frame = 0;
    uint64_t m_Sequence = 0;
    uint64_t m_Revision = 0;
};
