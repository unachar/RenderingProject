#pragma once
#include <cstdint>
#include <optional>
#include <vector>

// Slots are reusable only after the submission that last referenced them completes.
class EditorDescriptorPool
{
  public:
    explicit EditorDescriptorPool(uint32_t count) : m_Slots(count) {}

    std::optional<uint32_t> Allocate()
    {
        for (uint32_t index = 0; index < m_Slots.size(); ++index)
        {
            if (m_Slots[index].State != State::Free)
            {
                continue;
            }
            m_Slots[index].State = State::Used;
            return index;
        }
        return std::nullopt;
    }

    bool Retire(uint32_t index, uint64_t fence)
    {
        if (index >= m_Slots.size() || m_Slots[index].State != State::Used)
        {
            return false;
        }
        m_Slots[index] = {State::Retired, fence};
        return true;
    }

    void Collect(uint64_t completed)
    {
        for (auto& slot : m_Slots)
        {
            if (slot.State == State::Retired && slot.Fence <= completed)
            {
                slot = {};
            }
        }
    }

    uint32_t Available() const
    {
        uint32_t count = 0;
        for (const auto& slot : m_Slots)
        {
            if (slot.State == State::Free)
            {
                ++count;
            }
        }
        return count;
    }

  private:
    enum class State
    {
        Free,
        Used,
        Retired
    };

    struct Slot
    {
        State State = State::Free;
        uint64_t Fence = 0;
    };

    std::vector<Slot> m_Slots;
};
