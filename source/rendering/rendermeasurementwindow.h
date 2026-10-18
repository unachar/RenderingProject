#pragma once
#include "rendermeasurement.h"

namespace RenderMeasurement
{
// Completed GPU frames may arrive late or out of order. The captured CPU values
// travel in the same frame record; no timing is taken from the readback frame.
class Window final
{
public:
    Window(uint64_t firstSerial, uint32_t warmupFrames, uint32_t measuredFrames)
        : m_FirstSerial(firstSerial), m_Warmup(warmupFrames), m_Samples(measuredFrames)
    {
    }

    bool Record(const Sample& sample)
    {
        if (sample.Serial < m_FirstSerial) return false;
        const uint64_t relative = sample.Serial - m_FirstSerial;
        if (relative < m_Warmup) return false;
        const uint64_t index = relative - m_Warmup;
        if (index >= m_Samples.size() || m_Samples[static_cast<size_t>(index)]) return false;
        m_Samples[static_cast<size_t>(index)] = sample;
        ++m_Recorded;
        return true;
    }

    bool Complete() const { return m_Recorded == m_Samples.size(); }

    std::vector<Sample> Samples() const
    {
        std::vector<Sample> samples;
        samples.reserve(m_Recorded);
        for (const auto& sample : m_Samples)
            if (sample) samples.push_back(*sample);
        return samples;
    }

private:
    uint64_t m_FirstSerial;
    uint32_t m_Warmup;
    size_t m_Recorded = 0;
    std::vector<std::optional<Sample>> m_Samples;
};
}
