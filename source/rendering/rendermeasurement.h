#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RenderMeasurement
{
inline constexpr uint32_t NoParent = UINT32_MAX;

inline bool IsGpuTimingComplete(uint32_t events, uint32_t resolvedEvents, uint32_t droppedEvents)
{
    return events != 0 && resolvedEvents == events && droppedEvents == 0;
}

inline uint32_t TraceStorageWidth(uint32_t width, bool checkerboard)
{
    return checkerboard ? width / 2 + width % 2 : width;
}

// Matches BLACK checkerboard packing in RayReflectionsCS.hlsl. The final
// column of an odd-sized image can map outside the image and must be skipped.
inline uint32_t TracePixelX(uint32_t storageX, uint32_t y, uint32_t sequence, bool checkerboard)
{
    return checkerboard ? storageX * 2 + ((y ^ sequence) & 1u) : storageX;
}

struct Sample
{
    uint64_t Serial = 0;
    double FrameMs = 0.0;
    double CpuMs = 0.0;
    double GpuMs = 0.0;
    bool GpuValid = false;
};

struct Summary
{
    size_t FrameSamples = 0;
    size_t CpuSamples = 0;
    size_t GpuSamples = 0;
    size_t DuplicateSamples = 0;
    std::optional<double> FrameMeanMs;
    std::optional<double> CpuMeanMs;
    std::optional<double> GpuMeanMs;
    std::optional<double> LowOnePercentFps;
};

inline Summary Summarize(const std::vector<Sample>& samples)
{
    Summary result;
    std::unordered_set<uint64_t> serials;
    std::vector<double> frameTimes;
    long double frameTotal = 0.0;
    long double cpuTotal = 0.0;
    long double gpuTotal = 0.0;
    for (const Sample& sample : samples)
    {
        if (!serials.insert(sample.Serial).second)
        {
            ++result.DuplicateSamples;
            continue;
        }
        if (std::isfinite(sample.FrameMs) && sample.FrameMs > 0.0)
        {
            frameTimes.push_back(sample.FrameMs);
            frameTotal += sample.FrameMs;
        }
        if (std::isfinite(sample.CpuMs) && sample.CpuMs >= 0.0)
        {
            cpuTotal += sample.CpuMs;
            ++result.CpuSamples;
        }
        if (sample.GpuValid && std::isfinite(sample.GpuMs) && sample.GpuMs >= 0.0)
        {
            gpuTotal += sample.GpuMs;
            ++result.GpuSamples;
        }
    }
    result.FrameSamples = frameTimes.size();
    if (!frameTimes.empty())
    {
        result.FrameMeanMs = static_cast<double>(frameTotal / frameTimes.size());
        std::sort(frameTimes.begin(), frameTimes.end(), std::greater<double>());
        const size_t slowCount = frameTimes.size() / 100 + (frameTimes.size() % 100 != 0 ? 1 : 0);
        long double slowTotal = 0.0;
        for (size_t index = 0; index < slowCount; ++index)
        {
            slowTotal += frameTimes[index];
        }
        result.LowOnePercentFps = static_cast<double>(1000.0L * slowCount / slowTotal);
    }
    if (result.CpuSamples != 0)
    {
        result.CpuMeanMs = static_cast<double>(cpuTotal / result.CpuSamples);
    }
    if (result.GpuSamples != 0)
    {
        result.GpuMeanMs = static_cast<double>(gpuTotal / result.GpuSamples);
    }
    return result;
}

struct EventInterval
{
    uint32_t Id = 0;
    uint32_t ParentId = NoParent;
    uint32_t QueueId = 0;
    uint64_t Begin = 0;
    uint64_t End = 0;
    bool Valid = false;
};

// Exclusive time is defined only within one queue. Child intervals are merged
// before subtraction, so overlapping siblings and grandchildren are not charged twice.
inline double ExclusiveTicks(const EventInterval& parent, const std::vector<EventInterval>& events)
{
    const double unavailable = std::numeric_limits<double>::quiet_NaN();
    if (!parent.Valid || parent.End < parent.Begin)
    {
        return unavailable;
    }

    std::vector<std::pair<uint64_t, uint64_t>> children;
    for (const EventInterval& event : events)
    {
        if (event.ParentId != parent.Id || event.QueueId != parent.QueueId)
        {
            continue;
        }
        if (!event.Valid || event.End < event.Begin)
        {
            return unavailable;
        }
        const uint64_t begin = (std::max)(parent.Begin, event.Begin);
        const uint64_t end = (std::min)(parent.End, event.End);
        if (end > begin)
        {
            children.emplace_back(begin, end);
        }
    }
    std::sort(children.begin(), children.end());
    uint64_t covered = 0;
    uint64_t rangeBegin = 0;
    uint64_t rangeEnd = 0;
    for (const auto& child : children)
    {
        if (child.first > rangeEnd)
        {
            covered += rangeEnd - rangeBegin;
            rangeBegin = child.first;
            rangeEnd = child.second;
        }
        else
        {
            rangeEnd = (std::max)(rangeEnd, child.second);
        }
    }
    covered += rangeEnd - rangeBegin;
    return static_cast<double>(parent.End - parent.Begin - covered);
}
} // namespace RenderMeasurement
