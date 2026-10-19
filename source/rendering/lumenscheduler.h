#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace Lumen
{
constexpr uint32_t InvalidSlot = ~0u;
inline bool LightingChangeRequiresReset(bool changed, uint32_t quietFrames)
{
    return changed && quietFrames >= 8;
}
inline bool HasFreshLighting(uint32_t sequence, uint32_t lastUpdate, uint32_t maximumAge)
{
    return sequence - lastUpdate <= maximumAge;
}
inline bool CanReuseSurfaceLighting(uint32_t sequence, uint32_t lastUpdate, uint32_t lastSceneChange,
                                   bool changingScene)
{
    return changingScene ? HasFreshLighting(sequence, lastUpdate, 8) :
                           sequence - lastUpdate <= sequence - lastSceneChange;
}
struct Input
{
    uint64_t Owner = 0;
    uint64_t Revision = 0;
    float Distance = 0;
};
struct Work
{
    uint32_t Instance = 0;
    uint32_t Slot = 0;
};
struct Schedule
{
    std::vector<uint32_t> Slots;
    std::vector<Work> Captures, Updates;
};

// Ownership survives TLAS reordering. Only captured tiles appear in the lookup map.
class Scheduler
{
  public:
    explicit Scheduler(uint32_t capacity = 128) : m_Entries(capacity) {}

    Schedule Prepare(const std::vector<Input>& inputs, uint32_t captureBudget, uint32_t lightingBudget)
    {
        Schedule result;
        result.Slots.assign(inputs.size(), InvalidSlot);
        std::vector<uint32_t> desired;
        for (uint32_t i = 0; i < inputs.size(); ++i)
        {
            if (inputs[i].Owner != 0)
            {
                desired.push_back(i);
            }
        }
        std::stable_sort(desired.begin(), desired.end(), [&](uint32_t a, uint32_t b) {
            return inputs[a].Distance < inputs[b].Distance;
        });
        desired.resize((std::min)(desired.size(), m_Entries.size()));
        for (auto& entry : m_Entries)
        {
            entry.Instance = InvalidSlot;
            const auto found = std::find_if(desired.begin(), desired.end(), [&](uint32_t i) {
                return inputs[i].Owner == entry.Owner;
            });
            if (found == desired.end())
            {
                entry = {};
            }
        }
        for (uint32_t instance : desired)
        {
            const auto& input = inputs[instance];
            auto found = std::find_if(m_Entries.begin(), m_Entries.end(), [&](const Entry& entry) {
                return entry.Owner == input.Owner;
            });
            if (found == m_Entries.end())
            {
                found = std::find_if(m_Entries.begin(), m_Entries.end(), [](const Entry& entry) {
                    return entry.Owner == 0;
                });
            }
            if (found == m_Entries.end())
            {
                continue;
            }
            const uint32_t slot = static_cast<uint32_t>(found - m_Entries.begin());
            if (found->Owner != input.Owner || found->Revision != input.Revision)
            {
                found->Ready = false;
            }
            found->Owner = input.Owner;
            found->Revision = input.Revision;
            found->Instance = instance;
            if (!found->Ready && result.Captures.size() < captureBudget)
            {
                result.Captures.push_back({instance, slot});
                found->Ready = true;
            }
            if (found->Ready)
            {
                result.Slots[instance] = slot;
            }
        }
        for (uint32_t scanned = 0; scanned < m_Entries.size() && result.Updates.size() < lightingBudget; ++scanned)
        {
            const uint32_t slot = m_Next++ % static_cast<uint32_t>(m_Entries.size());
            const auto& entry = m_Entries[slot];
            const bool alreadyUpdated = std::any_of(result.Updates.begin(), result.Updates.end(), [&](Work work) {
                return work.Slot == slot;
            });
            if (entry.Ready && entry.Instance != InvalidSlot && !alreadyUpdated)
            {
                result.Updates.push_back({entry.Instance, slot});
            }
        }
        return result;
    }

  private:
    struct Entry
    {
        uint64_t Owner = 0, Revision = 0;
        uint32_t Instance = InvalidSlot;
        bool Ready = false;
    };
    std::vector<Entry> m_Entries;
    uint32_t m_Next = 0;
};
} // namespace Lumen
