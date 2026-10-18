#pragma once
#include <cstddef>
#include <vector>
#include <cstdint>

namespace ShadowPages
{
inline bool NeedsStaticRefresh(bool cacheEnabled, bool mappingMatches, bool staticContentMatches)
{
    return !cacheEnabled || !mappingMatches || !staticContentMatches;
}

inline bool UpdateDue(uint64_t frame, unsigned level)
{
    const uint64_t period = uint64_t(1) << level;
    return frame % period == period / 2;
}

struct Tile
{
    unsigned X, Y;
    int LogicalX, LogicalY;
    unsigned Pass;
};

struct Region
{
    Tile First;
    unsigned Width, Height;
};

inline std::vector<Region> Merge(const std::vector<Tile>& tiles)
{
    std::vector<Region> regions;
    std::vector<bool> used(tiles.size(), false);
    for (std::size_t i = 0; i < tiles.size(); ++i)
    {
        if (used[i])
        {
            continue;
        }
        const auto& first = tiles[i];
        auto find = [&](unsigned x, unsigned y) {
            for (std::size_t j = 0; j < tiles.size(); ++j)
            {
                if (!used[j] && tiles[j].X == first.X + x && tiles[j].Y == first.Y + y &&
                    tiles[j].LogicalX == first.LogicalX + int(x) && tiles[j].LogicalY == first.LogicalY + int(y))
                {
                    return j;
                }
            }
            return tiles.size();
        };
        unsigned width = 1, height = 1;
        while (find(width, 0) != tiles.size())
        {
            ++width;
        }
        for (;; ++height)
        {
            bool complete = true;
            for (unsigned x = 0; x < width; ++x)
            {
                if (find(x, height) == tiles.size())
                {
                    complete = false;
                    break;
                }
            }
            if (!complete)
            {
                break;
            }
        }
        for (unsigned y = 0; y < height; ++y)
        {
            for (unsigned x = 0; x < width; ++x)
            {
                used[find(x, y)] = true;
            }
        }
        regions.push_back({first, width, height});
    }
    return regions;
}
} // namespace ShadowPages
