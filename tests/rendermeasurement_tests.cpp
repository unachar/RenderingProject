#include "rendering/rendermeasurement.h"
#include "rendering/rendermeasurementwindow.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Near(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-9;
}

void TestSummary()
{
    using namespace RenderMeasurement;
    const auto empty = Summarize({});
    Check(!empty.FrameMeanMs && !empty.LowOnePercentFps, "empty measurements are unavailable");
    std::vector<Sample> samples;
    for (uint64_t serial = 1; serial <= 100; ++serial)
        samples.push_back({serial, 10.0, 4.0, 5.0, true});
    samples.back().FrameMs = 100.0;
    samples.back().GpuValid = false;
    samples.push_back(samples.front());
    samples.push_back({101, std::numeric_limits<double>::quiet_NaN(), -1.0, 0.0, false});
    samples.push_back({102, 0.0, 0.0, std::numeric_limits<double>::infinity(), true});
    const auto result = Summarize(samples);
    Check(result.FrameSamples == 100 && result.CpuSamples == 101 && result.GpuSamples == 99,
        "each timing channel has its own valid count");
    Check(result.DuplicateSamples == 1, "duplicate serial is not counted twice");
    Check(Near(*result.FrameMeanMs, 10.9) && Near(*result.LowOnePercentFps, 10.0), "slowest one percent");
    Check(Near(*result.GpuMeanMs, 5.0), "missing GPU result is not zero");
    Check(Near(*Summarize({{1, 20.0, 0.0, 0.0, true}}).LowOnePercentFps, 50.0), "short run uses one sample");
    Check(Near(*Summarize({{1, 20.0, 0.0, 0.0, true}}).GpuMeanMs, 0.0), "valid zero duration is retained");
    samples.clear();
    for (uint64_t serial = 1; serial <= 101; ++serial)
        samples.push_back({serial, serial == 101 ? 100.0 : 10.0, 0.0, 0.0, false});
    Check(Near(*Summarize(samples).LowOnePercentFps, 1000.0 / 55.0), "one percent rounds upward");
}

void TestIntervals()
{
    using namespace RenderMeasurement;
    const EventInterval parent{0, NoParent, 0, 0, 100, true};
    std::vector<EventInterval> events{parent, {1, 0, 0, 10, 40, true}, {2, 1, 0, 20, 30, true}};
    Check(Near(ExclusiveTicks(parent, events), 70.0), "grandchild is not subtracted twice");
    Check(Near(ExclusiveTicks(events[1], events), 20.0), "child exclusive time");
    events.push_back({3, 0, 0, 30, 60, true});
    Check(Near(ExclusiveTicks(parent, events), 50.0), "overlapping children use union");
    events.push_back({4, 0, 1, 0, 100, true});
    Check(Near(ExclusiveTicks(parent, events), 50.0), "other queues are not subtracted");
    events.push_back({5, 999, 0, 0, 100, true});
    Check(Near(ExclusiveTicks(parent, events), 50.0), "missing parent is not attached implicitly");
    events.push_back({6, 0, 0, 90, 120, true});
    Check(Near(ExclusiveTicks(parent, events), 40.0), "child is clipped to parent");
    events.push_back({7, 0, 0, 10, 10, false});
    Check(std::isnan(ExclusiveTicks(parent, events)), "unfinished child makes exclusive unavailable");
    Check(std::isnan(ExclusiveTicks({8, NoParent, 0, 5, 1, true}, {})), "backward parent is invalid");
    Check(std::isnan(ExclusiveTicks({8, NoParent, 0, 0, 1, false}, {})), "invalid parent is unavailable");
    Check(Near(ExclusiveTicks({9, NoParent, 0, 0, 0, true}, {}), 0.0), "zero interval is valid");
}

void TestPackedCheckerboard()
{
    using namespace RenderMeasurement;
    Check(TraceStorageWidth(5, true) == 3 && TraceStorageWidth(5, false) == 5, "odd trace storage width");
    Check(TracePixelX(0, 0, 0, true) == 0, "black parity");
    Check(TracePixelX(0, 1, 0, true) == 1, "alternate row parity");
    Check(TracePixelX(0, 0, 1, true) == 1, "alternate frame parity");
    Check(TracePixelX(2, 1, 0, true) == 5, "odd edge is outside the active image");
    Check(TracePixelX(2, 1, 1, false) == 2, "full resolution storage mapping");
}

void TestIncompleteTiming()
{
    using namespace RenderMeasurement;
    Check(!IsGpuTimingComplete(0, 0, 0), "no GPU events is unavailable");
    Check(IsGpuTimingComplete(2, 2, 0), "all same-name events were resolved");
    Check(!IsGpuTimingComplete(2, 1, 0), "partial same-name sum is unavailable");
    Check(!IsGpuTimingComplete(2, 2, 1), "query overflow prevents a complete per-pass sum");
}

void TestDelayedMeasurementWindow()
{
    using namespace RenderMeasurement;
    Window window(10, 2, 3);
    Check(!window.Record({11, 10, 2, 3, true}), "warmup is not measured");
    Check(window.Record({14, 12, 2, 3, true}), "last delayed GPU result is accepted");
    Check(!window.Record({14, 999, 2, 3, true}), "duplicate readback is ignored");
    Check(window.Record({12, 10, 2, 0, false}), "missing GPU timing remains an explicit sample");
    Check(!window.Complete(), "missing middle serial prevents completion");
    Check(window.Record({13, 11, 2, 4, true}) && window.Complete(), "out-of-order readback joins by serial");
    Check(!window.Record({15, 10, 2, 3, true}), "drain frames are not measured");
    const auto samples = window.Samples();
    Check(samples.size() == 3 && samples.front().Serial == 12 && samples.back().Serial == 14,
        "measurement results are returned in serial order");
    Check(Summarize(samples).GpuSamples == 2, "missing GPU value does not enter mean");
    Window empty(1, 0, 0);
    Check(empty.Complete() && empty.Samples().empty(), "zero-length capture is bounded");
}
}

int main()
{
    try
    {
        TestSummary();
        TestIntervals();
        TestPackedCheckerboard();
        TestIncompleteTiming();
        TestDelayedMeasurementWindow();
        std::cout << "rendermeasurement tests: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
