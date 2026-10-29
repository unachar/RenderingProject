#include "models/skinningwork.h"
#include <cassert>
#include <cstdio>
#include <array>

int main()
{
    const auto shadow = PlanSkinningWork(1, UINT64_MAX, UINT64_MAX, false);
    assert(shadow.Base && !shadow.Outlines);
    const auto toon = PlanSkinningWork(1, 1, UINT64_MAX, true);
    assert(!toon.Base && toon.Outlines); // 後から輪郭線を追加するときも、前フレームの頂点を維持する。
    const auto cached = PlanSkinningWork(1, 1, 1, true);
    assert(!cached.Base && !cached.Outlines);
    const auto pbrNext = PlanSkinningWork(2, 1, 1, false);
    assert(pbrNext.Base && !pbrNext.Outlines);
    const auto toonNext = PlanSkinningWork(2, 2, 1, true);
    assert(!toonNext.Base && toonNext.Outlines);
    std::array<uint64_t, 4> outlinePoses{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    assert(PlanOutlineSkinningModes(1, outlinePoses, 0) == 0); // Extrude / shadows.
    assert(PlanOutlineSkinningModes(1, outlinePoses, 1u << 2) == (1u << 2));
    outlinePoses[2] = 1;
    assert(PlanOutlineSkinningModes(1, outlinePoses, 1u << 2) == 0);
    // Changing preset without changing pose needs just the new preset.
    assert(PlanOutlineSkinningModes(1, outlinePoses, 1u << 1) == (1u << 1));
    outlinePoses[1] = 1;
    assert(PlanOutlineSkinningModes(1, outlinePoses, (1u << 1) | (1u << 2)) == 0);
    // A second entity can use another preset on the same model and pose.
    assert(PlanOutlineSkinningModes(1, outlinePoses, 1u << 3) == (1u << 3));
    assert(PlanOutlineSkinningModes(2, outlinePoses, 1u << 2) == (1u << 2));
    assert(PlanOutlineSkinningModes(2, outlinePoses, 0xfffffff0u) == 0);
    std::puts("Skinning work: PASS (shadow/PBR/late-outline/pose-change)");
}
