#include "proceduralmath.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <istream>
#include <ostream>
#include <iomanip>
#include <string>

namespace Procedural
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        uint32_t Hash(int x, int y, uint32_t seed)
        {
            uint32_t value = static_cast<uint32_t>(x) * 0x9e3779b9u;
            value ^= static_cast<uint32_t>(y) * 0x85ebca6bu;
            value ^= seed;
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            return value ^ (value >> 16);
        }

        float Random01(int x, int y, uint32_t seed)
        {
            return static_cast<float>(Hash(x, y, seed) >> 8) / 16777216.0f;
        }

        float Fade(float value)
        {
            return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
        }

        float Lerp(float first, float second, float weight)
        {
            return first + (second - first) * weight;
        }

        float Gradient(int x, int y, float offsetX, float offsetY, uint32_t seed)
        {
            const float diagonal = 0.70710678118f;
            switch (Hash(x, y, seed) & 7u)
            {
                case 0:
                    return offsetX;
                case 1:
                    return -offsetX;
                case 2:
                    return offsetY;
                case 3:
                    return -offsetY;
                case 4:
                    return (offsetX + offsetY) * diagonal;
                case 5:
                    return (offsetX - offsetY) * diagonal;
                case 6:
                    return (-offsetX + offsetY) * diagonal;
                default:
                    return (-offsetX - offsetY) * diagonal;
            }
        }

        float LatticeNoise(float x, float y, uint32_t seed, bool useGradient)
        {
            const int cellX = static_cast<int>(std::floor(x));
            const int cellY = static_cast<int>(std::floor(y));
            const float localX = x - cellX;
            const float localY = y - cellY;
            float corners[4]{};
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 2; ++column)
                {
                    if (useGradient)
                    {
                        corners[row * 2 + column] =
                            Gradient(cellX + column, cellY + row, localX - column, localY - row, seed) * 1.41421356237f;
                    }
                    else
                    {
                        corners[row * 2 + column] = Random01(cellX + column, cellY + row, seed) * 2.0f - 1.0f;
                    }
                }
            }
            const float lower = Lerp(corners[0], corners[1], Fade(localX));
            const float upper = Lerp(corners[2], corners[3], Fade(localX));
            return Lerp(lower, upper, Fade(localY));
        }

        float CellularNoise(float x, float y, uint32_t seed)
        {
            const int cellX = static_cast<int>(std::floor(x));
            const int cellY = static_cast<int>(std::floor(y));
            float nearestSquared = std::numeric_limits<float>::max();
            for (int row = -1; row <= 1; ++row)
            {
                for (int column = -1; column <= 1; ++column)
                {
                    const int sampleX = cellX + column;
                    const int sampleY = cellY + row;
                    const float deltaX = sampleX + Random01(sampleX, sampleY, seed) - x;
                    const float deltaY = sampleY + Random01(sampleX, sampleY, seed ^ 0xa511e9b3u) - y;
                    nearestSquared = (std::min)(nearestSquared, deltaX * deltaX + deltaY * deltaY);
                }
            }
            return 1.0f - 2.0f * std::clamp(std::sqrt(nearestSquared), 0.0f, 1.0f);
        }

        float FractalNoise(float x, float y, const BasicNoiseSettings& settings, bool ridged)
        {
            float total = 0.0f;
            float amplitude = 1.0f;
            float amplitudeSum = 0.0f;
            const int octaves = std::clamp(settings.Octaves, 1, 8);
            for (int octave = 0; octave < octaves; ++octave)
            {
                float sample = LatticeNoise(x, y, settings.Seed + static_cast<uint32_t>(octave) * 1013u, true);
                if (ridged)
                {
                    sample = 1.0f - std::abs(sample);
                    sample = sample * sample * 2.0f - 1.0f;
                }
                total += sample * amplitude;
                amplitudeSum += amplitude;
                amplitude *= std::clamp(settings.Persistence, 0.0f, 1.0f);
                x *= std::clamp(settings.Lacunarity, 1.0f, 4.0f);
                y *= std::clamp(settings.Lacunarity, 1.0f, 4.0f);
            }
            return total / amplitudeSum;
        }
    }

    static float SampleSingleNoise(float x, float y, const BasicNoiseSettings& settings)
    {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(settings.Frequency))
        {
            return 0.0f;
        }
        x *= std::clamp(settings.Frequency, 0.0001f, 2.0f);
        y *= std::clamp(settings.Frequency, 0.0001f, 2.0f);
        // 最大オクターブでも整数セルへの変換が定義範囲内に収まるようにする。
        x = std::clamp(x, -10000.0f, 10000.0f);
        y = std::clamp(y, -10000.0f, 10000.0f);
        float result = 0.0f;
        switch (settings.Method)
        {
            case NoiseMethod::Perlin:
                result = LatticeNoise(x, y, settings.Seed, true);
                break;
            case NoiseMethod::Value:
                result = LatticeNoise(x, y, settings.Seed, false);
                break;
            case NoiseMethod::Worley:
                result = CellularNoise(x, y, settings.Seed);
                break;
            case NoiseMethod::Fractal:
                result = FractalNoise(x, y, settings, false);
                break;
            case NoiseMethod::Ridged:
                result = FractalNoise(x, y, settings, true);
                break;
            case NoiseMethod::DomainWarp: {
                const float warp = std::clamp(settings.WarpStrength, 0.0f, 10.0f);
                const float warpedX = x + LatticeNoise(x + 17.3f, y + 9.2f, settings.Seed, true) * warp;
                const float warpedY = y + LatticeNoise(x - 7.1f, y + 31.7f, settings.Seed ^ 239u, true) * warp;
                result = FractalNoise(warpedX, warpedY, settings, false);
                break;
            }
            default:
                break;
        }
        return std::clamp(result, -1.0f, 1.0f);
    }

    void ApplyCommonNoiseParameters(NoiseSettings& settings)
    {
        for (auto& layer : settings.Layers)
        {
            const auto method = layer.Noise.Method;
            layer.Noise = settings;
            layer.Noise.Method = method;
        }
    }

    float SampleNoise(float x, float y, const NoiseSettings& settings)
    {
        if (settings.Method != NoiseMethod::Composite)
        {
            return SampleSingleNoise(x, y, settings);
        }

        double total = 0.0;
        double weightSum = 0.0;
        const int count = std::clamp(settings.LayerCount, 2, 5);
        for (int index = 0; index < count; ++index)
        {
            const auto& layer = settings.Layers[index];
            if (!std::isfinite(layer.Weight) || layer.Weight <= 0.0f)
            {
                continue;
            }
            total += SampleSingleNoise(x, y, layer.Noise) * static_cast<double>(layer.Weight);
            weightSum += layer.Weight;
        }
        return weightSum > 0.0 ? static_cast<float>(total / weightSum) : 0.0f;
    }

    std::vector<float> BuildHeightMap(const TerrainSettings& settings)
    {
        const int resolution = std::clamp(settings.Resolution, 4, 256);
        const int side = resolution + 1;
        const float size = std::clamp(settings.Size, 1.0f, 500.0f);
        const float height = std::clamp(settings.Height, 0.0f, 100.0f);
        std::vector<float> heights(static_cast<size_t>(side) * side);
        for (int row = 0; row < side; ++row)
        {
            for (int column = 0; column < side; ++column)
            {
                const float x = size * (static_cast<float>(column) / resolution - 0.5f);
                const float z = size * (static_cast<float>(row) / resolution - 0.5f);
                heights[row * side + column] = settings.BaseHeight + SampleNoise(x, z, settings.Noise) * height;
            }
        }
        return heights;
    }

    Vector3 SampleMotion(MotionMethod method, double time, float speed, const Vector3& amplitude, uint32_t seed)
    {
        if (!std::isfinite(time) || !std::isfinite(speed))
        {
            return {};
        }
        const double cycles = time * speed;
        const float phase = static_cast<float>(std::remainder(cycles, 1.0) * 2.0 * kPi);
        switch (method)
        {
            case MotionMethod::Wave:
                return {amplitude.X * std::sin(phase), amplitude.Y * std::sin(phase), amplitude.Z * std::sin(phase)};
            case MotionMethod::Orbit:
                return {amplitude.X * (std::cos(phase) - 1.0f), amplitude.Y * std::sin(phase),
                        amplitude.Z * std::sin(phase)};
            case MotionMethod::Noise: {
                NoiseSettings settings;
                settings.Method = NoiseMethod::Perlin;
                settings.Seed = seed;
                settings.Frequency = 1.0f;
                const float coordinate = static_cast<float>(std::remainder(cycles, 8192.0));
                return {amplitude.X * (SampleNoise(coordinate, 11.2f, settings) - SampleNoise(0.0f, 11.2f, settings)),
                        amplitude.Y * (SampleNoise(coordinate, 37.9f, settings) - SampleNoise(0.0f, 37.9f, settings)),
                        amplitude.Z * (SampleNoise(coordinate, 93.1f, settings) - SampleNoise(0.0f, 93.1f, settings))};
            }
            default:
                return {};
        }
    }

    bool TerrainSettingsEqual(const TerrainSettings& first, const TerrainSettings& second)
    {
        if (first.Noise.LayerCount != second.Noise.LayerCount)
        {
            return false;
        }
        for (size_t index = 0; index < first.Noise.Layers.size(); ++index)
        {
            const auto& a = first.Noise.Layers[index];
            const auto& b = second.Noise.Layers[index];
            if (a.Weight != b.Weight || a.Noise.Method != b.Noise.Method || a.Noise.Seed != b.Noise.Seed ||
                a.Noise.Frequency != b.Noise.Frequency || a.Noise.Octaves != b.Noise.Octaves ||
                a.Noise.Persistence != b.Noise.Persistence || a.Noise.Lacunarity != b.Noise.Lacunarity ||
                a.Noise.WarpStrength != b.Noise.WarpStrength)
            {
                return false;
            }
        }
        return first.Resolution == second.Resolution && first.Size == second.Size && first.Height == second.Height &&
               first.BaseHeight == second.BaseHeight && first.Noise.Method == second.Noise.Method &&
               first.Noise.Seed == second.Noise.Seed && first.Noise.Frequency == second.Noise.Frequency &&
               first.Noise.Octaves == second.Noise.Octaves && first.Noise.Persistence == second.Noise.Persistence &&
               first.Noise.Lacunarity == second.Noise.Lacunarity &&
               first.Noise.WarpStrength == second.Noise.WarpStrength;
    }

    bool WriteTerrainPreset(std::ostream& stream, const TerrainSettings& settings)
    {
        const auto writeNoise = [&stream](const BasicNoiseSettings& noise) {
            stream << static_cast<int>(noise.Method) << ' ' << noise.Seed << ' ' << noise.Frequency << ' '
                   << noise.Octaves << ' ' << noise.Persistence << ' ' << noise.Lacunarity << ' ' << noise.WarpStrength
                   << '\n';
        };
        stream << "PROCEDURAL_TERRAIN 2\n" << std::setprecision(9);
        writeNoise(settings.Noise);
        stream << settings.Resolution << ' ' << settings.Size << ' ' << settings.Height << ' ' << settings.BaseHeight
               << '\n';
        stream << settings.Noise.LayerCount << '\n';
        for (const auto& layer : settings.Noise.Layers)
        {
            writeNoise(layer.Noise);
            stream << layer.Weight << '\n';
        }
        return static_cast<bool>(stream);
    }

    bool ReadTerrainPreset(std::istream& stream, TerrainSettings& settings)
    {
        std::string signature;
        int version = 0;
        TerrainSettings loaded;
        const auto readNoise = [&stream](BasicNoiseSettings& noise, int maximumMethod) {
            int method = 0;
            if (!(stream >> method >> noise.Seed >> noise.Frequency >> noise.Octaves >> noise.Persistence >>
                  noise.Lacunarity >> noise.WarpStrength) ||
                method < 0 || method > maximumMethod || !std::isfinite(noise.Frequency) || noise.Frequency < 0.0001f ||
                noise.Frequency > 2.0f || noise.Octaves < 1 || noise.Octaves > 8 || !std::isfinite(noise.Persistence) ||
                noise.Persistence < 0.0f || noise.Persistence > 1.0f || !std::isfinite(noise.Lacunarity) ||
                noise.Lacunarity < 1.0f || noise.Lacunarity > 4.0f || !std::isfinite(noise.WarpStrength) ||
                noise.WarpStrength < 0.0f || noise.WarpStrength > 10.0f)
            {
                return false;
            }
            noise.Method = static_cast<NoiseMethod>(method);
            return true;
        };
        if (!(stream >> signature >> version) || signature != "PROCEDURAL_TERRAIN" || (version != 1 && version != 2) ||
            !readNoise(loaded.Noise, version == 1 ? 5 : 6) ||
            !(stream >> loaded.Resolution >> loaded.Size >> loaded.Height >> loaded.BaseHeight) ||
            loaded.Resolution < 4 || loaded.Resolution > 256 || !std::isfinite(loaded.Size) || loaded.Size < 1.0f ||
            loaded.Size > 500.0f || !std::isfinite(loaded.Height) || loaded.Height < 0.0f || loaded.Height > 100.0f ||
            !std::isfinite(loaded.BaseHeight) || std::abs(loaded.BaseHeight) > 1000.0f)
        {
            return false;
        }
        if (version == 2)
        {
            if (!(stream >> loaded.Noise.LayerCount) || loaded.Noise.LayerCount < 2 || loaded.Noise.LayerCount > 5)
            {
                return false;
            }
            for (auto& layer : loaded.Noise.Layers)
            {
                if (!readNoise(layer.Noise, 5) || !(stream >> layer.Weight) || !std::isfinite(layer.Weight) ||
                    layer.Weight < 0.0f || layer.Weight > 5.0f)
                {
                    return false;
                }
            }
        }
        else
        {
            ApplyCommonNoiseParameters(loaded.Noise);
        }
        settings = loaded;
        return true;
    }
} // 名前空間 Procedural の終わり。
