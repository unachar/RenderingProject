#pragma once
#include <cstdint>
#include <array>
#include <cstddef>

struct SkinningWork
{
    bool Base;
    bool Outlines;
};

inline SkinningWork PlanSkinningWork(uint64_t pose, uint64_t basePose, uint64_t outlinePose, bool wantsOutlines)
{
    return {pose != basePose, wantsOutlines && pose != outlinePose};
}

// A model can be drawn by multiple entities with different TEO presets. A single
// outline pose stamp incorrectly treats an uncomputed preset as ready.
template <std::size_t N>
inline uint32_t PlanOutlineSkinningModes(uint64_t pose, const std::array<uint64_t, N>& modePoses,
                                         uint32_t requestedModes)
{
    static_assert(N <= 32, "Outline mode mask must fit in uint32_t");
    uint32_t work = 0;
    for (std::size_t mode = 0; mode < N; ++mode)
    {
        const uint32_t bit = uint32_t{1} << mode;
        if ((requestedModes & bit) != 0 && modePoses[mode] != pose)
        {
            work |= bit;
        }
    }
    return work;
}
