#include "thumbnailcache.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

std::string ThumbnailCacheFileName(AssetHandle handle)
{
    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0') << handle << ".dds";
    return name.str();
}

ThumbnailDimensions FitThumbnail(uint32_t width, uint32_t height, float size)
{
    if (!width || !height || !std::isfinite(size) || size <= 0) return {};
    const float scale = size / static_cast<float>(std::max(width, height));
    return {width * scale, height * scale};
}

const ThumbnailRecord& ThumbnailCache::Request(AssetHandle handle, ThumbnailPriority priority)
{
    auto& record = m_Records[handle];
    if (record.LastUsedFrame != m_Frame) record.Priority = priority;
    else record.Priority = std::min(record.Priority, priority);
    record.LastUsedFrame = m_Frame;
    if (record.State == ThumbnailState::Unloaded || record.State == ThumbnailState::Dirty)
    {
        record.State = ThumbnailState::Queued;
        record.Revision = ++m_Revision;
        record.Sequence = ++m_Sequence;
    }
    return record;
}

void ThumbnailCache::MarkDirty(AssetHandle handle)
{
    const auto found = m_Records.find(handle);
    if (found == m_Records.end()) return;
    auto& record = found->second;
    record.Revision = ++m_Revision;
    record.State = record.LastUsedFrame == m_Frame ? ThumbnailState::Queued : ThumbnailState::Dirty;
    record.Sequence = ++m_Sequence;
    record.NextAttemptFrame = 0;
}

void ThumbnailCache::Remove(AssetHandle handle) { m_Records.erase(handle); }

std::optional<ThumbnailTicket> ThumbnailCache::TakeNext()
{
    auto best = m_Records.end();
    for (auto current = m_Records.begin(); current != m_Records.end(); ++current)
    {
        const auto& record = current->second;
        if (record.State != ThumbnailState::Queued || record.NextAttemptFrame > m_Frame) continue;
        const auto priority = record.LastUsedFrame == m_Frame ? record.Priority : ThumbnailPriority::Background;
        const auto bestPriority = best == m_Records.end() || best->second.LastUsedFrame != m_Frame ?
            ThumbnailPriority::Background : best->second.Priority;
        if (best == m_Records.end() || priority < bestPriority ||
            (priority == bestPriority && record.Sequence < best->second.Sequence)) best = current;
    }
    if (best == m_Records.end()) return std::nullopt;
    best->second.State = ThumbnailState::Generating;
    return ThumbnailTicket{best->first, best->second.Revision};
}

bool ThumbnailCache::Complete(const ThumbnailTicket& ticket, bool success)
{
    const auto found = m_Records.find(ticket.Handle);
    if (found == m_Records.end() || found->second.Revision != ticket.Revision ||
        found->second.State != ThumbnailState::Generating) return false;
    found->second.State = success ? ThumbnailState::Ready : ThumbnailState::Failed;
    return true;
}

bool ThumbnailCache::Defer(const ThumbnailTicket& ticket, uint64_t delayFrames)
{
    const auto found = m_Records.find(ticket.Handle);
    if (found == m_Records.end() || found->second.Revision != ticket.Revision ||
        found->second.State != ThumbnailState::Generating) return false;
    found->second.State = ThumbnailState::Queued;
    found->second.Sequence = ++m_Sequence;
    found->second.NextAttemptFrame = m_Frame + delayFrames;
    return true;
}

const ThumbnailRecord* ThumbnailCache::Find(AssetHandle handle) const
{
    const auto found = m_Records.find(handle);
    return found == m_Records.end() ? nullptr : &found->second;
}
