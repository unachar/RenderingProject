#ifndef RAY_TELEMETRY_INCLUDED
#define RAY_TELEMETRY_INCLUDED
#include "../../source/rendering/raytelemetrylayout.h"
#ifndef ENABLE_RAY_TELEMETRY
#define ENABLE_RAY_TELEMETRY 0
#endif
#if ENABLE_RAY_TELEMETRY
RWByteAddressBuffer RayTelemetryCounters : register(u0, space9);

void CountTelemetryRay(uint channel)
{
    uint original;
    RayTelemetryCounters.InterlockedAdd(channel * 8, 1, original);
    if (original == 0xffffffffu)
    {
        uint ignored;
        RayTelemetryCounters.InterlockedAdd(channel * 8 + 4, 1, ignored);
    }
}
#else
void CountTelemetryRay(uint channel) {}
#endif
#endif
