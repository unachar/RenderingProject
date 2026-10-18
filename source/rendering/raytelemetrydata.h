#pragma once
#include "raytelemetrylayout.h"
#include <array>
#include <cstdint>

namespace RayTelemetry
{
enum Provider : uint32_t { Radiance = 1u, ShadowAo = 2u };

struct Counts
{
    uint64_t Serial = 0;
    std::array<uint64_t, RAY_TELEMETRY_CHANNEL_COUNT> Rays{};
    uint32_t ActiveProviders = 0;
    uint32_t MeasuredProviders = 0;
    bool Requested = false;
    bool Valid = false;
    bool AllSourcesCovered = false;

    bool ProviderKnown(uint32_t provider) const
    {
        return Valid && (ActiveProviders & provider & ~MeasuredProviders) == 0;
    }
};

inline Counts Decode(const std::array<uint32_t, RAY_TELEMETRY_CHANNEL_COUNT * 2>& words,
    uint64_t storedSerial, uint64_t expectedSerial, bool requested, uint32_t active, uint32_t measured)
{
    Counts counts;
    counts.Serial = expectedSerial;
    counts.Requested = requested;
    counts.ActiveProviders = active;
    counts.MeasuredProviders = measured;
    counts.Valid = requested && storedSerial == expectedSerial;
    if (counts.Valid)
        for (size_t channel = 0; channel < counts.Rays.size(); ++channel)
            counts.Rays[channel] = uint64_t(words[channel * 2]) | (uint64_t(words[channel * 2 + 1]) << 32);
    return counts;
}
}
