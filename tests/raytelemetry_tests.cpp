#include "rendering/raytelemetrydata.h"
#include <iostream>
#include <stdexcept>

void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try
    {
        using namespace RayTelemetry;
        std::array<uint32_t, RAY_TELEMETRY_CHANNEL_COUNT * 2> words{};
        words[0] = 32;
        words[1] = 1;
        const auto counts = Decode(words, 11, 11, true, Radiance, Radiance);
        Check(counts.Valid && counts.Rays[0] == (uint64_t(1) << 32) + 32, "counter carry is not truncated");
        Check(counts.ProviderKnown(Radiance), "instrumented producer is known");
        Check(counts.Rays[RAY_TELEMETRY_GI] == 0, "a measured zero is valid");
        Check(!Decode(words, 11, 12, true, Radiance, Radiance).Valid, "different serial cannot reuse readback");
        Check(!Decode(words, 11, 11, false, 0, 0).Valid, "disabled measurement is unavailable");
        const auto partial = Decode(words, 11, 11, true, Radiance | ShadowAo, Radiance);
        Check(partial.ProviderKnown(Radiance) && !partial.ProviderKnown(ShadowAo), "missing shader variant stays unavailable");
        Check(!counts.AllSourcesCovered, "partial provider coverage is not a total ray count");
        std::cout << "raytelemetry tests: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
