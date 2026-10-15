#include "models/skinningwork.h"
#include <cassert>
#include <cstdio>

int main()
{
    const auto shadow = PlanSkinningWork(1, UINT64_MAX, UINT64_MAX, false);
    assert(shadow.Base && !shadow.Outlines);
    const auto toon = PlanSkinningWork(1, 1, UINT64_MAX, true);
    assert(!toon.Base && toon.Outlines); // Keep previous vertices intact when adding an outline later.
    const auto cached = PlanSkinningWork(1, 1, 1, true);
    assert(!cached.Base && !cached.Outlines);
    const auto pbrNext = PlanSkinningWork(2, 1, 1, false);
    assert(pbrNext.Base && !pbrNext.Outlines);
    const auto toonNext = PlanSkinningWork(2, 2, 1, true);
    assert(!toonNext.Base && toonNext.Outlines);
    std::puts("Skinning work: PASS (shadow/PBR/late-outline/pose-change)");
}
