#pragma once
#include <cstdint>

struct SkinningWork
{
    bool Base;
    bool Outlines;
};

inline SkinningWork PlanSkinningWork(uint64_t pose, uint64_t basePose, uint64_t outlinePose, bool wantsOutlines)
{
    return {pose != basePose, wantsOutlines && pose != outlinePose};
}
