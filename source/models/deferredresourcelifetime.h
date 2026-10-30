#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Scene/history references own the resource; GPU work may outlive the last CPU reference.
template <typename Resource> class DeferredResourceLifetime
{
  public:
    explicit DeferredResourceLifetime(std::function<uint64_t()> pendingFence) : m_State(std::make_shared<State>())
    {
        m_State->PendingFence = std::move(pendingFence);
    }

    std::shared_ptr<Resource> Own(std::unique_ptr<Resource> resource)
    {
        return std::shared_ptr<Resource>(resource.release(), [state = m_State](Resource* released) {
            if (state->Active)
            {
                state->Retired.push_back({state->PendingFence(), std::unique_ptr<Resource>(released)});
            }
            else
            {
                delete released;
            }
        });
    }

    void Collect(uint64_t completedFence)
    {
        std::erase_if(m_State->Retired, [completedFence](const auto& item) {
            return item.first <= completedFence;
        });
    }

    // Call only after GPU idle, before the registry and component owners are destroyed.
    void Shutdown()
    {
        m_State->Active = false;
        m_State->Retired.clear();
    }

  private:
    struct State
    {
        std::function<uint64_t()> PendingFence;
        bool Active = true;
        std::vector<std::pair<uint64_t, std::unique_ptr<Resource>>> Retired;
    };

    // Undo snapshots can survive registry shutdown; their deleters must not reference a destroyed registry.
    std::shared_ptr<State> m_State;
};
