#include "vertexdeformationmath.h"
#include <algorithm>
#include <cmath>

namespace Procedural
{
    namespace
    {
        constexpr double kTwoPi = 6.28318530717958647692;

        Vector3 Subtract(const Vector3& first, const Vector3& second)
        {
            return {first.X - second.X, first.Y - second.Y, first.Z - second.Z};
        }

        Vector3 AddScaled(const Vector3& position, const Vector3& direction, float distance)
        {
            return {position.X + direction.X * distance, position.Y + direction.Y * distance,
                    position.Z + direction.Z * distance};
        }

        Vector3 Cross(const Vector3& first, const Vector3& second)
        {
            return {first.Y * second.Z - first.Z * second.Y, first.Z * second.X - first.X * second.Z,
                    first.X * second.Y - first.Y * second.X};
        }

        Vector3 Normalize(const Vector3& normal, const Vector3& fallback)
        {
            const float lengthSquared = normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z;
            if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-16f)
            {
                return fallback;
            }
            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            return {normal.X * inverseLength, normal.Y * inverseLength, normal.Z * inverseLength};
        }

        float FiniteAmplitude(float amplitude)
        {
            if (!std::isfinite(amplitude))
            {
                return 0.0f;
            }
            return std::clamp(amplitude, -100.0f, 100.0f);
        }

        float SpatialFrequency(const VertexMotionSettings& settings)
        {
            if (!std::isfinite(settings.SpatialFrequency))
            {
                return 0.3f;
            }
            return std::clamp(settings.SpatialFrequency, 0.001f, 4.0f);
        }

        Vector3 SampleSpatialOffset(const Vector3& position, const VertexMotionSettings& settings, double time)
        {
            const double cycles = time * settings.Speed;
            if (!std::isfinite(cycles))
            {
                return {};
            }
            const double phase = std::remainder(cycles, 1.0) * kTwoPi;
            const double frequency = SpatialFrequency(settings);
            const double phaseX = (position.Y * 0.73 + position.Z) * frequency;
            const double phaseY = (position.X + position.Z * 0.61) * frequency;
            const double phaseZ = (position.X * 0.83 + position.Y) * frequency;
            switch (settings.Method)
            {
                case MotionMethod::Wave:
                    return {static_cast<float>(std::sin(phaseX + phase)), static_cast<float>(std::sin(phaseY + phase)),
                            static_cast<float>(std::sin(phaseZ + phase))};
                case MotionMethod::Orbit:
                    return {static_cast<float>(std::cos(phaseY + phase)), static_cast<float>(std::sin(phaseX + phase)),
                            static_cast<float>(std::sin(phaseZ + phase))};
                case MotionMethod::Noise: {
                    NoiseSettings noise;
                    noise.Method = NoiseMethod::Perlin;
                    noise.Seed = settings.Seed;
                    noise.Frequency = static_cast<float>(frequency);
                    const float coordinate = static_cast<float>(std::remainder(cycles, 8192.0));
                    return {SampleNoise(position.Y + coordinate, position.Z + 11.2f, noise),
                            SampleNoise(position.X + coordinate, position.Z + 37.9f, noise),
                            SampleNoise(position.X + 93.1f, position.Y + coordinate, noise)};
                }
                default:
                    return {};
            }
        }
    }

    Vector3 DeformPosition(const Vector3& basePosition, const VertexMotionSettings& settings, double time)
    {
        if (!std::isfinite(time) || !std::isfinite(settings.Speed))
        {
            return basePosition;
        }
        // 空間上の基準場を差し引き、時刻 0 ですべての元の頂点位置を正確に復元する。
        const Vector3 currentOffset = SampleSpatialOffset(basePosition, settings, time);
        const Vector3 restOffset = SampleSpatialOffset(basePosition, settings, 0.0);
        return {basePosition.X + FiniteAmplitude(settings.Amplitude.X) * (currentOffset.X - restOffset.X),
                basePosition.Y + FiniteAmplitude(settings.Amplitude.Y) * (currentOffset.Y - restOffset.Y),
                basePosition.Z + FiniteAmplitude(settings.Amplitude.Z) * (currentOffset.Z - restOffset.Z)};
    }

    Vector3 DeformNormal(const Vector3& basePosition, const Vector3& baseNormal, const VertexMotionSettings& settings,
                         double time)
    {
        const Vector3 unitNormal = Normalize(baseNormal, {0.0f, 1.0f, 0.0f});
        Vector3 reference = {0.0f, 1.0f, 0.0f};
        if (std::abs(unitNormal.Y) > 0.9f)
        {
            reference = {1.0f, 0.0f, 0.0f};
        }
        const Vector3 tangent = Normalize(Cross(reference, unitNormal), {0.0f, 0.0f, 1.0f});
        const Vector3 bitangent = Cross(unitNormal, tangent);
        // 両側のサンプリングで変形のヤコビ行列を近似し、元の滑らかな法線を保持する。
        const float sampleDistance = 0.001f / (std::max)(1.0f, SpatialFrequency(settings));
        const Vector3 deformedTangent =
            Subtract(DeformPosition(AddScaled(basePosition, tangent, sampleDistance), settings, time),
                     DeformPosition(AddScaled(basePosition, tangent, -sampleDistance), settings, time));
        const Vector3 deformedBitangent =
            Subtract(DeformPosition(AddScaled(basePosition, bitangent, sampleDistance), settings, time),
                     DeformPosition(AddScaled(basePosition, bitangent, -sampleDistance), settings, time));
        return Normalize(Cross(deformedTangent, deformedBitangent), unitNormal);
    }

    Vector3 GetDeformationBoundsPadding(const VertexMotionSettings& settings)
    {
        // 場のサンプルはすべて [-1, 1] なので、基準サンプルとの差の絶対値は 2 以下になる。
        return {std::abs(FiniteAmplitude(settings.Amplitude.X)) * 2.0f,
                std::abs(FiniteAmplitude(settings.Amplitude.Y)) * 2.0f,
                std::abs(FiniteAmplitude(settings.Amplitude.Z)) * 2.0f};
    }

    bool VertexMotionSettingsEqual(const VertexMotionSettings& first, const VertexMotionSettings& second)
    {
        return first.Method == second.Method && first.Amplitude.X == second.Amplitude.X &&
               first.Amplitude.Y == second.Amplitude.Y && first.Amplitude.Z == second.Amplitude.Z &&
               first.Speed == second.Speed && first.SpatialFrequency == second.SpatialFrequency &&
               first.Seed == second.Seed;
    }
} // 名前空間 Procedural の終わり。
