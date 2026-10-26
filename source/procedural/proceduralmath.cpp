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
