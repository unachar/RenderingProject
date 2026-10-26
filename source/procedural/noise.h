#pragma once

// CPU noise shared by terrain, deformation and Procedural Studio.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Procedural
{
    enum class NoiseMethod
    {
        Perlin,
        Value,
        Worley,
        Fractal,
        Ridged,
        DomainWarp,
        Composite
    };

    struct BasicNoiseSettings
    {
        NoiseMethod Method = NoiseMethod::Fractal;
        uint32_t Seed = 42;
        float Frequency = 0.08f;
        int Octaves = 5;
        float Persistence = 0.5f;
        float Lacunarity = 2.0f;
        float WarpStrength = 1.5f;
    };

    struct NoiseLayer
    {
        BasicNoiseSettings Noise;
        float Weight = 1.0f;
    };

    struct NoiseSettings : BasicNoiseSettings
    {
        int LayerCount = 2;
        std::array<NoiseLayer, 5> Layers;
    };

    namespace NoiseDetail
    {
        inline uint32_t Hash(int x, int y, uint32_t seed)
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

        inline float Random01(int x, int y, uint32_t seed)
        {
            return static_cast<float>(Hash(x, y, seed) >> 8) / 16777216.0f;
        }

        inline float Fade(float value)
        {
            return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
        }

        inline float Lerp(float first, float second, float weight)
        {
            return first + (second - first) * weight;
        }

        inline float Gradient(int x, int y, float offsetX, float offsetY, uint32_t seed)
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

        inline float LatticeNoise(float x, float y, uint32_t seed, bool useGradient)
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

        inline float CellularNoise(float x, float y, uint32_t seed)
        {
            const int cellX = static_cast<int>(std::floor(x));
            const int cellY = static_cast<int>(std::floor(y));
            float nearestSquared = (std::numeric_limits<float>::max)();
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

        inline float FractalNoise(float x, float y, const BasicNoiseSettings& settings, bool ridged)
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

    inline float SampleSingleNoise(float x, float y, const BasicNoiseSettings& settings)
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
                result = NoiseDetail::LatticeNoise(x, y, settings.Seed, true);
                break;
            case NoiseMethod::Value:
                result = NoiseDetail::LatticeNoise(x, y, settings.Seed, false);
                break;
            case NoiseMethod::Worley:
                result = NoiseDetail::CellularNoise(x, y, settings.Seed);
                break;
            case NoiseMethod::Fractal:
                result = NoiseDetail::FractalNoise(x, y, settings, false);
                break;
            case NoiseMethod::Ridged:
                result = NoiseDetail::FractalNoise(x, y, settings, true);
                break;
            case NoiseMethod::DomainWarp: {
                const float warp = std::clamp(settings.WarpStrength, 0.0f, 10.0f);
                const float warpedX = x + NoiseDetail::LatticeNoise(x + 17.3f, y + 9.2f, settings.Seed, true) * warp;
                const float warpedY =
                    y + NoiseDetail::LatticeNoise(x - 7.1f, y + 31.7f, settings.Seed ^ 239u, true) * warp;
                result = NoiseDetail::FractalNoise(warpedX, warpedY, settings, false);
                break;
            }
            default:
                break;
        }
        return std::clamp(result, -1.0f, 1.0f);
    }

    inline float SampleNoise(float x, float y, const NoiseSettings& settings)
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

}

namespace ProcStudio
{
    enum class Basis
    {
        Perlin,
        Value,
        Worley,
        CellEdge, // Worleyノイズの距離差（F2 - F1）
        White,
        Simplex,
        Voronoi,
        Cell,
        Gabor,
        Wavelet,
        Curl,
        ValueGradient,
        SparseConvolution,
        Spot,
        LocalRandom,
        Chevron,
        LatticeFree,
        Phasor,
        AnisotropicGabor,
        Harmonic,
        Flow,
        Ridged, // Single-octave ridge; FractalType::Ridged is the feedback multifractal.
        Count
    };
    enum class FractalType
    {
        FBm,
        Ridged,
        Billow,
        Turbulence,
        HybridMultifractal,
        DomainWarp,
        None,
        Multifractal,
        HeterogeneousMultifractal,
        Count
    };

