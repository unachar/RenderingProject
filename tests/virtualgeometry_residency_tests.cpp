#include "virtualgeometry/virtualgeometryresidency.h"
#include <stdexcept>

void TestVirtualGeometryResidency()
{
    using namespace VirtualGeometryStreaming;
    auto require = [](bool condition, const char* message) {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    };
    ResidencyManager manager;
    std::vector<uint8_t> pinned{1, 0, 0};
    require(manager.AddPages(pinned, 2 * 65536), "Pinned admission");
    manager.Request(0, 1);
    manager.Request(0, 1);
    manager.Request(1, 1);
    auto root = manager.BeginLoad(0);
    auto child = manager.BeginLoad(1);
    require(root && child, "Budget allocations");
    require(!manager.BeginLoad(2), "Budget exceeded");
    manager.CompleteCopy(*root, 2);
    manager.CompleteCopy(*child, 3);
    manager.Publish(2);
    require(manager.Snapshot()[0].Flags == 1 && manager.Snapshot()[1].Flags == 0, "Published unfinished Copy");
    manager.Publish(3);
    manager.MarkSnapshotUsed(7);
    require(!manager.Retire(0, 7), "Pinned page evicted");
    require(manager.Retire(1, 7), "Child retirement");
    require(manager.Snapshot()[1].Flags == 0, "Retired page in future snapshot");
    manager.Collect(6, 3);
    manager.Request(2, 2);
    require(!manager.BeginLoad(2), "Slot reused before graphics completion");
    manager.Collect(7, 2);
    require(!manager.BeginLoad(2), "Slot reused before Copy completion");
    manager.Collect(7, 3);
    auto replacement = manager.BeginLoad(2);
    require(bool(replacement), "Safe slot not reclaimed");
    require(replacement->Slot == child->Slot && replacement->Generation != child->Generation,
            "Slot generation unchanged");
    manager.CompleteCopy(*child, 8);
    manager.Publish(8);
    require(manager.Snapshot()[2].Flags == 0, "Stale completion published replacement");
    manager.CompleteCopy(*replacement, 9);
    manager.Publish(9);
    manager.SetBudget(65536);
    require(manager.Snapshot()[0].Flags == 1, "Budget shrink removed pinned root");
    require(!manager.AddPages(std::vector<uint8_t>{1}, 65536), "Pinned budget overflow admitted");
    require(manager.PageCount() == 3, "Failed admission partially registered");
    require(manager.Retire(2, 10), "Replacement retirement");
    manager.Collect(10, 9);
    require(manager.ReservedBytes() == 65536, "Retired bytes still reserved");
}
