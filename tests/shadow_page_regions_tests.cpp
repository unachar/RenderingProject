#include "rendering/shadowpageregions.h"
#include <cassert>
#include <cstdio>
#include <set>
#include <utility>
#include <algorithm>
#include <cmath>

static void CheckCoverage(const std::vector<ShadowPages::Tile>& tiles)
{
    std::set<std::pair<unsigned, unsigned>> expected, actual;
    for (const auto& tile : tiles)
    {
        expected.emplace(tile.X, tile.Y);
    }
    for (const auto& region : ShadowPages::Merge(tiles))
    {
        for (unsigned y = 0; y < region.Height; ++y)
        {
            for (unsigned x = 0; x < region.Width; ++x)
            {
                assert(actual.emplace(region.First.X + x, region.First.Y + y).second);
                const auto tile = std::find_if(tiles.begin(), tiles.end(), [&](const auto& candidate) {
                    return candidate.X == region.First.X + x && candidate.Y == region.First.Y + y;
                });
                assert(tile != tiles.end());
                assert(tile->LogicalX == region.First.LogicalX + int(x));
                assert(tile->LogicalY == region.First.LogicalY + int(y));
                // 投影範囲とビューポートを広げても、アトラス上のテクセル位置は変わらない。
                for (double clip : {-1.0, -0.6, 0.0, 0.7, 1.0})
                {
                    const double mergedX = (clip + 2 * x - (region.Width - 1)) / region.Width;
                    const double mergedY = (clip - 2 * y + (region.Height - 1)) / region.Height;
                    assert(std::abs((mergedX + 1) * region.Width / 2 - (x + (clip + 1) / 2)) < 1e-10);
                    assert(std::abs((1 - mergedY) * region.Height / 2 - (y + (1 - clip) / 2)) < 1e-10);
                }
            }
        }
    }
    assert(actual == expected); // 更新不要なページをクリアせず、同じページを二重に描画しない。
}

int main()
{
    using namespace ShadowPages;
    assert(!NeedsStaticRefresh(true, true, true)); // 動くアクターの描画では、静止物体の深度を再利用する。
    assert(NeedsStaticRefresh(true, false, true)); // リングの再利用、またはカメラの変更。
    assert(NeedsStaticRefresh(true, true, false)); // 静的ジオメトリ、またはマテリアルの編集。
    assert(NeedsStaticRefresh(false, true, true)); // 明示的にキャッシュを使わない場合。
    for (uint64_t frame = 0; frame < 64; ++frame)
    {
        unsigned farLevelsDue = 0;
        for (unsigned level = 1; level < 4; ++level)
        {
            farLevelsDue += UpdateDue(frame, level);
        }
        assert(farLevelsDue <= 1); // 遠方のクリップマップの更新を1フレームに集中させない。
        assert(UpdateDue(frame, 0));
    }
    for (unsigned level = 0; level < 4; ++level)
    {
        unsigned updates = 0;
        for (uint64_t frame = 0; frame < 64; ++frame)
        {
            updates += UpdateDue(frame, level);
        }
        assert(updates == 64 / (1u << level)); // 既存の更新頻度を維持する。
    }
    assert(Merge({}).empty());
    std::vector<Tile> block;
    for (unsigned y = 0; y < 4; ++y)
    {
        for (unsigned x = 0; x < 4; ++x)
        {
            block.push_back({x + 2, y + 3, int(x) - 4, int(y) + 8, y * 4 + x});
        }
    }
    auto regions = Merge(block);
    assert(regions.size() == 1 && regions[0].Width == 4 && regions[0].Height == 4);
    CheckCoverage(block);
    block.erase(block.begin() + 5);
    CheckCoverage(block);
    // アトラス上で隣接していても、論理リングの継ぎ目をまたぐ座標は結合しない。
    std::vector<Tile> seam{{0, 0, 7, 0, 0}, {1, 0, 0, 0, 1}};
    assert(Merge(seam).size() == 2);
    CheckCoverage(seam);
    CheckCoverage({{7, 0, 7, 0, 0}, {0, 0, 8, 0, 1}, {1, 0, 9, 0, 2}});
    // 4×4の常駐領域について、更新が必要なページの全マスクを検証する。
    for (unsigned mask = 0; mask < 65536; ++mask)
    {
        std::vector<Tile> tiles;
        for (unsigned i = 0; i < 16; ++i)
        {
            if (mask & (1u << i))
            {
                tiles.push_back({i % 4, i / 4, int(i % 4), int(i / 4), i});
            }
        }
        CheckCoverage(tiles);
    }
    std::puts("Shadow page regions: PASS (65536 dirty masks, ring seams, exact coverage)");
}
