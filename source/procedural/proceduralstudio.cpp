#include "pch.h"
#include "proceduralstudio.h"
#include "proceduralworkgraph.h"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace ProcStudio
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        uint32_t Hash(uint32_t value)
        {
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            return value ^ (value >> 16);
        }

        uint32_t Hash3(int x, int y, int z, uint32_t seed)
        {
            return Hash(
                static_cast<uint32_t>(x) * 0x8da6b343u ^
                Hash(static_cast<uint32_t>(y) * 0xd8163841u ^ Hash(static_cast<uint32_t>(z) * 0xcb1ab31fu ^ seed)));
        }

        float Unit(uint32_t h)
        {
            return static_cast<float>(h >> 8) / 16777216.0f;
        }

        float Fade(float t)
        {
            return t * t * t * (t * (t * 6 - 15) + 10);
        }

        float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        float Saturate(float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        float Smooth(float a, float b, float v)
        {
            const float t = Saturate((v - a) / (std::max)(b - a, 1e-6f));
            return t * t * (3 - 2 * t);
        }

        int Wrap(int value, int period)
        {
            if (period <= 0)
            {
                return value;
            }
            const int m = value % period;
            return m < 0 ? m + period : m;
        }

        Float3 Add(Float3 a, Float3 b)
        {
            return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
        }

        Float3 Sub(Float3 a, Float3 b)
        {
            return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
        }

        Float3 Mul(Float3 a, float s)
        {
            return {a.X * s, a.Y * s, a.Z * s};
        }

        float Dot(Float3 a, Float3 b)
        {
            return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
        }

        Float3 Cross(Float3 a, Float3 b)
        {
            return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
        }

        Float3 Normalize(Float3 a)
        {
            const float l = std::sqrt(Dot(a, a));
            return l > 1e-12f ? Mul(a, 1 / l) : Float3{0, 1, 0};
        }

        Float3 Mix(const std::array<float, 3>& a, const std::array<float, 3>& b, float t)
        {
            return {Lerp(a[0], b[0], t), Lerp(a[1], b[1], t), Lerp(a[2], b[2], t)};
        }

        Float3 Mix3(Float3 a, Float3 b, float t)
        {
            return {Lerp(a.X, b.X, t), Lerp(a.Y, b.Y, t), Lerp(a.Z, b.Z, t)};
        }

        Float3 ToF3(const std::array<float, 3>& a)
        {
            return {a[0], a[1], a[2]};
        }

        float GradientDot(uint32_t h, float x, float y, float z)
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

        float Perlin(float x, float y, float z, uint32_t seed, int period)
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

        float ValueNoise(float x, float y, float z, uint32_t seed, int period)
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

        void Worley(float x, float y, float z, uint32_t seed, int period, float& f1, float& f2)
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
    }

    float BasisNoise(Basis basis, float x, float y, float z, uint32_t seed, int period)
    {
        switch (basis)
        {
            case Basis::Value:
                return ValueNoise(x, y, z, seed, period);
            case Basis::Worley:
            case Basis::CellEdge: {
                float f1;
                float f2;
                Worley(x, y, z, seed, period, f1, f2);
                // 各ノイズ基底をフラクタル合成で同じように扱えるよう、[-1, 1] に変換する。
                const float value = basis == Basis::Worley ? Saturate(f1 * 1.2f) : Saturate((f2 - f1) * 2.0f);
                return value * 2 - 1;
            }
            default:
                return Perlin(x, y, z, seed, period);
        }
    }

    float FractalNoise(const NoiseParams& params, float x, float y, float z, bool tileable)
    {
        const int octaves = std::clamp(params.Octaves, 1, 12);
        float frequency = (std::max)(params.Frequency, 0.001f);
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
            const uint32_t seed = Hash(params.Seed + static_cast<uint32_t>(o) * 1013u);
            float n = BasisNoise(params.BasisType, x * f, y * f, z * f, seed, period);
            switch (params.Fractal)
            {
                case FractalType::Ridged: {
                    n = params.RidgeOffset - std::fabs(n);
                    n *= n * weight;
                    weight = Saturate(n * 2);
                    sum += n * amplitude;
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
                return Saturate(value / (std::max)(params.RidgeOffset * params.RidgeOffset, 0.25f));
            case FractalType::Turbulence:
                return Saturate(value * 1.6f);
            case FractalType::HybridMultifractal:
                return Saturate(value * 0.8f);
            default:
                return Saturate(value * 0.8f + 0.5f);
        }
    }

    Float4 Image::Sample(float u, float v) const
    {
        if (Width <= 0 || Height <= 0)
        {
            return {};
        }
        const float x = (u - std::floor(u)) * Width - 0.5f;
        const float y = (v - std::floor(v)) * Height - 0.5f;
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const float tx = x - x0;
        const float ty = y - y0;
        auto at = [&](int px, int py) {
            return Pixels[Wrap(py, Height) * Width + Wrap(px, Width)];
        };
        const Float4 a = at(x0, y0);
        const Float4 b = at(x0 + 1, y0);
        const Float4 c = at(x0, y0 + 1);
        const Float4 d = at(x0 + 1, y0 + 1);
        auto mix = [&](float p, float q, float r, float s) {
            return Lerp(Lerp(p, q, tx), Lerp(r, s, tx), ty);
        };
        return {mix(a.X, b.X, c.X, d.X), mix(a.Y, b.Y, c.Y, d.Y), mix(a.Z, b.Z, c.Z, d.Z), mix(a.W, b.W, c.W, d.W)};
    }

    const char* TextureKindName(TextureKind kind)
    {
        static const char* names[] = {"フラクタルノイズ", "大理石 (Marble)",  "木目 (Wood)", "御影石 (Granite)",
                                      "ボロノイ",         "レンガ",           "チェッカー",  "フラクタル水面",
                                      "フラクタル雲",     "マンデルブロ集合", "ジュリア集合"};
        const int index = std::clamp(static_cast<int>(kind), 0, static_cast<int>(TextureKind::Count) - 1);
        return names[index];
    }

    bool IsSolidCapable(TextureKind kind)
    {
        return kind == TextureKind::FractalNoise || kind == TextureKind::Marble || kind == TextureKind::Wood ||
               kind == TextureKind::Granite || kind == TextureKind::Voronoi || kind == TextureKind::FractalCloud;
    }

    namespace
    {
        float Escape(const TextureParams& p, float u, float v, bool julia)
        {
            const float scale = 3.0f / (std::max)(p.Zoom, 1e-4f);
            double zx = julia ? (u - 0.5) * scale : 0;
            double zy = julia ? (v - 0.5) * scale : 0;
            const double cx = julia ? p.JuliaX : p.CenterX + (u - 0.5) * scale;
            const double cy = julia ? p.JuliaY : p.CenterY + (v - 0.5) * scale;
            if (julia)
            {
                zx += p.CenterX + 0.5;
                zy += p.CenterY;
            }
            const int limit = std::clamp(p.Iterations, 8, 4000);
            int i = 0;
            for (; i < limit && zx * zx + zy * zy < 256.0; ++i)
            {
                const double t = zx * zx - zy * zy + cx;
                zy = 2 * zx * zy + cy;
                zx = t;
            }
            if (i >= limit)
            {
                return 0;
            }
            // 滑らかな反復回数。
            const double log = std::log(std::log(std::sqrt(zx * zx + zy * zy)) / std::log(2.0)) / std::log(2.0);
            return Saturate(static_cast<float>((i + 1 - log) / limit) * 4.0f);
        }

        float WaterHeight(const TextureParams& p, float x, float y, float z, bool tileable)
        {
            // 波立つ fBm: 尖らせた稜線を時間とともに移動する。整数の移動量でタイル性を保つ。
            NoiseParams n = p.Noise;
            n.Fractal = FractalType::FBm;
            const float t = p.Time * 0.15f;
            const float a = FractalNoise(n, x + t, y + t * 0.6f, z + t * 0.3f, tileable);
            NoiseParams r = n;
            r.Fractal = FractalType::Ridged;
            r.Seed ^= 0x51ed27u;
            const float b = FractalNoise(r, x - t * 0.7f, y + t * 0.4f, z - t * 0.2f, tileable);
            return Saturate(Lerp(a, b, p.Choppiness));
        }
    }

    float TextureScalar(const TextureParams& p, float x, float y, float z, bool solid)
    {
        const bool tile = p.Tileable && !solid;
        float value = 0;
        switch (p.Kind)
        {
            case TextureKind::Marble: {
                const float n = FractalNoise(p.Noise, x, y, z, tile);
                // 縞の周波数を整数にし、タイル化したパターンの周期性を保つ。
                value = 0.5f + 0.5f * std::sin((x * std::round(p.RingCount) + n * p.Turbulence) * 2 * kPi);
                value = std::pow(value, 0.6f);
                break;
            }
            case TextureKind::Wood: {
                const float n = FractalNoise(p.Noise, x, y, z, tile);
                const float cx = x - 0.5f;
                const float cz = (solid ? z : y) - 0.5f;
                const float r = std::sqrt(cx * cx + cz * cz) * p.RingCount * 2 + n * p.Turbulence * 0.5f;
                value = r - std::floor(r);
                value = Smooth(0.0f, 0.8f, value) * (1 - Smooth(0.85f, 1.0f, value));
                break;
            }
            case TextureKind::Granite: {
                NoiseParams n = p.Noise;
                n.BasisType = Basis::Worley;
                n.Fractal = FractalType::FBm;
                const float cells = FractalNoise(n, x, y, z, tile);
                const float grain = FractalNoise(p.Noise, x * 4, y * 4, z * 4, tile);
                value = Saturate(cells * 0.6f + grain * 0.4f);
                value = value > 0.62f ? 1.0f : value < 0.3f ? 0.0f : value;
                break;
            }
            case TextureKind::Voronoi: {
                float f1;
                float f2;
                const int period = tile ? (std::max)(1, static_cast<int>(std::round(p.Noise.Frequency))) : 0;
                const float f = tile ? static_cast<float>(period) : p.Noise.Frequency;
                Worley(x * f, y * f, z * f, p.Noise.Seed, period, f1, f2);
                value = Saturate((f2 - f1) * 3.0f);
                break;
            }
            case TextureKind::Brick: {
                const float bx = x * p.BricksX;
                const float by = y * p.BricksY;
                const int row = static_cast<int>(std::floor(by));
                const float sx = bx + (row & 1) * 0.5f;
                const float fx = sx - std::floor(sx);
                const float fy = by - std::floor(by);
                const float mx = p.Mortar * p.BricksX * 0.5f;
                const float my = p.Mortar * p.BricksY * 0.5f;
                const float edge =
                    (std::min)(Smooth(0, mx, (std::min)(fx, 1 - fx)), Smooth(0, my, (std::min)(fy, 1 - fy)));
                const float variation = Unit(Hash3(Wrap(static_cast<int>(std::floor(sx)), (std::max)(p.BricksX, 1)),
                                                   Wrap(row, (std::max)(p.BricksY, 1)), 0, p.Noise.Seed));
                value = edge * (0.6f + 0.4f * variation) * (0.85f + 0.15f * FractalNoise(p.Noise, x, y, z, tile));
                break;
            }
            case TextureKind::Checker: {
                const int cx = static_cast<int>(std::floor(x * p.BricksX));
                const int cy = static_cast<int>(std::floor(y * p.BricksX));
                value = ((cx + cy) & 1) ? 1.0f : 0.0f;
                break;
            }
            case TextureKind::FractalWater:
                value = WaterHeight(p, x, y, z, tile);
                break;
            case TextureKind::FractalCloud: {
                NoiseParams n = p.Noise;
                n.Fractal = FractalType::FBm;
                const float base = FractalNoise(n, x, y, z, tile);
                NoiseParams cell = n;
                cell.BasisType = Basis::Worley;
                cell.Seed ^= 0x7f4a7c15u;
                const float billows = 1 - FractalNoise(cell, x, y, z, tile);
                const float shape = base * 0.65f + billows * 0.35f;
                const float threshold = 1 - p.Coverage;
                value = Smooth(threshold - p.Softness * 0.5f, threshold + p.Softness * 0.5f, shape);
                break;
            }
            case TextureKind::Mandelbrot:
                value = Escape(p, x, y, false);
                break;
            case TextureKind::Julia:
                value = Escape(p, x, y, true);
                break;
            default:
                value = FractalNoise(p.Noise, x, y, z, tile);
                break;
        }
        return Saturate((value - 0.5f) * p.Contrast + 0.5f + p.Bias);
    }

    Float3 TextureColor(const TextureParams& p, float x, float y, float z, bool solid)
    {
        const float v = TextureScalar(p, x, y, z, solid);
        switch (p.Kind)
        {
            case TextureKind::Brick:
                // 目地には ColorB、レンガには ColorC を使い、レンガごとに陰影を変える。
                return Mix3(ToF3(p.ColorB), Mul(ToF3(p.ColorC), 0.6f + 0.6f * v), Saturate(v * 4));
            case TextureKind::FractalWater: {
                // 海用の固定配色。汎用ランプの色は石やノイズ向けに調整されている。
                const Float3 deep{0.01f, 0.07f, 0.13f};
                const Float3 shallow{0.04f, 0.32f, 0.42f};
                const Float3 foam{0.92f, 0.95f, 0.98f};
                const Float3 water = Mix3(deep, shallow, Smooth(0.2f, 0.75f, v));
                return Mix3(water, foam, Smooth(0.72f, 0.92f, v) * p.Choppiness);
            }
            case TextureKind::FractalCloud: {
                const Float3 sky{0.32f, 0.52f, 0.86f};
                return Mix3(sky, Mix(p.ColorB, std::array<float, 3>{1, 1, 1}, 0.5f), v);
            }
            case TextureKind::Mandelbrot:
            case TextureKind::Julia: {
                if (v <= 0)
                {
                    return {0, 0, 0};
                }
                const float t = std::sqrt(v);
                const float glow = Saturate(t * 1.6f);
                return Mul(Float3{0.5f + 0.5f * std::cos(2 * kPi * (t + 0.0f)),
                                  0.5f + 0.5f * std::cos(2 * kPi * (t + 0.33f)),
                                  0.5f + 0.5f * std::cos(2 * kPi * (t + 0.67f))},
                           glow);
            }
            default:
                // A → B → C の 3 色ランプ。
                return v < 0.5f ? Mix(p.ColorA, p.ColorB, v * 2) : Mix(p.ColorB, p.ColorC, (v - 0.5f) * 2);
        }
    }

    Image GenerateTexture(const TextureParams& p, int width, int height)
    {
        return ProceduralWorkGraph::GenerateTexture(p, width, height);
    }

    Volume GenerateVolume(const TextureParams& p, int extent)
    {
        return ProceduralWorkGraph::GenerateVolume(p, extent);
    }

    Volume VolumeFromImage(const Image& image, int extent, int octaves, float verticalVariation, uint32_t seed)
    {
        return ProceduralWorkGraph::VolumeFromImage(image, extent, octaves, verticalVariation, seed);
    }

    std::vector<Float4> GenerateSolidVolume(const TextureParams& p, int extent)
    {
        return ProceduralWorkGraph::GenerateSolidVolume(p, extent);
    }

    // ------------------------------------------------------------------------------------------------
    // メッシュ生成。
    void Mesh::Clear()
    {
        Positions.clear();
        Normals.clear();
        Colors.clear();
        UVs.clear();
        Indices.clear();
    }

    uint32_t Mesh::AddVertex(Float3 position, Float3 color, Float2 uv)
    {
        Positions.push_back(position);
        Normals.push_back({0, 0, 0});
        Colors.push_back(color);
        UVs.push_back(uv);
        return static_cast<uint32_t>(Positions.size() - 1);
    }

    void Mesh::ComputeNormals()
    {
        Normals.assign(Positions.size(), Float3{0, 0, 0});
        for (size_t triangleIndex = 0; triangleIndex + 2 < Indices.size(); triangleIndex += 3)
        {
            const uint32_t firstVertex = Indices[triangleIndex];
            const uint32_t secondVertex = Indices[triangleIndex + 1];
            const uint32_t thirdVertex = Indices[triangleIndex + 2];
            const Float3 normal = Cross(Sub(Positions[secondVertex], Positions[firstVertex]),
                                        Sub(Positions[thirdVertex], Positions[firstVertex])); // 面積による重み付け。
            Normals[firstVertex] = Add(Normals[firstVertex], normal);
            Normals[secondVertex] = Add(Normals[secondVertex], normal);
            Normals[thirdVertex] = Add(Normals[thirdVertex], normal);
        }
        for (auto& normal : Normals)
        {
            normal = Normalize(normal);
        }
    }

    void Mesh::Bounds(Float3& minimum, Float3& maximum) const
    {
        minimum = {1e30f, 1e30f, 1e30f};
        maximum = {-1e30f, -1e30f, -1e30f};
        for (const auto& position : Positions)
        {
            minimum = {(std::min)(minimum.X, position.X), (std::min)(minimum.Y, position.Y),
                       (std::min)(minimum.Z, position.Z)};
            maximum = {(std::max)(maximum.X, position.X), (std::max)(maximum.Y, position.Y),
                       (std::max)(maximum.Z, position.Z)};
        }
        if (Positions.empty())
        {
            minimum = maximum = {0, 0, 0};
        }
    }

    const char* ModelKindName(ModelKind kind)
    {
        static const char* names[] = {"球 (プレビュー)", "立方体 (プレビュー)", "平面 (プレビュー)",
                                      "フラクタル地形",  "フラクタルツリー",    "ノイズ岩 / 小惑星",
                                      "フラクタル水面",  "メンガーのスポンジ",  "シェルピンスキーのピラミッド",
                                      "フラクタルの葉"};
        const int index = std::clamp(static_cast<int>(kind), 0, static_cast<int>(ModelKind::Count) - 1);
        return names[index];
    }

    const char* LeafShapeName(LeafShape shape)
    {
        static const char* names[] = {"板ポリ (軽量)", "単葉", "羽状複葉 (フラクタル)", "シダ (フラクタル)",
                                      "掌状 (フラクタル)"};
        return names[std::clamp(static_cast<int>(shape), 0, static_cast<int>(LeafShape::Count) - 1)];
    }

    Mesh GenerateLeaf(const LeafParams& params, uint32_t seed)
    {
        return ProceduralWorkGraph::GenerateLeaf(params, seed);
    }

    Mesh GenerateModel(const ModelParams& p)
    {
        Mesh mesh;
        switch (p.Kind)
        {
            case ModelKind::Rock:
            case ModelKind::Sphere:
            case ModelKind::Cube:
            case ModelKind::MengerSponge:
            case ModelKind::SierpinskiPyramid:
                mesh = ProceduralWorkGraph::GenerateAnalyticModel(p);
                break;
            case ModelKind::Plane:
            case ModelKind::Terrain:
            case ModelKind::WaterSurface:
                mesh = ProceduralWorkGraph::GenerateGridModel(p);
                break;
            case ModelKind::FractalTree:
                mesh = ProceduralWorkGraph::GenerateTree(p);
                break;
            case ModelKind::FractalLeaf:
                mesh = ProceduralWorkGraph::GenerateLeaf(p.Leaf, p.Seed, true);
                break;
            default:
                break;
        }
        return mesh;
    }

    bool WriteObj(const std::filesystem::path& path, const Mesh& mesh, bool vertexColors,
                  const std::string& diffuseTexture, std::string& error)
    {
        std::ofstream obj(path, std::ios::binary | std::ios::trunc);
        if (!obj)
        {
            error = "OBJ を書き込めません";
            return false;
        }
        auto mtlPath = path;
        mtlPath.replace_extension(".mtl");
        // 先頭行のマーカーにより、エディターは座標変換せず等倍で配置する。
        // u8string を使う。Windows の string() は日本語など非 ASCII のファイル名で例外や文字化けを起こす。
        const auto mtlName = mtlPath.filename().u8string();
        obj << kProceduralObjMarker << "\nmtllib " << std::string(mtlName.begin(), mtlName.end())
            << "\nusemtl procedural\n";
        obj.setf(std::ios::fixed);
        obj.precision(6);
        for (size_t i = 0; i < mesh.Positions.size(); ++i)
        {
            const auto& p = mesh.Positions[i];
            obj << "v " << p.X << ' ' << p.Y << ' ' << p.Z;
            if (vertexColors)
            {
                const auto& c = mesh.Colors[i];
                obj << ' ' << c.X << ' ' << c.Y << ' ' << c.Z;
            }
            obj << '\n';
        }
        for (const auto& uv : mesh.UVs)
        {
            obj << "vt " << uv.X << ' ' << 1 - uv.Y << '\n';
        }
        for (const auto& n : mesh.Normals)
        {
            obj << "vn " << n.X << ' ' << n.Y << ' ' << n.Z << '\n';
        }
        for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
        {
            obj << 'f';
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t index = mesh.Indices[i + k] + 1;
                obj << ' ' << index << '/' << index << '/' << index;
            }
            obj << '\n';
        }
        std::ofstream mtl(mtlPath, std::ios::binary | std::ios::trunc);
        if (!mtl)
        {
            error = "MTL を書き込めません";
            return false;
        }
        mtl << "newmtl procedural\nKd 1 1 1\nKa 0 0 0\nKs 0.04 0.04 0.04\nNs 32\n";
        if (!diffuseTexture.empty())
        {
            mtl << "map_Kd " << diffuseTexture << '\n';
        }
        if (!obj.good() || !mtl.good())
        {
            error = "ファイル書き込み中にエラーが発生しました";
            return false;
        }
        return true;
    }
}

namespace ProcStudio
{
    bool IsProceduralObj(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        std::string line;
        return std::getline(input, line) && line.rfind("# Procedural Studio", 0) == 0;
    }
}
