#include "virtualgeometryresidency.h"
#include <algorithm>
#include <tuple>

namespace VirtualGeometryStreaming
{
namespace
{
constexpr uint64_t PageBytes = 65536;
constexpr uint64_t MaximumBudget = 1024ull * 1024 * 1024;
} // namespace

bool ResidencyManager::CanAddPages(std::span<const uint8_t> pinned, uint64_t budget) const
{
    budget = std::min(budget, MaximumBudget);
    uint64_t pinnedCount = m_PinnedPages + std::count(pinned.begin(), pinned.end(), uint8_t(1));
    if (pinnedCount > budget / PageBytes || pinned.size() > 0x7fffffffull - m_Pages.size() ||
        std::any_of(pinned.begin(), pinned.end(), [](uint8_t value) {
            return value > 1;
        }))
    {
        return false;
    }
    return true;
}

bool ResidencyManager::AddPages(std::span<const uint8_t> pinned, uint64_t budget, uint32_t priority)
{
    if (!CanAddPages(pinned, budget))
    {
        return false;
    }
    // Reserve all vectors before changing visible registration state.
    size_t count = m_Pages.size() + pinned.size();
    m_Pages.reserve(count);
    m_Snapshot.reserve(count);
    for (uint8_t value : pinned)
    {
        PageRecord page;
        page.Pinned = value != 0;
        page.Priority = std::min(2u, priority);
        m_PinnedPages += page.Pinned;
        m_Pages.push_back(page);
        m_Snapshot.emplace_back();
    }
    SetBudget(budget);
    return true;
}

void ResidencyManager::SetBudget(uint64_t bytes)
{
    m_Budget = std::min(bytes, MaximumBudget) / PageBytes * PageBytes;
}

uint64_t ResidencyManager::ReservedBytes() const
{
    return m_ReservedPages * PageBytes;
}

void ResidencyManager::Request(PageId page, uint64_t frame)
{
    if (page >= m_Pages.size())
    {
        return;
    }
    auto& record = m_Pages[page];
    record.LastUsed = std::max(record.LastUsed, frame);
    if (record.State == PageState::Evicted && !record.Retiring)
    {
        record.State = PageState::Requested;
    }
}

std::optional<LoadTicket> ResidencyManager::BeginLoad(PageId page)
{
    if (page >= m_Pages.size() || m_Pages[page].State != PageState::Requested || m_Pages[page].Retiring ||
        ReservedBytes() + PageBytes > m_Budget)
    {
        return std::nullopt;
    }
    // Preserve budget space for pinned pages that have not started loading.
    if (!m_Pages[page].Pinned)
    {
        uint64_t unallocatedPinned = m_PinnedPages - m_AllocatedPinnedPages;
        if (ReservedBytes() + (unallocatedPinned + 1) * PageBytes > m_Budget)
        {
            return std::nullopt;
        }
    }
    uint32_t slot = 0;
    while (slot < m_Slots.size() && (m_Slots[slot] != UINT32_MAX || m_Generations[slot] >= UINT32_MAX))
    {
        ++slot;
    }
    if (slot == m_Slots.size())
    {
        if (slot >= MaximumBudget / PageBytes)
        {
            return std::nullopt;
        }
        m_Slots.reserve(m_Slots.size() + 1);
        m_Generations.reserve(m_Generations.size() + 1);
        m_Slots.push_back(UINT32_MAX);
        m_Generations.push_back(0);
    }
    auto& record = m_Pages[page];
    record.Slot = slot;
    record.Generation = ++m_Generations[slot];
    record.CopyFence = 0;
    record.GraphicsFence = 0;
    record.State = PageState::Loading;
    m_Slots[slot] = page;
    ++m_ReservedPages;
    m_AllocatedPinnedPages += record.Pinned;
    return LoadTicket{page, slot, record.Generation};
}

bool ResidencyManager::Matches(const LoadTicket& ticket) const
{
    return ticket.Page < m_Pages.size() && m_Pages[ticket.Page].Slot == ticket.Slot &&
           m_Pages[ticket.Page].Generation == ticket.Generation && !m_Pages[ticket.Page].Retiring &&
           m_Pages[ticket.Page].State == PageState::Loading;
}

void ResidencyManager::CompleteCopy(const LoadTicket& ticket, uint64_t fence)
{
    if (Matches(ticket) && fence)
    {
        m_Pages[ticket.Page].CopyFence = fence;
    }
}

void ResidencyManager::Publish(uint64_t completedCopy, uint64_t frame)
{
    for (PageId index = 0; index < m_Pages.size(); ++index)
    {
        auto& record = m_Pages[index];
        if (record.State == PageState::Loading && !record.Retiring && record.CopyFence &&
            record.CopyFence <= completedCopy)
        {
            record.State = PageState::Resident;
            record.LastUsed = std::max(record.LastUsed, frame);
            m_Snapshot[index] = {record.Slot, uint32_t(record.Generation), 1, 0};
        }
    }
}

void ResidencyManager::MarkSnapshotUsed(uint64_t graphicsFence)
{
    for (auto& record : m_Pages)
    {
        if (record.State == PageState::Resident)
        {
            record.GraphicsFence = std::max(record.GraphicsFence, graphicsFence);
        }
    }
}

void ResidencyManager::MarkUsed(PageId page, uint64_t frame)
{
    if (page < m_Pages.size())
    {
        m_Pages[page].LastUsed = std::max(m_Pages[page].LastUsed, frame);
    }
}

bool ResidencyManager::Retire(PageId page, uint64_t graphicsFence)
{
    if (page >= m_Pages.size())
    {
        return false;
    }
    auto& record = m_Pages[page];
    if (record.Pinned || record.Retiring || record.State != PageState::Resident)
    {
        return false;
    }
    record.Retiring = true;
    record.State = PageState::Evicted;
    record.GraphicsFence = std::max(record.GraphicsFence, graphicsFence);
    m_Snapshot[page] = {};
    return true;
}

void ResidencyManager::Cancel(const LoadTicket& ticket, uint64_t copyFence)
{
    if (Matches(ticket))
    {
        auto& record = m_Pages[ticket.Page];
        record.CopyFence = std::max(record.CopyFence, copyFence);
        record.Retiring = true;
        record.State = PageState::Evicted;
        m_Snapshot[ticket.Page] = {};
    }
}

void ResidencyManager::Collect(uint64_t completedGraphics, uint64_t completedCopy)
{
    for (auto& record : m_Pages)
    {
        if (record.Retiring && record.GraphicsFence <= completedGraphics && record.CopyFence <= completedCopy)
        {
            m_Slots[record.Slot] = UINT32_MAX;
            --m_ReservedPages;
            m_AllocatedPinnedPages -= record.Pinned;
            record.Slot = UINT32_MAX;
            record.Retiring = false;
            record.CopyFence = 0;
            record.GraphicsFence = 0;
        }
    }
}

std::optional<PageId> ResidencyManager::OldestEvictable(uint64_t olderThanFrame) const
{
    std::optional<PageId> oldest;
    auto evictionOrder = [](const PageRecord& record) {
        return std::tuple{!record.Predicted, record.Priority, record.LastUsed};
    };
    for (PageId page = 0; page < m_Pages.size(); ++page)
    {
        const auto& record = m_Pages[page];
        if (!record.Pinned && !record.Retiring && record.State == PageState::Resident &&
            record.LastUsed < olderThanFrame && (!oldest || evictionOrder(record) < evictionOrder(m_Pages[*oldest])))
        {
            oldest = page;
        }
    }
    return oldest;
}

void ResidencyManager::MarkPredicted(PageId page, bool predicted)
{
    if (page < m_Pages.size())
    {
        m_Pages[page].Predicted = predicted;
    }
}
} // namespace VirtualGeometryStreaming
