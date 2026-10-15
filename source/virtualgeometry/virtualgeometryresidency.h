#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace VirtualGeometryStreaming
{
using PageId = uint32_t;
struct LoadTicket
{
    PageId Page = UINT32_MAX;
    uint32_t Slot = UINT32_MAX;
    uint64_t Generation = 0;
    bool operator==(const LoadTicket&) const = default;
};
enum class PageState : uint8_t { Evicted, Requested, Loading, Resident };
struct ResidencyEntry
{
    uint32_t Slot = UINT32_MAX;
    uint32_t Generation = 0;
    uint32_t Flags = 0;
    uint32_t Padding = 0;
};
static_assert(sizeof(ResidencyEntry) == 16);
struct PageRecord
{
    PageState State = PageState::Evicted;
    bool Pinned = false;
    bool Retiring = false;
    uint32_t Slot = UINT32_MAX;
    uint64_t Generation = 0;
    uint64_t LastUsed = 0;
    uint64_t GraphicsFence = 0;
    uint64_t CopyFence = 0;
};
class ResidencyManager
{
public:
    bool AddPages(std::span<const uint8_t> pinned, uint64_t budget);
    bool CanAddPages(std::span<const uint8_t> pinned, uint64_t budget) const;
    void SetBudget(uint64_t bytes);
    void Request(PageId page, uint64_t frame);
    std::optional<LoadTicket> BeginLoad(PageId page);
    void CompleteCopy(const LoadTicket& ticket, uint64_t fence);
    void Publish(uint64_t completedCopy, uint64_t frame = 0);
    bool Retire(PageId page, uint64_t graphicsFence);
    void Cancel(const LoadTicket& ticket, uint64_t copyFence = 0);
    void Collect(uint64_t completedGraphics, uint64_t completedCopy);
    void MarkSnapshotUsed(uint64_t graphicsFence);
    void MarkUsed(PageId page, uint64_t frame);
    std::optional<PageId> OldestEvictable(uint64_t olderThanFrame = UINT64_MAX) const;
    bool Matches(const LoadTicket& ticket) const;
    const std::vector<ResidencyEntry>& Snapshot() const { return m_Snapshot; }
    const std::vector<PageRecord>& Pages() const { return m_Pages; }
    uint32_t PageCount() const { return uint32_t(m_Pages.size()); }
    uint64_t ReservedBytes() const;
    uint64_t Budget() const { return m_Budget; }
private:
    std::vector<PageRecord> m_Pages;
    std::vector<ResidencyEntry> m_Snapshot;
    std::vector<PageId> m_Slots;
    std::vector<uint64_t> m_Generations;
    uint64_t m_Budget = 256ull * 1024 * 1024;
    uint64_t m_ReservedPages = 0;
    uint64_t m_PinnedPages = 0;
    uint64_t m_AllocatedPinnedPages = 0;
};
}