    struct NoiseParams
    {
        Basis BasisType = Basis::Perlin;
        FractalType Fractal = FractalType::FBm;
        uint32_t Seed = 1337;
        float Frequency = 4.0f; // テクスチャ全体のセル数。立体テクスチャではワールド空間 1 単位あたりのセル数。
        int Octaves = 5;
        float Lacunarity = 2.0f;
        float Gain = 0.5f;
        float WarpStrength = 0.6f;
        float RidgeOffset = 1.0f;
        float Direction = 0.0f;       // Radians in the XY plane for directional kernels.
        float Anisotropy = 4.0f;      // Transverse kernel compression, [1, 8].
        float KernelFrequency = 2.0f; // Oscillations per kernel radius.
        float Phase = 0.0f;           // Radians; flow rotates lattice gradients continuously.
    };

    namespace NoiseDetail
    {
        inline uint32_t Hash(uint32_t value)
        {
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            return value ^ (value >> 16);
        }

        inline uint32_t Hash3(int x, int y, int z, uint32_t seed)
        {
            return Hash(
                static_cast<uint32_t>(x) * 0x8da6b343u ^
                Hash(static_cast<uint32_t>(y) * 0xd8163841u ^ Hash(static_cast<uint32_t>(z) * 0xcb1ab31fu ^ seed)));
        }

        inline float Unit(uint32_t h)
        {
            return static_cast<float>(h >> 8) / 16777216.0f;
        }

        inline float Fade(float t)
        {
            return t * t * t * (t * (t * 6 - 15) + 10);
        }

        inline float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        inline float Saturate(float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        inline float Smooth(float a, float b, float v)
        {
            const float t = Saturate((v - a) / (std::max)(b - a, 1e-6f));
            return t * t * (3 - 2 * t);
        }

        inline int Wrap(int value, int period)
        {
            if (period <= 0)
            {
                return value;
            }
            const int m = value % period;
            return m < 0 ? m + period : m;
        }

        inline float GradientDot(uint32_t h, float x, float y, float z)
        {
            // 立方体の辺方向に沿う 12 個の勾配
            switch (h % 12u)
            {
                case 0:
                    return x + y;
                case 1:
                    return -x + y;
                case 2:
                    return x - y;
                case 3:
                    return -x - y;
                case 4:
                    return x + z;
                case 5:
                    return -x + z;
                case 6:
                    return x - z;
                case 7:
                    return -x - z;
                case 8:
                    return y + z;
                case 9:
                    return -y + z;
                case 10:
                    return y - z;
                default:
                    return -y - z;
            }
        }

        inline float Perlin(float x, float y, float z, uint32_t seed, int period)
        {
            const int ix = static_cast<int>(std::floor(x));
            const int iy = static_cast<int>(std::floor(y));
            const int iz = static_cast<int>(std::floor(z));
            const float fx = x - ix;
            const float fy = y - iy;
            const float fz = z - iz;
            const float u = Fade(fx);
            const float v = Fade(fy);
            const float w = Fade(fz);
            auto corner = [&](int dx, int dy, int dz) {
                const uint32_t h = Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                return GradientDot(h, fx - dx, fy - dy, fz - dz);
            };
            const float x00 = Lerp(corner(0, 0, 0), corner(1, 0, 0), u);
            const float x10 = Lerp(corner(0, 1, 0), corner(1, 1, 0), u);
            const float x01 = Lerp(corner(0, 0, 1), corner(1, 0, 1), u);
            const float x11 = Lerp(corner(0, 1, 1), corner(1, 1, 1), u);
            return std::clamp(Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w) * 0.95f, -1.0f, 1.0f);
        }

