#include "../source/audio/livemusicclock.h"
#include <cassert>
#include <cmath>
#include <vector>
using namespace LiveMusic;
static bool Near(double a, double b) { return std::abs(a - b) < 1e-9; }
int main()
{
    TempoMap map(120);
    assert(Near(map.SecondsToBeats(0.5), 1.0));
    assert(Near(map.BeatsToSeconds(4.0), 2.0));
    auto position = map.Sample(2.75);
    assert(position.WholeBeat == 5 && position.BeatInBar == 1 && Near(position.Phase, 0.5));
    std::vector<uint64_t> beats;
    ForEachCrossedBeat(map, 0.2, 2.1, [&](uint64_t beat, double) { beats.push_back(beat); });
    assert((beats == std::vector<uint64_t>{1, 2, 3, 4}));
    map.Set({{0.0, 120.0, 0.0}, {2.0, 60.0, 0.0}});
    assert(Near(map.SecondsToBeats(3.0), 5.0));
    assert(Near(map.BeatsToSeconds(5.0), 3.0));
    assert(Near(map.BeatsToSeconds(4.0), 2.0));
    assert(Near(map.SecondsToBeats(-1.0), 0.0));
    std::vector<uint64_t> seek;
    ForEachCrossedBeat(map, 3.0, 1.0, [&](uint64_t b, double) { seek.push_back(b); });
    assert(seek.empty());
}
