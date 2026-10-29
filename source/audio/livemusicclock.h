#pragma once
// Live music timing primitives. Independent of renderer, ECS and XAudio2.
// Times are seconds; beat positions are zero-based quarter notes.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace LiveMusic
{
struct TempoPoint
{
    double TimeSeconds = 0.0;
    double BPM = 120.0;
    double BeatPosition = 0.0; // accumulated quarter-note beats
};

struct BeatPosition
{
    double AbsoluteBeat = 0.0;
    uint64_t WholeBeat = 0;
    double Phase = 0.0;
    uint64_t Bar = 0;
    uint32_t BeatInBar = 0;
};

class TempoMap
{
  public:
    explicit TempoMap(double bpm = 120.0) { Set({{0.0, bpm, 0.0}}); }

    // Sorted, piecewise-constant BPM map. First point must be at time zero.
    void Set(std::vector<TempoPoint> points)
    {
        if (points.empty() || points.front().TimeSeconds != 0.0)
            throw std::invalid_argument("TempoMap requires a time-zero point");
        for (size_t i = 0; i < points.size(); ++i)
        {
            if (!std::isfinite(points[i].TimeSeconds) || !std::isfinite(points[i].BPM) ||
                points[i].TimeSeconds < 0.0 || points[i].BPM <= 0.0 ||
                (i && points[i].TimeSeconds <= points[i - 1].TimeSeconds))
                throw std::invalid_argument("Invalid tempo point");
            points[i].BeatPosition = i == 0 ? 0.0 :
                points[i - 1].BeatPosition +
                (points[i].TimeSeconds - points[i - 1].TimeSeconds) * points[i - 1].BPM / 60.0;
        }
        m_Points = std::move(points);
    }

    double SecondsToBeats(double seconds) const
    {
        if (!std::isfinite(seconds)) return 0.0;
        seconds = std::max(0.0, seconds);
        auto it = std::upper_bound(m_Points.begin(), m_Points.end(), seconds,
            [](double time, const TempoPoint& point) { return time < point.TimeSeconds; });
        if (it != m_Points.begin()) --it;
        return it->BeatPosition + (seconds - it->TimeSeconds) * it->BPM / 60.0;
    }

    double BeatsToSeconds(double beats) const
    {
        if (!std::isfinite(beats)) return 0.0;
        beats = std::max(0.0, beats);
        auto it = std::upper_bound(m_Points.begin(), m_Points.end(), beats,
            [](double beat, const TempoPoint& point) { return beat < point.BeatPosition; });
        if (it != m_Points.begin()) --it;
        return it->TimeSeconds + (beats - it->BeatPosition) * 60.0 / it->BPM;
    }

    BeatPosition Sample(double seconds, uint32_t beatsPerBar = 4) const
    {
        beatsPerBar = std::max(1u, beatsPerBar);
        const double beat = SecondsToBeats(seconds);
        const auto whole = static_cast<uint64_t>(std::floor(beat));
        return {beat, whole, beat - static_cast<double>(whole),
                whole / beatsPerBar, static_cast<uint32_t>(whole % beatsPerBar)};
    }

    const std::vector<TempoPoint>& Points() const { return m_Points; }

  private:
    std::vector<TempoPoint> m_Points;
};

// Stateless event crossing: no missed beats when a rendered frame spans multiple beats.
// On seek, do not replay past events: evaluate the full state at the new position instead.
template <typename F>
void ForEachCrossedBeat(const TempoMap& tempo, double previousSeconds,
                        double currentSeconds, F&& callback)
{
    if (!std::isfinite(previousSeconds) || !std::isfinite(currentSeconds) ||
        currentSeconds <= previousSeconds) return;
    const double prev = tempo.SecondsToBeats(previousSeconds);
    const double next = tempo.SecondsToBeats(currentSeconds);
    const uint64_t first = static_cast<uint64_t>(std::floor(prev)) + 1;
    const uint64_t last = static_cast<uint64_t>(std::floor(next));
    for (uint64_t beat = first; beat <= last && beat >= first; ++beat)
    {
        callback(beat, tempo.BeatsToSeconds(static_cast<double>(beat)));
        if (beat == UINT64_MAX) break;
    }
}
} // namespace LiveMusic
