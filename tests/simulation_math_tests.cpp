#include "../source/procedural/proceduralmath.h"
#include "../source/scene/solarcycle.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace
{
int g_Checks = 0;

void Check(bool passed, const char* message)
{
    ++g_Checks;
    if (!passed)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
}

int main()
{
    using namespace Procedural;
    for (int method = 0; method < 6; ++method)
    {
        NoiseSettings settings;
        settings.Method = static_cast<NoiseMethod>(method);
        settings.Octaves = 8;
        settings.Lacunarity = 4.0f;
        bool seedChangesResult = false;
        for (int sample = -100; sample < 100; ++sample)
        {
            const float x = sample * 1.37f;
            const float original = SampleNoise(x, -3.79f, settings);
            Check(std::isfinite(original) && original >= -1.0f && original <= 1.0f, "finite bounded noise");
            Check(original == SampleNoise(x, -3.79f, settings), "deterministic noise");
            NoiseSettings other = settings;
            other.Seed += 1;
            seedChangesResult |= original != SampleNoise(x, -3.79f, other);
        }
        Check(seedChangesResult, "seed affects every method");
    }
    NoiseSettings perlin;
    perlin.Method = NoiseMethod::Perlin;
    perlin.Frequency = 1.0f;
    Check(std::abs(SampleNoise(-0.00001f, 0.37f, perlin) - SampleNoise(0.00001f, 0.37f, perlin)) < 0.001f,
        "Perlin continuous across negative lattice boundary");
    Check(SampleNoise(std::numeric_limits<float>::infinity(), 0.0f, perlin) == 0.0f, "invalid noise coordinate");

    TerrainSettings terrain;
    terrain.Resolution = 12;
    const auto heights = BuildHeightMap(terrain);
    Check(heights.size() == 169, "terrain lattice dimensions");
    Check(heights == BuildHeightMap(terrain), "terrain reproducibility");
    for (float height : heights)
    {
        Check(height >= terrain.BaseHeight - terrain.Height && height <= terrain.BaseHeight + terrain.Height,
            "terrain height limits");
    }
    terrain.Height = 0.0f;
    for (float height : BuildHeightMap(terrain))
    {
        Check(height == terrain.BaseHeight, "zero amplitude flat terrain");
    }
    Check(SolarCycle::WrapHours(-1.0) == 23.0f && SolarCycle::WrapHours(49.0) == 1.0f, "clock wraps both directions");
    Check(SolarCycle::WrapHours(24.0 * 1000000.0 + 8.0) == 8.0f, "long-running clock");
    const auto morning = SolarCycle::Calculate(6.0f, 0.0f, 81);
    const auto noon = SolarCycle::Calculate(12.0f, 0.0f, 81);
    const auto evening = SolarCycle::Calculate(18.0f, 0.0f, 81);
    const auto night = SolarCycle::Calculate(0.0f, 0.0f, 81);
    Check(morning.East > 0.99f && std::abs(morning.Up) < 0.0001f, "sunrise in east");
    Check(noon.Up > 0.99f, "solar noon");
    Check(evening.East < -0.99f && std::abs(evening.Up) < 0.0001f, "sunset in west");
    Check(night.Up < -0.99f && SolarCycle::Daylight(night.Up) == 0.0f, "night daylight is zero");
    Check(SolarCycle::Calculate(12.0f, 35.0f, 172).Up > SolarCycle::Calculate(12.0f, 35.0f, 355).Up,
        "summer sun higher than winter");
    for (int hour = 0; hour < 24; ++hour)
    {
        const auto sun = SolarCycle::Calculate(static_cast<float>(hour), 35.0f, 172);
        const float length = sun.East * sun.East + sun.Up * sun.Up + sun.North * sun.North;
        Check(std::abs(length - 1.0f) < 0.00001f, "normalized solar direction");
    }
    for (int method = 0; method < 3; ++method)
    {
        const auto origin = SampleMotion(static_cast<MotionMethod>(method), 0.0, 0.2f, { 2.0f, 3.0f, 4.0f }, 42);
        Check(origin.X == 0.0f && origin.Y == 0.0f && origin.Z == 0.0f, "motion starts at authored origin");
        const auto sample = SampleMotion(static_cast<MotionMethod>(method), 123456.5, 0.2f, { 2.0f, 3.0f, 4.0f }, 42);
        Check(std::isfinite(sample.X) && std::isfinite(sample.Y) && std::isfinite(sample.Z), "long-running finite motion");
    }
    std::printf("PASS: %d simulation checks\n", g_Checks);
}