        inline float ValueNoise(float x, float y, float z, uint32_t seed, int period)
        {
            const int ix = static_cast<int>(std::floor(x));
            const int iy = static_cast<int>(std::floor(y));
            const int iz = static_cast<int>(std::floor(z));
            const float u = Fade(x - ix);
            const float v = Fade(y - iy);
            const float w = Fade(z - iz);
            auto corner = [&](int dx, int dy, int dz) {
                return Unit(Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed)) * 2 - 1;
            };
            const float x00 = Lerp(corner(0, 0, 0), corner(1, 0, 0), u);
            const float x10 = Lerp(corner(0, 1, 0), corner(1, 1, 0), u);
            const float x01 = Lerp(corner(0, 0, 1), corner(1, 0, 1), u);
            const float x11 = Lerp(corner(0, 1, 1), corner(1, 1, 1), u);
            return Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w);
        }

        inline void Worley(float x, float y, float z, uint32_t seed, int period, float& f1, float& f2)
        {
            const int ix = static_cast<int>(std::floor(x));
            const int iy = static_cast<int>(std::floor(y));
            const int iz = static_cast<int>(std::floor(z));
            f1 = f2 = 1e9f;
            for (int dz = -1; dz <= 1; ++dz)
            {
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const uint32_t h =
                            Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                        const float px = ix + dx + Unit(h);
                        const float py = iy + dy + Unit(Hash(h ^ 0x68bc21ebu));
                        const float pz = iz + dz + Unit(Hash(h ^ 0x02e5be93u));
                        const float d = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
                        if (d < f1)
                        {
                            f2 = f1;
                            f1 = d;
                        }
                        else if (d < f2)
                        {
                            f2 = d;
                        }
                    }
                }
            }
            f1 = std::sqrt(f1);
            f2 = std::sqrt(f2);
        }

        // Compact kernels share a bounded neighborhood and wrapped *hashes*, not wrapped
        // offsets: this is essential to preserve both continuity and tileability.
        inline float SimplexRaw(float x, float y, float z, uint32_t seed)
        {
            const float skew = (x + y + z) / 3.0f;
            const int i = static_cast<int>(std::floor(x + skew));
            const int j = static_cast<int>(std::floor(y + skew));
            const int k = static_cast<int>(std::floor(z + skew));
            const float unskew = (i + j + k) / 6.0f;
            const float a = x - (i - unskew), b = y - (j - unskew), c = z - (k - unskew);
            const int ix = a >= b && a >= c ? 1 : 0;
            const int iy = b > a && b >= c ? 1 : 0;
            const int iz = c > a && c > b ? 1 : 0;
            const int jx = a >= b || a >= c ? 1 : 0;
            const int jy = b > a || b >= c ? 1 : 0;
            const int jz = c > a || c > b ? 1 : 0;
            float sum = 0;
            for (int corner = 0; corner < 4; ++corner)
            {
                const int dx = corner == 0 ? 0 : (corner == 1 ? ix : (corner == 2 ? jx : 1));
                const int dy = corner == 0 ? 0 : (corner == 1 ? iy : (corner == 2 ? jy : 1));
                const int dz = corner == 0 ? 0 : (corner == 1 ? iz : (corner == 2 ? jz : 1));
                const float px = a - dx + corner / 6.0f;
                const float py = b - dy + corner / 6.0f;
                const float pz = c - dz + corner / 6.0f;
                const float t = (std::max)(0.0f, 0.6f - px * px - py * py - pz * pz);
                sum += t * t * t * t * GradientDot(Hash3(i + dx, j + dy, k + dz, seed), px, py, pz);
            }
            return std::clamp(32.0f * sum, -1.0f, 1.0f);
        }

        inline float Simplex(float x, float y, float z, uint32_t seed, int period)
        {
            if (period <= 0)
            {
                return SimplexRaw(x, y, z, seed);
            }
            // Blend translated simplex fields with quintic weights. Skewed simplex
            // lattice coordinates cannot be wrapped as Cartesian lattice cells.
            const float px = x - std::floor(x / period) * period;
            const float py = y - std::floor(y / period) * period;
            const float pz = z - std::floor(z / period) * period;
            const float u = Fade(px / period), v = Fade(py / period), w = Fade(pz / period);
            float sum = 0;
            for (int dz = 0; dz < 2; ++dz)
            {
                for (int dy = 0; dy < 2; ++dy)
                {
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        sum += SimplexRaw(px - dx * period, py - dy * period, pz - dz * period, seed) *
                               (dx == 0 ? 1 - u : u) * (dy == 0 ? 1 - v : v) * (dz == 0 ? 1 - w : w);
                    }
                }
            }
            return sum;
        }

        inline float VoronoiCell(float x, float y, float z, uint32_t seed, int period)
        {
            const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
            const int iz = static_cast<int>(std::floor(z));
            float nearest = 1e9f;
            uint32_t nearestHash = 0;
            // Radius two ensures the nearest feature is found even near cell corners.
            for (int dz = -2; dz <= 2; ++dz)
            {
                for (int dy = -2; dy <= 2; ++dy)
                {
                    for (int dx = -2; dx <= 2; ++dx)
                    {
                        const uint32_t h =
                            Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                        const float a = ix + dx + Unit(h) - x;
                        const float b = iy + dy + Unit(Hash(h ^ 0x68bc21ebu)) - y;
                        const float c = iz + dz + Unit(Hash(h ^ 0x02e5be93u)) - z;
                        const float d = a * a + b * b + c * c;
                        if (d < nearest)
                        {
                            nearest = d;
                            nearestHash = h;
                        }
                    }
                }
            }
            return Unit(Hash(nearestHash ^ 0xb5297a4du)) * 2 - 1;
        }

        inline float Flow(float x, float y, float z, uint32_t seed, int period, float phase)
        {
            const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
            const int iz = static_cast<int>(std::floor(z));
            const float a = x - ix, b = y - iy, c = z - iz;
            const float u = Fade(a), v = Fade(b), w = Fade(c);
            const float co = std::cos(phase), si = std::sin(phase);
            float sum = 0;
            for (int dz = 0; dz < 2; ++dz)
            {
                for (int dy = 0; dy < 2; ++dy)
                {
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const uint32_t h =
                            Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                        // Rotate each lattice gradient, rather than translating the field.
                        const float px = a - dx, py = b - dy, pz = c - dz;
                        const float g = GradientDot(h, co * px + si * py, -si * px + co * py, pz);
                        sum += g * (dx == 0 ? 1 - u : u) * (dy == 0 ? 1 - v : v) * (dz == 0 ? 1 - w : w);
                    }
                }
            }
            return std::clamp(sum * 0.95f, -1.0f, 1.0f);
        }

        inline std::array<float, 3> CurlVector(float x, float y, float z, uint32_t seed, int period)
        {
            // Curl of three independent scalar potentials; do not normalize the
            // vector, since doing so would destroy its divergence-free property.
            const float e = 0.002f;
            const uint32_t sy = seed ^ 0x68bc21ebu, sz = seed ^ 0x02e5be93u;
            const float cx = (Perlin(x, y + e, z, sz, period) - Perlin(x, y - e, z, sz, period) -
                              Perlin(x, y, z + e, sy, period) + Perlin(x, y, z - e, sy, period)) /
                             (2 * e);
            const float cy = (Perlin(x, y, z + e, seed, period) - Perlin(x, y, z - e, seed, period) -
                              Perlin(x + e, y, z, sz, period) + Perlin(x - e, y, z, sz, period)) /
                             (2 * e);
            const float cz = (Perlin(x + e, y, z, sy, period) - Perlin(x - e, y, z, sy, period) -
                              Perlin(x, y + e, z, seed, period) + Perlin(x, y - e, z, seed, period)) /
                             (2 * e);
            return {cx, cy, cz};
        }

        inline float Spectral(float x, float y, float z, uint32_t seed, int period, bool harmonic)
        {
            const float tau = 6.28318530718f;
            float sum = 0, norm = 0;
            // No spatial lattice. Integer wavevectors for periodic synthesis, random
            // continuous wavevectors otherwise. Harmonic uses one fundamental + overtones.
            const uint32_t fundamental = Hash(seed);
            for (int wave = 0; wave < 16; ++wave)
            {
                const uint32_t h = harmonic ? fundamental : Hash(seed + static_cast<uint32_t>(wave) * 1013u);
                float a = Unit(h) * 2 - 1;
                float b = Unit(Hash(h ^ 0x68bc21ebu)) * 2 - 1;
                float c = Unit(Hash(h ^ 0x02e5be93u)) * 2 - 1;
                if (period > 0)
                {
                    a = std::floor(a * period + 0.5f) / period;
                    b = std::floor(b * period + 0.5f) / period;
                    c = std::floor(c * period + 0.5f) / period;
                    if (a == 0 && b == 0 && c == 0)
                    {
                        a = 1.0f / period;
                    }
                }
                const float multiple = harmonic ? static_cast<float>(wave + 1) : 1.0f;
                const float amplitude = harmonic ? 1.0f / (multiple * multiple) : 1.0f;
                const float phase = tau * Unit(Hash(seed + static_cast<uint32_t>(wave) * 7919u));
                sum += std::sin(tau * multiple * (a * x + b * y + c * z) + phase) * amplitude;
                norm += amplitude;
            }
            return sum / norm;
        }

        inline float CompactWavelet(float r2)
        {
            // Laplacian of the compact radial bump (1-r^2)^5 in 3D, rescaled.
            // Its integral is zero; both value and slope vanish at the support boundary.
            const float t = (std::max)(0.0f, 1.0f - r2);
            return t * t * t * (1.0f - (11.0f / 3.0f) * r2);
        }

        inline float KernelNoise(Basis basis, float x, float y, float z, uint32_t seed, int period, float direction,
                                 float anisotropy, float kernelFrequency, float phase)
        {
            const float tau = 6.28318530718f;
            const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
            const int iz = static_cast<int>(std::floor(z));
            float real = 0, imaginary = 0, spots = 0;
            const float stretch = std::clamp(anisotropy, 1.0f, 8.0f);
            const float frequency = std::clamp(kernelFrequency, 0.25f, 8.0f);
            for (int dz = -1; dz <= 1; ++dz)
            {
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const uint32_t cellHash =
                            Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                        // Two stratified random impulses per cell: bounded work and no empty support holes.
                        for (int impulse = 0; impulse < 2; ++impulse)
                        {
                            const uint32_t h = Hash(cellHash + static_cast<uint32_t>(impulse) * 0x9e3779b9u);
                            const float a = x - (ix + dx + Unit(h));
                            const float b = y - (iy + dy + Unit(Hash(h ^ 0x68bc21ebu)));
                            const float c = z - (iz + dz + Unit(Hash(h ^ 0x02e5be93u)));
                            const float r2 = a * a + b * b + c * c;
                            if (r2 >= 1.0f)
                            {
                                continue;
                            }
                            const float t = 1 - r2;
                            const float weight = Unit(Hash(h ^ 0xb5297a4du)) * 2 - 1;
                            if (basis == Basis::Wavelet)
                            {
                                real += weight * CompactWavelet(r2);
                                continue;
                            }
                            if (basis == Basis::SparseConvolution)
                            {
                                real += weight * t * t * t;
                                continue;
                            }
                            if (basis == Basis::Spot)
                            {
                                const float radius = 0.2f + 0.35f * Unit(Hash(h ^ 0x1b56c4e9u));
                                spots = (std::max)(spots, 1.0f - Smooth(radius * 0.6f, radius, std::sqrt(r2)));
                                continue;
                            }
                            const bool directed = basis == Basis::AnisotropicGabor || basis == Basis::Phasor;
                            const float angle = directed ? direction : tau * Unit(Hash(h ^ 0x1b56c4e9u));
                            const float along = a * std::cos(angle) + b * std::sin(angle);
                            const float across = -a * std::sin(angle) + b * std::cos(angle);
                            const float envelope =
                                std::exp(-4.0f *
                                         (along * along + across * across * (directed ? stretch : 1.0f) + c * c)) *
                                t * t;
                            const float theta = tau * frequency * along + phase + tau * Unit(Hash(h ^ 0x967a889bu));
                            if (basis == Basis::LocalRandom)
                            {
                                // Local random-phase synthesis with multiple independent frequencies.
                                real += envelope * (std::cos(theta) + 0.5f * std::cos(theta * 1.93f + h % 17u));
                            }
                            else
                            {
                                real += envelope * std::cos(theta);
                                imaginary += envelope * std::sin(theta);
                            }
                        }
                    }
                }
            }
            if (basis == Basis::Spot)
            {
                return spots * 2 - 1;
            }
            if (basis == Basis::Phasor)
            {
                return real / std::sqrt((std::max)(real * real + imaginary * imaginary, 1e-12f));
            }
            return real / (1.0f + std::fabs(real));
        }
    }

    inline float BasisNoiseConfigured(Basis basis, float x, float y, float z, uint32_t seed, int period,
                                      float direction, float anisotropy, float kernelFrequency, float phase)
    {
        switch (basis)
        {
            case Basis::Value:
                return NoiseDetail::ValueNoise(x, y, z, seed, period);
            case Basis::Worley:
            case Basis::CellEdge: {
                float f1;
                float f2;
                NoiseDetail::Worley(x, y, z, seed, period, f1, f2);
                // 各ノイズ基底をフラクタル合成で同じように扱えるよう、[-1, 1] に変換する。
                const float value =
                    basis == Basis::Worley ? NoiseDetail::Saturate(f1 * 1.2f) : NoiseDetail::Saturate((f2 - f1) * 2.0f);
                return value * 2 - 1;
            }
            case Basis::White: {
                const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
                const int iz = static_cast<int>(std::floor(z));
                const uint32_t cell = NoiseDetail::Hash3(NoiseDetail::Wrap(ix, period), NoiseDetail::Wrap(iy, period),
                                                         NoiseDetail::Wrap(iz, period), seed);
                return NoiseDetail::Unit(NoiseDetail::Hash3(static_cast<int>((x - ix) * 256),
                                                            static_cast<int>((y - iy) * 256),
                                                            static_cast<int>((z - iz) * 256), cell)) *
                           2 -
                       1;
            }
            case Basis::Simplex:
                return NoiseDetail::Simplex(x, y, z, seed, period);
            case Basis::Voronoi:
                return NoiseDetail::VoronoiCell(x, y, z, seed, period);
            case Basis::Cell:
                return NoiseDetail::Unit(NoiseDetail::Hash3(NoiseDetail::Wrap(static_cast<int>(std::floor(x)), period),
                                                            NoiseDetail::Wrap(static_cast<int>(std::floor(y)), period),
                                                            NoiseDetail::Wrap(static_cast<int>(std::floor(z)), period),
                                                            seed)) *
                           2 -
                       1;
            case Basis::Gabor:
            case Basis::AnisotropicGabor:
            case Basis::Wavelet:
            case Basis::SparseConvolution:
            case Basis::Spot:
            case Basis::LocalRandom:
            case Basis::Phasor:
                return NoiseDetail::KernelNoise(basis, x, y, z, seed, period, direction, anisotropy, kernelFrequency,
                                                phase);
            case Basis::Curl: {
                const auto curl = NoiseDetail::CurlVector(x, y, z, seed, period);
                return curl[2] / (1.0f + std::fabs(curl[2]));
            }
            case Basis::ValueGradient:
                return 0.5f *
                       (NoiseDetail::ValueNoise(x, y, z, seed, period) + NoiseDetail::Perlin(x, y, z, seed, period));
            case Basis::Chevron: {
                const float tau = 6.28318530718f;
                const float offset = NoiseDetail::Unit(NoiseDetail::Hash(seed));
                const float v = y + offset;
                const float triangle = std::fabs((v - std::floor(v)) * 2 - 1);
                return std::sin(tau * (x + triangle + offset + phase / tau));
            }
            case Basis::LatticeFree:
            case Basis::Harmonic:
                return NoiseDetail::Spectral(x, y, z, seed, period, basis == Basis::Harmonic);
            case Basis::Flow:
                return NoiseDetail::Flow(x, y, z, seed, period, phase);
            case Basis::Ridged:
                return 1.0f - 2.0f * std::fabs(NoiseDetail::Perlin(x, y, z, seed, period));
            default:
                return NoiseDetail::Perlin(x, y, z, seed, period);
        }
    }

    inline float BasisNoise(Basis basis, float x, float y, float z, uint32_t seed, int period)
    {
        return BasisNoiseConfigured(basis, x, y, z, seed, period, 0.0f, 4.0f, 2.0f, 0.0f);
    }

    inline float FractalNoise(const NoiseParams& params, float x, float y, float z, bool tileable)
    {
        const int octaves = std::clamp(params.Octaves, 1, 12);
        float frequency = (std::max)(params.Frequency, 0.001f);
        if (params.Fractal == FractalType::None)
        {
            const int period = tileable ? (std::max)(1, static_cast<int>(std::round(frequency))) : 0;
            const float scale = tileable ? static_cast<float>(period) : frequency;
            const float value =
                BasisNoiseConfigured(params.BasisType, x * scale, y * scale, z * scale, NoiseDetail::Hash(params.Seed),
                                     period, params.Direction, params.Anisotropy, params.KernelFrequency, params.Phase);
            return NoiseDetail::Saturate(value * 0.5f + 0.5f);
        }
        const float lacunarity = tileable ? (std::max)(1.0f, std::round(params.Lacunarity)) : params.Lacunarity;
        if (params.Fractal == FractalType::DomainWarp)
        {
            NoiseParams inner = params;
            inner.Fractal = FractalType::FBm;
            // 相関を抑えた 2 つの fBm 場で変形する。オフセットを周期の整数倍にしてタイル性を保つ。
            const float wx = FractalNoise(inner, x + 5.2f, y + 1.3f, z + 2.7f, tileable) * 2 - 1;
            NoiseParams second = inner;
            second.Seed = params.Seed ^ 0x9e3779b9u;
            const float wy = FractalNoise(second, x + 1.7f, y + 9.2f, z + 4.1f, tileable) * 2 - 1;
            const float s = params.WarpStrength / (std::max)(params.Frequency, 0.001f);
            return FractalNoise(inner, x + wx * s, y + wy * s, z + (wx - wy) * s * 0.5f, tileable);
        }
        float amplitude = 1;
        float sum = 0;
        float norm = 0;
        float weight = 1;
        for (int o = 0; o < octaves; ++o)
        {
            const int period = tileable ? (std::max)(1, static_cast<int>(std::round(frequency))) : 0;
            const float f = tileable ? static_cast<float>(period) : frequency;
            const uint32_t seed = NoiseDetail::Hash(params.Seed + static_cast<uint32_t>(o) * 1013u);
            float n = BasisNoiseConfigured(params.BasisType, x * f, y * f, z * f, seed, period, params.Direction,
                                           params.Anisotropy, params.KernelFrequency, params.Phase);
            switch (params.Fractal)
            {
                case FractalType::Ridged: {
                    n = params.RidgeOffset - std::fabs(n);
                    n *= n * weight;
                    weight = NoiseDetail::Saturate(n * 2);
                    sum += n * amplitude;
                    break;
                }
                case FractalType::Multifractal: {
                    // Product of offset signals; subtract the unit DC term after normalization.
                    weight *= 1.0f + n * amplitude;
                    sum = weight - 1.0f;
                    break;
                }
                case FractalType::HeterogeneousMultifractal: {
                    // Higher elevations receive more detail; valleys remain smooth.
                    const float signal = (n + params.RidgeOffset) * amplitude;
                    sum += o == 0 ? signal : signal * sum;
                    break;
                }
                case FractalType::Billow:
                    sum += (std::fabs(n) * 2 - 1) * amplitude;
                    break;
                case FractalType::Turbulence:
                    sum += std::fabs(n) * amplitude;
                    break;
                case FractalType::HybridMultifractal: {
                    const float signal = (n + params.RidgeOffset * 0.7f) * amplitude;
                    sum += o == 0 ? signal : signal * (std::min)(weight, 1.0f);
                    weight = (std::min)(weight * signal * 2, 1.0f);
                    break;
                }
                default:
                    sum += n * amplitude;
                    break;
            }
            norm += amplitude;
            amplitude *= params.Gain;
            frequency *= lacunarity;
        }
        float value = sum / (std::max)(norm, 1e-5f);
        switch (params.Fractal)
        {
            case FractalType::Ridged:
                return NoiseDetail::Saturate(value / (std::max)(params.RidgeOffset * params.RidgeOffset, 0.25f));
            case FractalType::Turbulence:
                return NoiseDetail::Saturate(value * 1.6f);
            case FractalType::HybridMultifractal:
                return NoiseDetail::Saturate(value * 0.8f);
            case FractalType::Multifractal:
                return NoiseDetail::Saturate(0.5f + 0.5f * sum / (1.0f + std::fabs(sum)));
            case FractalType::HeterogeneousMultifractal:
                return NoiseDetail::Saturate(sum / (1.0f + std::fabs(sum)));
            default:
                return NoiseDetail::Saturate(value * 0.8f + 0.5f);
        }
    }

}
