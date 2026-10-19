#include "rendering/lumenscheduler.h"
#include "rendering/lumencachemath.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace Lumen;
    Scheduler scheduler(3);
    std::vector<Input> inputs{{1, 10, 1}, {2, 20, 2}, {3, 30, 3}, {4, 40, 4}};
    auto first = scheduler.Prepare(inputs, 1, 1);
    assert(first.Captures.size() == 1 && first.Updates.size() == 1);
    assert(first.Slots[0] != InvalidSlot && first.Slots[1] == InvalidSlot);
    const auto slot = first.Slots[0];
    std::swap(inputs[0], inputs[1]);
    auto reordered = scheduler.Prepare(inputs, 1, 1);
    assert(reordered.Slots[1] == slot);
    assert(reordered.Captures.size() == 1 && reordered.Captures[0].Instance == 0);
    auto third = scheduler.Prepare(inputs, 1, 1);
    assert(third.Slots[2] != InvalidSlot && third.Slots[3] == InvalidSlot);
    assert(scheduler.Prepare(inputs, 1, 1).Captures.empty());
    inputs[1].Revision++;
    auto changed = scheduler.Prepare(inputs, 1, 1);
    assert(changed.Captures.size() == 1 && changed.Captures[0].Slot == slot);
    inputs.erase(inputs.begin() + 1);
    auto removed = scheduler.Prepare(inputs, 1, 1);
    assert(removed.Slots[2] != InvalidSlot);
    std::vector<bool> visited(3);
    for (int i = 0; i < 6; ++i)
    {
        auto frame = scheduler.Prepare(inputs, 1, 1);
        assert(frame.Updates.size() <= 1);
        for (auto work : frame.Updates)
        {
            visited[work.Slot] = true;
        }
    }
    assert(visited[0] && visited[1] && visited[2]);
    assert(scheduler.Prepare(inputs, 0, 0).Updates.empty());
    inputs[0].Owner = 0;
    assert(scheduler.Prepare(inputs, 1, 1).Slots[0] == InvalidSlot);
    assert(Scheduler(0).Prepare(inputs, 1, 1).Captures.empty());
    assert(AcceptSurfaceSample(0.001f, 0.05f, 1.0f, 0.1f));
    assert(!AcceptSurfaceSample(0.02f, 0.0f, 1.0f, 0.1f));
    assert(!AcceptSurfaceSample(0.0f, 0.0f, -1.0f, 0.1f));
    assert(!AcceptSurfaceSample(0.0f, 1.0f, 1.0f, 0.1f));
    assert(LightingChangeRequiresReset(true, 8));
    assert(!LightingChangeRequiresReset(true, 0));
    assert(!LightingChangeRequiresReset(false, 8));
    assert(HasFreshLighting(100, 96, 8));
    assert(!HasFreshLighting(100, 80, 8));
    assert(HasFreshLighting(2, 0xfffffffeu, 8));
    assert(!CanReuseSurfaceLighting(104, 80, 95, false));
    assert(CanReuseSurfaceLighting(104, 99, 95, false));
    assert(CanReuseSurfaceLighting(104, 99, 104, true));
    assert(!CanReuseSurfaceLighting(104, 80, 104, true));
    Scheduler animated(3);
    std::vector<Input> changing{{1, 1, 1}, {2, 1, 2}, {3, 1, 3}};
    for (int i = 0; i < 3; ++i)
    {
        animated.Prepare(changing, 1, 1);
    }
    std::vector<bool> updated(3);
    for (int i = 0; i < 12; ++i)
    {
        ++changing[2].Revision;
        const auto schedule = animated.Prepare(changing, 1, 1);
        for (auto work : schedule.Updates)
        {
            updated[work.Instance] = true;
        }
    }
    assert(updated[0] && updated[1] && updated[2]);
    std::cout << "Lumen scheduler: all tests passed\n";
}
