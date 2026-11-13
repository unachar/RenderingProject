#include "pch.h"
#include "proceduralstudio.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <functional>
#include <thread>

namespace ProcStudio
{
    namespace
    {
        // [0, count)をCPUスレッドへ分割する。生成処理は座標ごとに独立して実行する。
        void ParallelFor(int count, const std::function<void(int)>& body)
        {
            const int workerCount = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 32);
            if (count < 8 || workerCount == 1)
            {
                for (int itemIndex = 0; itemIndex < count; ++itemIndex)
                {
                    body(itemIndex);
                }
                return;
            }
            std::vector<std::thread> workerThreads;
            for (int workerIndex = 0; workerIndex < workerCount; ++workerIndex)
            {
                workerThreads.emplace_back([&, workerIndex] {
                    for (int itemIndex = workerIndex; itemIndex < count; itemIndex += workerCount)
                    {
                        body(itemIndex);
                    }
                });
            }
            for (auto& thread : workerThreads)
            {
                thread.join();
            }
        }

        constexpr float kPi = 3.14159265358979323846f;

        using NoiseDetail::Hash;
        using NoiseDetail::Hash3;
        using NoiseDetail::Lerp;
        using NoiseDetail::Saturate;
        using NoiseDetail::Smooth;
        using NoiseDetail::Unit;
        using NoiseDetail::Worley;
        using NoiseDetail::Wrap;

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
        static const char* names[] = {"フラクタルノイズ", "大理石 (Marble)",  "木目 (Wood)",  "御影石 (Granite)",
                                      "ボロノイ",         "レンガ",           "チェッカー",   "フラクタル水面",
                                      "フラクタル雲",     "マンデルブロ集合", "ジュリア集合", "幾何学模様"};
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
        float GeometricMask(const TextureParams& params, float x, float y)
        {
            const float cellX = x * std::clamp(params.RepeatX, 1, 128);
            const float cellY = y * std::clamp(params.RepeatY, 1, 128);
            const float localX = cellX - std::floor(cellX) - 0.5f;
            const float localY = cellY - std::floor(cellY) - 0.5f;
            const float size = std::clamp(params.PatternSize, 0.05f, 0.95f);
            const float softness = std::clamp(params.PatternSoftness, 0.001f, 0.15f);
            float distance = 0;
            switch (params.Pattern)
            {
                case GeometricPattern::Stripes:
                    distance = std::abs(localX) - size * 0.5f;
                    break;
                case GeometricPattern::Grid:
                    distance = (std::min)(0.5f - std::abs(localX), 0.5f - std::abs(localY)) - size * 0.5f;
                    break;
                case GeometricPattern::Dots:
                    distance = std::sqrt(localX * localX + localY * localY) - size * 0.5f;
                    break;
                case GeometricPattern::Diamonds:
                    distance = (std::abs(localX) + std::abs(localY) - size * 0.5f) / std::sqrt(2.0f);
                    break;
                case GeometricPattern::Hexagons:
                    distance = (std::max)(std::abs(localX) * 0.8660254f + std::abs(localY) * 0.5f, std::abs(localY)) -
                               size * 0.4330127f;
                    break;
                case GeometricPattern::Triangles:
                    // 大きい三角形のぼかしはセルを越えるため、隣接セルも評価して継ぎ目をなくす。
                    distance = 1;
                    for (int offsetY = -1; offsetY <= 1; ++offsetY)
                    {
                        for (int offsetX = -1; offsetX <= 1; ++offsetX)
                        {
                            const float triangleX = localX + offsetX;
                            const float triangleY = localY + offsetY;
                            const float triangleDistance =
                                (std::max)(std::abs(triangleX) * 0.8660254f + triangleY * 0.5f, -triangleY) -
                                size * 0.25f;
                            distance = (std::min)(distance, triangleDistance);
                        }
                    }
                    break;
                default:
                    return 0;
            }
            return 1 - Smooth(-softness * 0.5f, softness * 0.5f, distance);
        }

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
            case TextureKind::Geometric:
                value = GeometricMask(p, x, y);
                break;
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
            case TextureKind::Geometric:
                return Mix(p.ColorA, p.ColorB, v);
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
        Image image;
        image.Width = (std::max)(width, 1);
        image.Height = (std::max)(height, 1);
        image.Pixels.resize(static_cast<size_t>(image.Width) * image.Height);
        std::vector<float> heights;
        if (p.Output == TextureOutput::Normal)
        {
            heights.resize(image.Pixels.size());
        }
        ParallelFor(image.Height, [&](int y) {
            for (int x = 0; x < image.Width; ++x)
            {
                const float u = (x + 0.5f) / image.Width;
                const float v = (y + 0.5f) / image.Height;
                auto& out = image.Pixels[static_cast<size_t>(y) * image.Width + x];
                if (p.Output == TextureOutput::Color)
                {
                    const Float3 c = TextureColor(p, u, v, 0, false);
                    out = {c.X, c.Y, c.Z, 1};
                }
                else
                {
                    const float h = TextureScalar(p, u, v, 0, false);
                    out = {h, h, h, 1};
                    if (!heights.empty())
                    {
                        heights[static_cast<size_t>(y) * image.Width + x] = h;
                    }
                }
            }
        });
        if (!heights.empty())
        {
            // 折り返し付き中央差分から接空間法線マップを生成する（OpenGL の +Y 規約）。
            for (int y = 0; y < image.Height; ++y)
            {
                for (int x = 0; x < image.Width; ++x)
                {
                    auto h = [&](int px, int py) {
                        return heights[Wrap(py, image.Height) * image.Width + Wrap(px, image.Width)];
                    };
                    const float dx = (h(x + 1, y) - h(x - 1, y)) * p.NormalStrength;
                    const float dy = (h(x, y + 1) - h(x, y - 1)) * p.NormalStrength;
                    const Float3 n = Normalize({-dx, dy, 1});
                    image.Pixels[static_cast<size_t>(y) * image.Width + x] = {n.X * 0.5f + 0.5f, n.Y * 0.5f + 0.5f,
                                                                              n.Z * 0.5f + 0.5f, 1};
                }
            }
        }
        return image;
    }

    Volume GenerateVolume(const TextureParams& p, int extent)
    {
        Volume volume;
        volume.Extent = std::clamp(extent, 8, 256);
        const size_t n = static_cast<size_t>(volume.Extent);
        volume.Density.resize(n * n * n);
        TextureParams params = p;
        params.Tileable = true;
        ParallelFor(static_cast<int>(n), [&](int zi) {
            const size_t z = static_cast<size_t>(zi);
            for (size_t y = 0; y < n; ++y)
            {
                for (size_t x = 0; x < n; ++x)
                {
                    const float fx = (x + 0.5f) / n;
                    const float fy = (y + 0.5f) / n;
                    const float fz = (z + 0.5f) / n;
                    // 第 3 座標を格子軸として 2D スカラーを評価し、3 軸すべてでタイル化可能にする。
                    volume.Density[(z * n + y) * n + x] = TextureScalar(params, fx, fy, fz, false);
                }
            }
        });
        return volume;
    }

    Volume VolumeFromImage(const Image& image, int extent, int octaves, float verticalVariation, uint32_t seed)
    {
        Volume volume;
        volume.Extent = std::clamp(extent, 8, 256);
        const size_t n = static_cast<size_t>(volume.Extent);
        volume.Density.resize(n * n * n);
        octaves = std::clamp(octaves, 1, 6);
        ParallelFor(static_cast<int>(n), [&](int zi) {
            const size_t z = static_cast<size_t>(zi);
            for (size_t y = 0; y < n; ++y)
            {
                for (size_t x = 0; x < n; ++x)
                {
                    const float fx = (x + 0.5f) / n;
                    const float fy = (y + 0.5f) / n;
                    const float fz = (z + 0.5f) / n;
                    float sum = 0;
                    float norm = 0;
                    float amplitude = 1;
                    for (int o = 0; o < octaves; ++o)
                    {
                        const float scale = static_cast<float>(1 << o);
                        // 高さに応じて水平方向の参照位置をずらす。整数オフセットでボリュームの周期性を保つ。
                        const uint32_t h = Hash(seed + static_cast<uint32_t>(o) * 977u);
                        const float angle = fy * 2 * kPi;
                        const float ox = std::round(verticalVariation * 2) * std::cos(angle) * 0.5f / scale + Unit(h);
                        const float oz =
                            std::round(verticalVariation * 2) * std::sin(angle) * 0.5f / scale + Unit(Hash(h));
                        const Float4 a = image.Sample(fx * scale + ox, fz * scale + oz);
                        const Float4 b = image.Sample(fz * scale - oz + 0.37f, fy * scale + ox);
                        const float value = (a.X + a.Y + a.Z) / 3 * 0.7f + (b.X + b.Y + b.Z) / 3 * 0.3f;
                        sum += value * amplitude;
                        norm += amplitude;
                        amplitude *= 0.5f;
                    }
                    volume.Density[(z * n + y) * n + x] = Saturate(sum / norm);
                }
            }
        });
        return volume;
    }

    std::vector<Float4> GenerateSolidVolume(const TextureParams& p, int extent)
    {
        const size_t volumeExtent = static_cast<size_t>(std::clamp(extent, 8, 256));
        std::vector<Float4> voxels(volumeExtent * volumeExtent * volumeExtent);
        ParallelFor(static_cast<int>(volumeExtent), [&](int sliceIndex) {
            const size_t z = static_cast<size_t>(sliceIndex);
            for (size_t y = 0; y < volumeExtent; ++y)
            {
                for (size_t x = 0; x < volumeExtent; ++x)
                {
                    const Float3 color = TextureColor(p, (x + 0.5f) / volumeExtent, (y + 0.5f) / volumeExtent,
                                                      (z + 0.5f) / volumeExtent, true);
                    voxels[(z * volumeExtent + y) * volumeExtent + x] = {color.X, color.Y, color.Z, 1};
                }
            }
        });
        return voxels;
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

    const char* TerrainPresetName(TerrainPreset preset)
    {
        switch (preset)
        {
            case TerrainPreset::Default: return "標準（従来）";
            case TerrainPreset::Mountains: return "山脈";
            case TerrainPreset::Hills: return "丘陵";
            case TerrainPreset::Mesa: return "台地（メサ）";
            case TerrainPreset::Island: return "島";
            case TerrainPreset::Canyon: return "渓谷";
            default: return "?";
        }
    }

    void ApplyTerrainPreset(TerrainShapeParams& t, TerrainPreset preset)
    {
        t = TerrainShapeParams{};
        switch (preset)
        {
            case TerrainPreset::Mountains:
                t.MacroWarp = 0.6f;
                t.RidgeBlend = 0.75f;
                t.MountainMask = 0.6f;
                t.Exponent = 1.6f;
                t.MicroDetail = 0.5f;
                t.HydraulicDensity = 0.6f;
                t.ThermalIterations = 8;
                t.TalusAngle = 40.0f;
                t.SnowLine = 0.75f;
                break;
            case TerrainPreset::Hills:
                t.MacroWarp = 0.4f;
                t.Exponent = 1.3f;
                t.HydraulicDensity = 0.3f;
                t.ThermalIterations = 6;
                t.TalusAngle = 30.0f;
                t.RockLine = 0.85f;
                t.SnowLine = 1.1f;
                t.RockSlope = 0.85f;
                break;
            case TerrainPreset::Mesa:
                t.MacroWarp = 0.5f;
                t.Exponent = 1.2f;
                t.TerraceSteps = 5;
                t.TerraceStrength = 0.85f;
                t.ThermalIterations = 3;
                t.TalusAngle = 55.0f;
                t.SandLine = 0.6f;
                t.RockLine = 0.7f;
                t.SnowLine = 1.1f;
                t.RockSlope = 0.45f;
                break;
            case TerrainPreset::Island:
                t.MacroWarp = 0.5f;
                t.RidgeBlend = 0.4f;
                t.Exponent = 1.4f;
                t.EdgeFalloff = 0.7f;
                t.SeaLevel = 0.12f;
                t.SeaFlatten = 0.6f;
                t.HydraulicDensity = 0.4f;
                t.ThermalIterations = 5;
                t.SandLine = 0.17f;
                t.SnowLine = 0.9f;
                break;
            case TerrainPreset::Canyon:
                t.MacroWarp = 0.8f;
                t.RidgeBlend = 0.3f;
                t.Exponent = 0.7f;
                t.TerraceSteps = 8;
                t.TerraceStrength = 0.5f;
                t.HydraulicDensity = 0.8f;
                t.ErosionRate = 0.5f;
                t.ThermalIterations = 4;
                t.TalusAngle = 60.0f;
                t.SandLine = 0.45f;
                t.RockLine = 0.5f;
                t.SnowLine = 1.1f;
                t.RockSlope = 0.4f;
                break;
            default:
                break;
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

    namespace
    {
        const Float3 kWhite{1, 1, 1};

        void AddQuad(Mesh& mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
        {
            mesh.Indices.insert(mesh.Indices.end(), {a, b, c, a, c, d});
        }

        void BuildSphere(Mesh& mesh, float radius, int segments, int rings)
        {
            for (int r = 0; r <= rings; ++r)
            {
                const float v = static_cast<float>(r) / rings;
                const float phi = v * kPi;
                for (int s = 0; s <= segments; ++s)
                {
                    const float u = static_cast<float>(s) / segments;
                    const float theta = u * 2 * kPi;
                    const Float3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
                    mesh.AddVertex(Mul(n, radius), kWhite, {u, v});
                }
            }
            for (int r = 0; r < rings; ++r)
            {
                for (int s = 0; s < segments; ++s)
                {
                    const uint32_t a = r * (segments + 1) + s;
                    const uint32_t b = a + segments + 1;
                    mesh.Indices.insert(mesh.Indices.end(), {a, a + 1, b, a + 1, b + 1, b});
                }
            }
        }

        void AddBox(Mesh& mesh, Float3 center, float half, Float3 color, bool uvPerFace)
        {
            static const Float3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (const auto& n : normals)
            {
                const Float3 up = std::fabs(n.Y) > 0.5f ? Float3{0, 0, 1} : Float3{0, 1, 0};
                const Float3 right = Cross(up, n);
                const Float3 base = Add(center, Mul(n, half));
                uint32_t v[4];
                const float su[4] = {-1, 1, 1, -1};
                const float sv[4] = {-1, -1, 1, 1};
                for (int i = 0; i < 4; ++i)
                {
                    const Float3 p = Add(base, Add(Mul(right, su[i] * half), Mul(up, sv[i] * half)));
                    const Float2 uv =
                        uvPerFace ? Float2{su[i] * 0.5f + 0.5f, 0.5f - sv[i] * 0.5f} : Float2{p.X + p.Z, p.Y + p.Z};
                    v[i] = mesh.AddVertex(p, color, uv);
                }
                // 外側から見て反時計回り（右手系 OBJ 規約、外向き法線）。
                AddQuad(mesh, v[0], v[1], v[2], v[3]);
            }
        }

        // 既定の境界値では従来の配色と同じ結果になる。
        Float3 TerrainColor(const TerrainShapeParams& t, float h, float slope)
        {
            const Float3 sand{0.76f, 0.70f, 0.50f};
            const Float3 grass{0.22f, 0.45f, 0.16f};
            const Float3 rock{0.42f, 0.38f, 0.34f};
            const Float3 snow{0.95f, 0.96f, 0.98f};
            Float3 c = Mix3(sand, grass, Smooth(t.SandLine - 0.05f, t.SandLine + 0.05f, h));
            c = Mix3(c, rock, Smooth(t.RockLine - 0.075f, t.RockLine + 0.075f, h));
            c = Mix3(c, snow, Smooth(t.SnowLine - 0.05f, t.SnowLine + 0.05f, h));
            return Mix3(c, rock, Smooth(t.RockSlope - 0.125f, t.RockSlope + 0.125f, slope) *
                                     (1 - Smooth(t.SnowLine + 0.02f, t.SnowLine + 0.12f, h)));
        }

        void BuildGrid(Mesh& mesh, int resolution, float size, const std::function<float(float, float)>& height,
                       const std::function<Float3(float, float, float)>& color, int mapsX = 1, int mapsZ = 1)
        {
            resolution = std::clamp(resolution, 2, 512);
            const int stepsX = resolution * mapsX;
            const int stepsZ = resolution * mapsZ;
            const size_t vertexCount = static_cast<size_t>(stepsX + 1) * (stepsZ + 1);
            mesh.Positions.reserve(vertexCount);
            mesh.Colors.reserve(vertexCount);
            mesh.UVs.reserve(vertexCount);
            mesh.Indices.reserve(static_cast<size_t>(stepsX) * stepsZ * 6);
            for (int z = 0; z <= stepsZ; ++z)
            {
                for (int x = 0; x <= stepsX; ++x)
                {
                    const float u = static_cast<float>(x) / resolution;
                    const float v = static_cast<float>(z) / resolution;
                    const float h = height(u, v);
                    mesh.AddVertex({(u - mapsX * 0.5f) * size, h, (v - mapsZ * 0.5f) * size}, color(u, v, h), {u, v});
                }
            }
            for (int z = 0; z < stepsZ; ++z)
            {
                for (int x = 0; x < stepsX; ++x)
                {
                    const uint32_t a = z * (stepsX + 1) + x;
                    const uint32_t b = a + stepsX + 1;
                    mesh.Indices.insert(mesh.Indices.end(), {a, b, a + 1, a + 1, b, b + 1});
                }
            }
            mesh.ComputeNormals();
        }

        struct Random
        {
            uint32_t State;

            float Next()
            {
                State = Hash(State + 0x9e3779b9u);
                return Unit(State);
            }

            float Signed()
            {
                return Next() * 2 - 1;
            }
        };

        // ---- リアルな地形 ----------------------------------------------------------------

        // クランプしていない生の高さを返す。おおむね [0, 1] だが、フラクタルの種類や基底ノイズによっては
        // 大きく範囲外に出る（Hybrid Multifractal・Turbulence・Worley 系では最大で 6 割の点が 1 を超える）。
        float TerrainShapeRaw(const TerrainShapeParams& t, const NoiseParams& n, float u, float v)
        {
            const float baseFrequency = (std::max)(n.Frequency, 0.001f);
            float wu = u;
            float wv = v;
            if (t.MacroWarp > 0)
            {
                NoiseParams warp = n;
                warp.Fractal = FractalType::FBm;
                warp.Octaves = 3;
                warp.Frequency = baseFrequency * 0.35f;
                warp.Seed = n.Seed ^ 0x51ed27u;
                const float wx = FractalNoise(warp, u + 11.3f, v + 4.7f, 0.73f, false) * 2 - 1;
                warp.Seed ^= 0x9e3779b9u;
                const float wz = FractalNoise(warp, u + 2.9f, v + 17.1f, 0.21f, false) * 2 - 1;
                const float strength = t.MacroWarp / baseFrequency;
                wu += wx * strength;
                wv += wz * strength;
            }
            float h = FractalNoiseUnclamped(n, wu, wv, 0.37f, false);
            if (t.RidgeBlend > 0)
            {
                NoiseParams ridge = n;
                ridge.Fractal = FractalType::Ridged;
                ridge.RidgeOffset = 1.0f;
                ridge.Seed = n.Seed ^ 0x2545f491u;
                const float r = FractalNoiseUnclamped(ridge, wu, wv, 0.37f, false);
                // 大きな起伏を保ったまま、稜線だけを鋭くする。
                h = Lerp(h, r * (0.35f + 0.65f * h), t.RidgeBlend);
            }
            if (t.MountainMask > 0)
            {
                NoiseParams mask = n;
                mask.Fractal = FractalType::FBm;
                mask.Octaves = 3;
                mask.Frequency = (std::max)(t.MaskFrequency, 0.01f);
                mask.Seed = n.Seed ^ 0x68e31da4u;
                const float m = Smooth(0.35f, 0.65f, FractalNoise(mask, u, v, 0.11f, false));
                h *= Lerp(1.0f, 0.15f + 0.85f * m, t.MountainMask);
            }
            if (t.MicroDetail > 0)
            {
                NoiseParams detail = n;
                detail.Fractal = FractalType::Ridged;
                detail.RidgeOffset = 1.0f;
                detail.Octaves = 3;
                detail.Frequency = baseFrequency * 8;
                detail.Seed = n.Seed ^ 0xb5297a4du;
                const float d = FractalNoise(detail, wu, wv, 0.59f, false) - 0.5f;
                h += d * t.MicroDetail * 0.08f * Smooth(0.25f, 0.75f, h);
            }
            return h;
        }

        // 正規化済みの高さ [0, 1] に、高さカーブ・段丘・外周・海面を適用する。
        float TerrainShapePost(const TerrainShapeParams& t, float h, float u, float v, int mapsX, int mapsZ)
        {
            if (t.Exponent != 1.0f)
            {
                h = std::pow(h, (std::max)(t.Exponent, 0.05f));
            }
            if (t.TerraceSteps > 0 && t.TerraceStrength > 0)
            {
                const float steps = static_cast<float>(t.TerraceSteps);
                const float level = h * steps;
                const float floorLevel = std::floor(level);
                // 平らな段の後に急な崖が来る形。
                const float terrace = (floorLevel + Smooth(0.55f, 1.0f, level - floorLevel)) / steps;
                h = Lerp(h, terrace, t.TerraceStrength);
            }
            if (t.EdgeFalloff > 0)
            {
                const float dx = u / mapsX * 2 - 1;
                const float dz = v / mapsZ * 2 - 1;
                const float distance = std::sqrt(dx * dx + dz * dz);
                h *= Smooth(0.0f, t.EdgeFalloff, 1 - distance);
            }
            if (t.SeaLevel > 0 && h < t.SeaLevel)
            {
                h = t.SeaLevel - (t.SeaLevel - h) * (1 - Saturate(t.SeaFlatten));
            }
            return h;
        }

        // 熱侵食。安息角を超える高低差を隣接セルと対称にやり取りするため、質量が保存され並列化できる。
        void ThermalErosion(std::vector<float>& heights, int width, int depth, float cell, int iterations,
                            float talusAngle)
        {
            if (iterations <= 0)
            {
                return;
            }
            const float talus = std::tan(std::clamp(talusAngle, 1.0f, 89.0f) * kPi / 180.0f) * cell;
            constexpr float kRate = 0.2f;
            std::vector<float> next(heights.size());
            for (int iteration = 0; iteration < iterations; ++iteration)
            {
                ParallelFor(depth, [&](int z) {
                    for (int x = 0; x < width; ++x)
                    {
                        const size_t i = static_cast<size_t>(z) * width + x;
                        const float h = heights[i];
                        float delta = 0;
                        const auto exchange = [&](int nx, int nz) {
                            if (nx < 0 || nz < 0 || nx >= width || nz >= depth)
                            {
                                return;
                            }
                            const float diff = heights[static_cast<size_t>(nz) * width + nx] - h;
                            const float excess = std::fabs(diff) - talus;
                            if (excess > 0)
                            {
                                delta += (diff > 0 ? 0.5f : -0.5f) * kRate * excess;
                            }
                        };
                        exchange(x - 1, z);
                        exchange(x + 1, z);
                        exchange(x, z - 1);
                        exchange(x, z + 1);
                        next[i] = h + delta;
                    }
                });
                heights.swap(next);
            }
        }

        // 水滴による水食（侵食と堆積）。高さはセル単位に換算して処理し、解像度による差を抑える。
        // 決定的な結果にするため、水滴は 1 スレッドで順に流す。
        void HydraulicErosion(std::vector<float>& heights, int width, int depth, float cell, int resolution,
                              const TerrainShapeParams& t, uint32_t seed)
        {
            if (t.HydraulicDensity <= 0 || width < 3 || depth < 3)
            {
                return;
            }
            const float toCell = 1.0f / (std::max)(cell, 1e-5f);
            for (auto& h : heights)
            {
                h *= toCell;
            }
            const auto at = [&](int x, int z) -> float& {
                return heights[static_cast<size_t>(z) * width + x];
            };
            const size_t droplets =
                static_cast<size_t>(static_cast<double>(t.HydraulicDensity) * width * depth);
            const int lifetime = std::clamp(static_cast<int>(30.0f * resolution / 128.0f), 16, 96);
            const float inertia = Saturate(t.Inertia);
            const float erosionRate = Saturate(t.ErosionRate);
            const float depositionRate = Saturate(t.DepositionRate);
            const float evaporation = Saturate(t.Evaporation);
            constexpr float kGravity = 4.0f;
            constexpr float kMinCapacity = 0.01f;
            Random random{Hash(seed ^ 0xe7037ed1u)};
            for (size_t droplet = 0; droplet < droplets; ++droplet)
            {
                float px = random.Next() * (width - 1.001f);
                float pz = random.Next() * (depth - 1.001f);
                float dirX = 0;
                float dirZ = 0;
                float speed = 1;
                float water = 1;
                float sediment = 0;
                for (int step = 0; step < lifetime; ++step)
                {
                    const int cx = static_cast<int>(px);
                    const int cz = static_cast<int>(pz);
                    const float ox = px - cx;
                    const float oz = pz - cz;
                    const float h00 = at(cx, cz), h10 = at(cx + 1, cz);
                    const float h01 = at(cx, cz + 1), h11 = at(cx + 1, cz + 1);
                    const float gx = (h10 - h00) * (1 - oz) + (h11 - h01) * oz;
                    const float gz = (h01 - h00) * (1 - ox) + (h11 - h10) * ox;
                    const float height = Lerp(Lerp(h00, h10, ox), Lerp(h01, h11, ox), oz);
                    dirX = dirX * inertia - gx * (1 - inertia);
                    dirZ = dirZ * inertia - gz * (1 - inertia);
                    const float length = std::sqrt(dirX * dirX + dirZ * dirZ);
                    if (length < 1e-6f)
                    {
                        break;
                    }
                    dirX /= length;
                    dirZ /= length;
                    px += dirX;
                    pz += dirZ;
                    if (px < 0 || pz < 0 || px >= width - 1 || pz >= depth - 1)
                    {
                        break;
                    }
                    const int nx = static_cast<int>(px);
                    const int nz = static_cast<int>(pz);
                    const float nox = px - nx;
                    const float noz = pz - nz;
                    const float newHeight = Lerp(Lerp(at(nx, nz), at(nx + 1, nz), nox),
                                                 Lerp(at(nx, nz + 1), at(nx + 1, nz + 1), nox), noz);
                    const float deltaHeight = newHeight - height;
                    const float capacity =
                        (std::max)(-deltaHeight * speed * water * t.SedimentCapacity, kMinCapacity);
                    const float w00 = (1 - ox) * (1 - oz), w10 = ox * (1 - oz), w01 = (1 - ox) * oz, w11 = ox * oz;
                    if (sediment > capacity || deltaHeight > 0)
                    {
                        // 上り坂では窪みを埋め、容量超過分は落とす。
                        const float deposit = deltaHeight > 0 ? (std::min)(deltaHeight, sediment)
                                                              : (sediment - capacity) * depositionRate;
                        sediment -= deposit;
                        at(cx, cz) += deposit * w00;
                        at(cx + 1, cz) += deposit * w10;
                        at(cx, cz + 1) += deposit * w01;
                        at(cx + 1, cz + 1) += deposit * w11;
                    }
                    else
                    {
                        // 下り坂の高低差以上は削らず、穴が開くのを防ぐ。
                        const float erode = (std::min)((capacity - sediment) * erosionRate, -deltaHeight);
                        at(cx, cz) -= erode * w00;
                        at(cx + 1, cz) -= erode * w10;
                        at(cx, cz + 1) -= erode * w01;
                        at(cx + 1, cz + 1) -= erode * w11;
                        sediment += erode;
                    }
                    speed = std::sqrt((std::max)(0.0f, speed * speed - deltaHeight * kGravity));
                    water *= 1 - evaporation;
                    if (water < 0.01f)
                    {
                        break;
                    }
                }
            }
            for (auto& h : heights)
            {
                h *= cell;
            }
        }

        // 地形全体の高さ（ワールド単位）を (stepsX + 1) × (stepsZ + 1) の配列で返す。
        // 侵食は格子全体を参照するため、プレビューと保存の両方でこの関数を使う。
        std::vector<float> BuildTerrainHeights(const ModelParams& p, int resolution, int mapsX, int mapsZ)
        {
            const int width = resolution * mapsX + 1;
            const int depth = resolution * mapsZ + 1;
            NoiseParams noise = p.Noise;
            noise.Seed ^= p.Seed;
            std::vector<float> heights(static_cast<size_t>(width) * depth);
            ParallelFor(depth, [&](int z) {
                const float v = static_cast<float>(z) / resolution;
                for (int x = 0; x < width; ++x)
                {
                    const float u = static_cast<float>(x) / resolution;
                    heights[static_cast<size_t>(z) * width + x] = TerrainShapeRaw(p.Terrain, noise, u, v);
                }
            });
            // 高さが山頂で水平に切り取られる原因: 従来はノイズ値を [0, 1] でハードクランプしており、
            // 1 を超えた峰がすべて同じ高さで平らになっていた。クランプの代わりに、地形全体の実際の
            // 最小・最大で線形に正規化し直す。形は一切潰れず、範囲内に収まる設定では従来と同じ値になる。
            float low = 0.0f;
            float high = 1.0f;
            for (const float h : heights)
            {
                if (std::isfinite(h))
                {
                    low = (std::min)(low, h);
                    high = (std::max)(high, h);
                }
            }
            const float invRange = 1.0f / (std::max)(high - low, 1e-6f);
            ParallelFor(depth, [&](int z) {
                const float v = static_cast<float>(z) / resolution;
                for (int x = 0; x < width; ++x)
                {
                    float& h = heights[static_cast<size_t>(z) * width + x];
                    const float normalized = std::isfinite(h) ? (h - low) * invRange : 0.0f;
                    h = TerrainShapePost(p.Terrain, normalized, static_cast<float>(x) / resolution, v, mapsX,
                                         mapsZ) * p.Height;
                }
            });
            const float cell = p.Size / resolution;
            HydraulicErosion(heights, width, depth, cell, resolution, p.Terrain, noise.Seed);
            ThermalErosion(heights, width, depth, cell, p.Terrain.ThermalIterations, p.Terrain.TalusAngle);
            return heights;
        }

        Float3 Rotate(Float3 v, Float3 axis, float angle)
        {
            // Rodrigues の回転公式。
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return Add(Add(Mul(v, c), Mul(Cross(axis, v), s)), Mul(axis, Dot(axis, v) * (1 - c)));
        }

        void AddBranch(Mesh& mesh, Float3 a, Float3 b, float ra, float rb, int segments, Float3 color, float v0,
                       float v1)
        {
            const Float3 axis = Normalize(Sub(b, a));
            const Float3 helper = std::fabs(axis.Y) < 0.95f ? Float3{0, 1, 0} : Float3{1, 0, 0};
            const Float3 x = Normalize(Cross(helper, axis));
            const Float3 y = Cross(axis, x);
            const uint32_t first = static_cast<uint32_t>(mesh.Positions.size());
            for (int i = 0; i <= segments; ++i)
            {
                const float t = static_cast<float>(i) / segments * 2 * kPi;
                const Float3 dir = Add(Mul(x, std::cos(t)), Mul(y, std::sin(t)));
                mesh.AddVertex(Add(a, Mul(dir, ra)), color, {static_cast<float>(i) / segments, v0});
                mesh.AddVertex(Add(b, Mul(dir, rb)), color, {static_cast<float>(i) / segments, v1});
            }
            for (int i = 0; i < segments; ++i)
            {
                const uint32_t p = first + i * 2;
                // 外向きの頂点順（外側から見て反時計回り）。
                mesh.Indices.insert(mesh.Indices.end(), {p, p + 2, p + 1, p + 2, p + 3, p + 1});
            }
        }

        // ------------------------------------------------------------------------------------------------
        // フラクタルの葉。葉軸（中央の葉脈）に葉全体を縮小した子を配置する。
        // 再帰の深さが尽きるまで自己相似な構造を繰り返し、最終段階で葉身を作る。
        struct LeafBuilder
        {
            const LeafParams& P;
            Random Rng;
            Mesh& Out;

            // 葉の平面上の葉身（垂れ・折り曲げ前は z = 0）。基部、単位方向、長さ、半幅を指定する。
            void Blade(Float2 base, Float2 dir, float length, float halfWidth, float tint)
            {
                const int segments = std::clamp(P.Segments, 2, 16);
                const Float2 side{-dir.Y, dir.X};
                const Float3 baseColor{P.Color[0], P.Color[1], P.Color[2]};
                const Float3 tipColor{P.TipColor[0], P.TipColor[1], P.TipColor[2]};
                // 表裏それぞれに 3 列（左端・中央の葉脈・右端）×（分割数 + 1）行を作る。
                std::vector<Float3> positions;
                std::vector<Float3> colors;
                std::vector<Float2> uvs;
                for (int row = 0; row <= segments; ++row)
                {
                    const float t = static_cast<float>(row) / segments;
                    // 披針形の輪郭。長さの約 35 % で最も広く、先端は尖り、葉柄は細い。
                    // float の sin(pi) はわずかに負になる。負の底の pow が NaN となり、先端の行が壊れていた。
                    const float outline = std::pow((std::max)(std::sin(kPi * std::pow(t, 0.75f)), 0.0f), 0.9f);
                    const float w = halfWidth * outline;
                    for (int column = -1; column <= 1; ++column)
                    {
                        const Float2 p{base.X + dir.X * length * t + side.X * w * column,
                                       base.Y + dir.Y * length * t + side.Y * w * column};
                        positions.push_back({p.X, p.Y, 0});
                        const Float3 c = Mix3(baseColor, tipColor, t * 0.8f);
                        colors.push_back(Mul(c, tint * (column == 0 ? 0.85f : 1.0f))); // 中央の葉脈を暗くする。
                        uvs.push_back({0.5f + 0.5f * column * outline, t});
                    }
                }
                for (int face = 0; face < 2; ++face)
                {
                    const uint32_t first = static_cast<uint32_t>(Out.Positions.size());
                    for (size_t i = 0; i < positions.size(); ++i)
                    {
                        Out.AddVertex(positions[i], colors[i], uvs[i]);
                    }
                    for (int row = 0; row < segments; ++row)
                    {
                        for (int column = 0; column < 2; ++column)
                        {
                            const uint32_t a = first + row * 3 + column;
                            const uint32_t b = a + 1;
                            const uint32_t c = a + 3;
                            const uint32_t d = a + 4;
                            // 表面は +Z、裏面は -Z。法線が打ち消し合わないよう頂点を分離する。
                            if (face == 0)
                            {
                                Out.Indices.insert(Out.Indices.end(), {a, d, b, a, c, d});
                            }
                            else
                            {
                                Out.Indices.insert(Out.Indices.end(), {a, b, d, a, d, c});
                            }
                        }
                    }
                }
            }

            // 複葉の葉軸を作る細い帯。
            void Rachis(Float2 base, Float2 dir, float length, float halfWidth, float tint)
            {
                const Float2 side{-dir.Y, dir.X};
                const Float3 color = Mul(Float3{P.Color[0] * 0.8f, P.Color[1] * 0.75f, P.Color[2] * 0.6f}, tint);
                const Float2 tip{base.X + dir.X * length, base.Y + dir.Y * length};
                const Float3 corners[4] = {{base.X - side.X * halfWidth, base.Y - side.Y * halfWidth, 0},
                                           {base.X + side.X * halfWidth, base.Y + side.Y * halfWidth, 0},
                                           {tip.X + side.X * halfWidth * 0.4f, tip.Y + side.Y * halfWidth * 0.4f, 0},
                                           {tip.X - side.X * halfWidth * 0.4f, tip.Y - side.Y * halfWidth * 0.4f, 0}};
                for (int face = 0; face < 2; ++face)
                {
                    uint32_t v[4];
                    for (int i = 0; i < 4; ++i)
                    {
                        v[i] = Out.AddVertex(corners[i], color, {0.5f, i < 2 ? 0.0f : 1.0f});
                    }
                    // side が dir の左側にあるとき、頂点は +Z を軸とする右手系の順序に並ぶ。
                    if (face == 0)
                    {
                        Out.Indices.insert(Out.Indices.end(), {v[0], v[2], v[1], v[0], v[3], v[2]});
                    }
                    else
                    {
                        Out.Indices.insert(Out.Indices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
                    }
                }
            }

            static Float2 Turn(Float2 d, float angle)
            {
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                return {d.X * c - d.Y * s, d.X * s + d.Y * c};
            }

            void Grow(Float2 base, Float2 dir, float length, int depth)
            {
                const float tint = 1.0f + Rng.Signed() * P.ColorVariation;
                if (depth <= 0 || P.Shape == LeafShape::Simple || length < 1e-3f)
                {
                    Blade(base, dir, length, length * P.Width * 0.5f, tint);
                    return;
                }
                const float angle = P.LeafletAngle * kPi / 180;
                const int count = std::clamp(P.Leaflets, 1, 12);
                if (P.Shape == LeafShape::Palmate)
                {
                    // 裂片は基部から扇状に広がり、中央の裂片が葉の先へ続く。
                    const int lobes = count | 1; // 左右対称になる奇数個。
                    for (int i = 0; i < lobes; ++i)
                    {
                        const float u = lobes == 1 ? 0.0f : (static_cast<float>(i) / (lobes - 1)) * 2 - 1; // -1..1
                        // 側方の裂片は短くする。深い階層ほど縮め、子の裂片を親の輪郭内に収める。
                        const float lobeLength = length * (1 - P.Taper * 0.6f * std::fabs(u)) *
                                                 (depth > 1 ? (std::min)(P.LeafletScale + 0.35f, 1.0f) : 1.0f);
                        const Float2 childDir = Turn(dir, u * angle * 1.4f + Rng.Signed() * 0.05f);
                        Grow(base, childDir, lobeLength, depth - 1);
                    }
                    return;
                }
                const bool fern = P.Shape == LeafShape::Fern;
                const int pairs = fern ? count * 2 : count;
                Rachis(base, dir, length, length * 0.012f + 0.002f, tint);
                for (int i = 0; i < pairs; ++i)
                {
                    // 小葉は葉軸の少し上から始める。シダの小葉は左右交互に配置する。
                    const float t = 0.12f + 0.78f * (static_cast<float>(i) + 0.5f) / pairs;
                    const float scale = P.LeafletScale * (1 - P.Taper * t) * (fern ? 1.2f : 1.0f);
                    const Float2 at{base.X + dir.X * length * t, base.Y + dir.Y * length * t};
                    const float childLength = length * scale;
                    for (int sideSign = -1; sideSign <= 1; sideSign += 2)
                    {
                        if (fern && ((i & 1) ? sideSign < 0 : sideSign > 0) && depth == 1)
                        {
                            // 最終段階の小葉を交互に配置し、葉全体が鏡像に見えないようにする。
                            const Float2 offset{at.X + dir.X * length * 0.5f / pairs,
                                                at.Y + dir.Y * length * 0.5f / pairs};
                            Grow(offset, Turn(dir, sideSign * angle), childLength, depth - 1);
                            continue;
                        }
                        Grow(at, Turn(dir, sideSign * angle + Rng.Signed() * 0.04f), childLength, depth - 1);
                    }
                }
                // 先端の小葉。
                const float terminal = length * (fern ? 0.9f : 1.0f) * P.LeafletScale;
                const Float2 tip{base.X + dir.X * (length - terminal * 0.2f),
                                 base.Y + dir.Y * (length - terminal * 0.2f)};
                Grow(tip, dir, terminal, depth - 1);
            }
        };
    }

    const char* LeafShapeName(LeafShape shape)
    {
        static const char* names[] = {"板ポリ (軽量)", "単葉", "羽状複葉 (フラクタル)", "シダ (フラクタル)",
                                      "掌状 (フラクタル)"};
        return names[std::clamp(static_cast<int>(shape), 0, static_cast<int>(LeafShape::Count) - 1)];
    }

    Mesh GenerateLeaf(const LeafParams& params, uint32_t seed)
    {
        Mesh mesh;
        LeafBuilder builder{params, Random{Hash(seed ^ 0x1eafu)}, mesh};
        if (params.Shape == LeafShape::Card)
        {
            builder.Blade({0, 0}, {0, 1}, 1.0f, 0.5f, 1.0f);
            // 矩形カードでは各行を全幅に広げる（頂点は 3 列の行優先順）。
            for (size_t i = 0; i < mesh.Positions.size(); ++i)
            {
                const int column = static_cast<int>(i % 3); // 0 = +X 側の端、1 = 中央の葉脈、2 = -X 側の端。
                mesh.Positions[i].X = (1 - column) * 0.3f;
                mesh.UVs[i].X = 0.5f - (1 - column) * 0.5f;
            }
        }
        else
        {
            builder.Grow({0, 0}, {0, 1}, 1.0f, std::clamp(params.Depth, 0, 4));
        }
        // 葉全体の長さを 1 に正規化し、3D 空間で垂れと折り曲げを適用する。
        Float3 minimum;
        Float3 maximum;
        mesh.Bounds(minimum, maximum);
        const float extent = (std::max)(maximum.Y - minimum.Y, 1e-4f);
        for (auto& p : mesh.Positions)
        {
            p = Mul(p, 1.0f / extent);
            const float along = Saturate(p.Y);
            p.Z -= params.Droop * along * along; // 先端を下方・後方へ曲げる。
            p.Z += params.Fold * std::fabs(p.X); // V 字の断面。
        }
        mesh.ComputeNormals();
        return mesh;
    }

    namespace
    {
        struct LeafPlacement
        {
            Float3 Origin;
            Float3 Forward;
            Float3 Normal;
            float Tint;
        };

        void AppendLeaf(Mesh& mesh, const Mesh& leaf, const LeafPlacement& placement, float size)
        {
            const Float3 forward = Normalize(placement.Forward);
            Float3 side = Cross(forward, placement.Normal);
            if (Dot(side, side) < 1e-8f)
            {
                const Float3 helper = std::fabs(forward.Y) < 0.95f ? Float3{0, 1, 0} : Float3{1, 0, 0};
                side = Cross(forward, helper);
            }
            side = Normalize(side);
            // 変換後の法線と三角形の頂点順が一致するよう、右手系の基底を保つ。
            const Float3 normal = Cross(side, forward);
            const uint32_t firstVertex = static_cast<uint32_t>(mesh.Positions.size());
            for (size_t leafVertexIndex = 0; leafVertexIndex < leaf.Positions.size(); ++leafVertexIndex)
            {
                const Float3& leafPosition = leaf.Positions[leafVertexIndex];
                const Float3 worldPosition = Add(
                    placement.Origin,
                    Mul(Add(Add(Mul(side, leafPosition.X), Mul(forward, leafPosition.Y)), Mul(normal, leafPosition.Z)),
                        size));
                const uint32_t vertexIndex = mesh.AddVertex(
                    worldPosition, Mul(leaf.Colors[leafVertexIndex], placement.Tint), leaf.UVs[leafVertexIndex]);
                const Float3& leafNormal = leaf.Normals[leafVertexIndex];
                mesh.Normals[vertexIndex] =
                    Normalize(Add(Add(Mul(side, leafNormal.X), Mul(forward, leafNormal.Y)), Mul(normal, leafNormal.Z)));
            }
            for (uint32_t index : leaf.Indices)
            {
                mesh.Indices.push_back(firstVertex + index);
            }
        }

        void GrowTree(Mesh& mesh, const ModelParams& p, Random& random, Float3 origin, Float3 direction, float length,
                      float radius, int depth, std::vector<LeafPlacement>& leaves)
        {
            // 屈性で +Yへ曲げる。シード付き乱数の揺らぎで結果の再現性を保つ。
            direction = Normalize(Add(
                direction, Float3{random.Signed() * p.Jitter * 0.3f, p.Tropism, random.Signed() * p.Jitter * 0.3f}));
            const Float3 end = Add(origin, Mul(direction, length));
            const float endRadius = radius * (depth <= 1 ? 0.35f : p.RadiusRatio + (1 - p.RadiusRatio) * 0.25f);
            const float bark = 0.6f + 0.4f * random.Next();
            AddBranch(mesh, origin, end, radius, endRadius, (std::max)(3, p.RadialSegments - (p.Depth - depth)),
                      {0.36f * bark, 0.25f * bark, 0.16f * bark}, 0, length);
            if (depth <= 1)
            {
                if (p.Leaves)
                {
                    // 三角形数の予算に葉の詳細度を合わせられるよう、先に配置位置を収集する。
                    for (int i = 0; i < std::clamp(p.LeavesPerTip, 1, 12); ++i)
                    {
                        const Float3 jitter{random.Signed(), random.Signed() * 0.5f, random.Signed()};
                        const Float3 forward = Normalize(Add(direction, Mul(jitter, 1.2f)));
                        // 葉をほぼ上向き（光の方向）にし、軸周りにランダムなねじれを加える。
                        const Float3 normal =
                            Normalize(Add(Float3{0, 1, 0}, Mul(Float3{random.Signed(), 0, random.Signed()}, 0.6f)));
                        leaves.push_back(
                            {Add(end, Mul(jitter, p.LeafSize * 0.3f)), forward, normal, 0.75f + 0.25f * random.Next()});
                    }
                }
                return;
            }
            const Float3 helper = std::fabs(direction.Y) < 0.95f ? Float3{0, 1, 0} : Float3{1, 0, 0};
            const Float3 side = Normalize(Cross(helper, direction));
            const float golden = 2.39996323f;
            const float phase = random.Next() * 2 * kPi;
            for (int i = 0; i < p.Branches; ++i)
            {
                const float spread = (p.SpreadAngle + random.Signed() * p.SpreadAngle * p.Jitter) * kPi / 180;
                Float3 child = Rotate(direction, side, spread);
                child = Rotate(child, direction, phase + golden * i);
                const float scale = p.LengthRatio * (1 + random.Signed() * p.Jitter * 0.3f);
                GrowTree(mesh, p, random, end, child, length * scale, endRadius, depth - 1, leaves);
            }
            // depth が 2 より大きい場合、枝の途中に小枝を生やす。
            if (depth > 2)
            {
                GrowTree(mesh, p, random, end, direction, length * p.LengthRatio * 0.9f, endRadius * 0.95f, depth - 2,
                         leaves);
            }
        }

        void Menger(Mesh& mesh, Float3 center, float half, int level, Float3 color)
        {
            if (level <= 0)
            {
                AddBox(mesh, center, half, color, false);
                return;
            }
            const float child = half / 3;
            for (int z = -1; z <= 1; ++z)
            {
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        if ((x == 0) + (y == 0) + (z == 0) >= 2)
                        {
                            continue;
                        }
                        const Float3 tint{0.5f + 0.25f * (x + 1) * 0.5f, 0.55f + 0.2f * (y + 1) * 0.5f,
                                          0.7f + 0.15f * (z + 1) * 0.5f};
                        Menger(mesh, Add(center, Float3{x * child * 2, y * child * 2, z * child * 2}), child, level - 1,
                               level == 1
                                   ? Float3{color.X * tint.X * 1.3f, color.Y * tint.Y * 1.3f, color.Z * tint.Z * 1.3f}
                                   : color);
                    }
                }
            }
        }

        void Sierpinski(Mesh& mesh, Float3 a, Float3 b, Float3 c, Float3 d, int level, Float3 color)
        {
            if (level <= 0)
            {
                Float3 faces[4][3] = {{a, c, b}, {a, b, d}, {b, c, d}, {c, a, d}};
                const Float3 centroid = Mul(Add(Add(a, b), Add(c, d)), 0.25f);
                for (auto& face : faces)
                {
                    // 各面を四面体の中心から外側へ向ける。
                    const Float3 normal = Cross(Sub(face[1], face[0]), Sub(face[2], face[0]));
                    if (Dot(normal, Sub(face[0], centroid)) < 0)
                    {
                        std::swap(face[1], face[2]);
                    }
                    // 頂点を共有せず、面ごとの平らな陰影を作る。
                    const uint32_t i0 = mesh.AddVertex(face[0], color);
                    const uint32_t i1 = mesh.AddVertex(face[1], color);
                    const uint32_t i2 = mesh.AddVertex(face[2], color);
                    mesh.Indices.insert(mesh.Indices.end(), {i0, i1, i2});
                }
                return;
            }
            auto mid = [](Float3 p, Float3 q) {
                return Mul(Add(p, q), 0.5f);
            };
            const Float3 ab = mid(a, b);
            const Float3 ac = mid(a, c);
            const Float3 ad = mid(a, d);
            const Float3 bc = mid(b, c);
            const Float3 bd = mid(b, d);
            const Float3 cd = mid(c, d);
            const Float3 warm{color.X * 1.05f, color.Y * 0.95f, color.Z * 0.9f};
            Sierpinski(mesh, a, ab, ac, ad, level - 1, color);
            Sierpinski(mesh, ab, b, bc, bd, level - 1, warm);
            Sierpinski(mesh, ac, bc, c, cd, level - 1, color);
            Sierpinski(mesh, ad, bd, cd, d, level - 1, warm);
        }

        void Icosphere(Mesh& mesh, int subdivisions)
        {
            const float t = (1 + std::sqrt(5.0f)) / 2;
            std::vector<Float3> v = {{-1, t, 0},  {1, t, 0},  {-1, -t, 0}, {1, -t, 0}, {0, -1, t},  {0, 1, t},
                                     {0, -1, -t}, {0, 1, -t}, {t, 0, -1},  {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1}};
            std::vector<uint32_t> f = {0, 11, 5,  0, 5,  1, 0, 1, 7, 0, 7,  10, 0, 10, 11, 1, 5, 9, 5, 11,
                                       4, 11, 10, 2, 10, 7, 6, 7, 1, 8, 3,  9,  4, 3,  4,  2, 3, 2, 6, 3,
                                       6, 8,  3,  8, 9,  4, 9, 5, 2, 4, 11, 6,  2, 10, 8,  6, 7, 9, 8, 1};
            for (auto& p : v)
            {
                p = Normalize(p);
            }
            for (int s = 0; s < std::clamp(subdivisions, 0, 6); ++s)
            {
                std::vector<uint32_t> next;
                // 辺の中点を、小さい方の頂点インデックスで分類する。
                std::vector<std::vector<std::pair<uint32_t, uint32_t>>> buckets(v.size());
                auto fast = [&](uint32_t a, uint32_t b) {
                    const uint32_t lo = (std::min)(a, b);
                    const uint32_t hi = (std::max)(a, b);
                    for (const auto& entry : buckets[lo])
                    {
                        if (entry.first == hi)
                        {
                            return entry.second;
                        }
                    }
                    v.push_back(Normalize(Mul(Add(v[a], v[b]), 0.5f)));
                    const uint32_t index = static_cast<uint32_t>(v.size() - 1);
                    buckets[lo].push_back({hi, index});
                    return index;
                };
                for (size_t i = 0; i < f.size(); i += 3)
                {
                    const uint32_t a = fast(f[i], f[i + 1]);
                    const uint32_t b = fast(f[i + 1], f[i + 2]);
                    const uint32_t c = fast(f[i + 2], f[i]);
                    next.insert(next.end(), {f[i], a, c, f[i + 1], b, a, f[i + 2], c, b, a, b, c});
                }
                f.swap(next);
            }
            for (const auto& p : v)
            {
                mesh.AddVertex(p, kWhite, {0.5f + std::atan2(p.Z, p.X) / (2 * kPi), 0.5f - std::asin(p.Y) / kPi});
            }
            mesh.Indices = f;
        }
    }

    Mesh GenerateModel(const ModelParams& p)
    {
        Mesh mesh;
        switch (p.Kind)
        {
            case ModelKind::Sphere:
                BuildSphere(mesh, 1.0f, 64, 32);
                mesh.ComputeNormals();
                break;
            case ModelKind::Cube:
                AddBox(mesh, {0, 0, 0}, 0.8f, kWhite, true);
                mesh.ComputeNormals();
                break;
            case ModelKind::Plane:
                BuildGrid(
                    mesh, 1, 2.5f,
                    [](float, float) {
                        return 0.0f;
                    },
                    [](float, float, float) {
                        return kWhite;
                    });
                break;
            case ModelKind::Terrain: {
                const int resolution = std::clamp(p.Resolution, 2, 512);
                const int mapsX = std::clamp(p.TerrainMapsX, 1, kMaxTerrainMaps);
                const int mapsZ = std::clamp(p.TerrainMapsZ, 1, kMaxTerrainMaps);
                const auto heights = BuildTerrainHeights(p, resolution, mapsX, mapsZ);
                const size_t width = static_cast<size_t>(resolution) * mapsX + 1;
                BuildGrid(
                    mesh, resolution, p.Size,
                    [&](float u, float v) {
                        const size_t x = static_cast<size_t>(std::lround(u * resolution));
                        const size_t z = static_cast<size_t>(std::lround(v * resolution));
                        return heights[z * width + x];
                    },
                    [](float, float, float) {
                        return kWhite;
                    },
                    mapsX, mapsZ);
                if (p.TerrainColors)
                {
                    for (size_t i = 0; i < mesh.Positions.size(); ++i)
                    {
                        const float slope = 1 - std::fabs(mesh.Normals[i].Y);
                        mesh.Colors[i] = TerrainColor(p.Terrain, mesh.Positions[i].Y / (std::max)(p.Height, 1e-3f),
                                                      slope * 2.5f);
                    }
                }
                break;
            }
            case ModelKind::FractalTree: {
                Random random{Hash(p.Seed)};
                std::vector<LeafPlacement> leaves;
                GrowTree(mesh, p, random, {0, 0, 0}, {0, 1, 0}, p.TrunkLength, p.TrunkRadius, std::clamp(p.Depth, 1, 9),
                         leaves);
                mesh.ComputeNormals();
                if (!leaves.empty())
                {
                    // 葉のテンプレートを 1 つ作り、全配置位置で共有する。予算内になるまで再帰を減らす。
                    LeafParams leafParams = p.Leaf;
                    Mesh leaf = GenerateLeaf(leafParams, p.Seed);
                    // まず葉身の分割を減らしてフラクタル構造を保ち、その後に再帰の深さを減らす。
                    const size_t budget = static_cast<size_t>((std::max)(p.LeafTriangleBudget, 1000));
                    while (leaf.Indices.size() / 3 * leaves.size() > budget &&
                           (leafParams.Segments > 3 || leafParams.Depth > 0))
                    {
                        if (leafParams.Segments > 3)
                        {
                            --leafParams.Segments;
                        }
                        else
                        {
                            --leafParams.Depth;
                            leafParams.Segments = (std::max)(p.Leaf.Segments, 3);
                        }
                        leaf = GenerateLeaf(leafParams, p.Seed);
                    }
                    const size_t barkVertices = mesh.Positions.size();
                    mesh.Positions.reserve(barkVertices + leaf.Positions.size() * leaves.size());
                    mesh.Indices.reserve(mesh.Indices.size() + leaf.Indices.size() * leaves.size());
                    for (const auto& placement : leaves)
                    {
                        AppendLeaf(mesh, leaf, placement, p.LeafSize);
                    }
                }
                break;
            }
            case ModelKind::FractalLeaf:
                mesh = GenerateLeaf(p.Leaf, p.Seed);
                // 単独の葉は表を上、先端を -Z に向けて平置きする。適切な回転で頂点順を保つ。
                for (auto* list : {&mesh.Positions, &mesh.Normals})
                {
                    for (auto& v : *list)
                    {
                        v = {v.X, v.Z, -v.Y};
                    }
                }
                break;
            case ModelKind::Rock: {
                Icosphere(mesh, p.Subdivisions);
                NoiseParams n = p.Noise;
                n.Seed ^= p.Seed;
                for (auto& position : mesh.Positions)
                {
                    const float d =
                        FractalNoise(n, position.X * 0.5f + 7, position.Y * 0.5f + 3, position.Z * 0.5f + 5, false);
                    position = Mul(position, 1 + (d - 0.5f) * 2 * p.Displacement);
                }
                mesh.ComputeNormals();
                for (size_t i = 0; i < mesh.Positions.size(); ++i)
                {
                    const float g = 0.18f + 0.22f * FractalNoise(n, mesh.Positions[i].X * 2, mesh.Positions[i].Y * 2,
                                                                 mesh.Positions[i].Z * 2, false);
                    mesh.Colors[i] = {g * 1.05f, g, g * 0.92f};
                }
                break;
            }
            case ModelKind::WaterSurface: {
                TextureParams water;
                water.Kind = TextureKind::FractalWater;
                water.Noise = p.Noise;
                water.Noise.Seed ^= p.Seed;
                water.Time = p.Time;
                water.Tileable = false;
                BuildGrid(
                    mesh, p.Resolution, p.Size,
                    [&](float u, float v) {
                        return (TextureScalar(water, u, v, 0, false) - 0.5f) * 2 * p.WaveAmplitude;
                    },
                    [&](float, float, float h) {
                        const float t = Saturate(h / (std::max)(p.WaveAmplitude, 1e-3f) * 0.5f + 0.5f);
                        return Mix3(Float3{0.02f, 0.12f, 0.2f}, Float3{0.1f, 0.4f, 0.5f}, t);
                    });
                break;
            }
            case ModelKind::MengerSponge:
                Menger(mesh, {0, 0, 0}, 1.0f, std::clamp(p.Level, 0, 3), {0.8f, 0.78f, 0.72f});
                break;
            case ModelKind::SierpinskiPyramid: {
                const float s = 1.2f;
                Sierpinski(mesh, {-s, -s * 0.7f, -s * 0.58f}, {s, -s * 0.7f, -s * 0.58f}, {0, -s * 0.7f, s * 1.15f},
                           {0, s * 1.0f, 0}, std::clamp(p.Level, 0, 7), {0.85f, 0.65f, 0.3f});
                mesh.ComputeNormals();
                break;
            }
            default:
                break;
        }
        if (p.Kind == ModelKind::MengerSponge)
        {
            mesh.ComputeNormals();
        }
        return mesh;
    }

    namespace
    {
        // OBJ のテキスト化を高速に行う。iostream の operator<< は 1 値ごとにロケール処理が走り、
        // 数百万頂点では保存時間の大半を占めていた。std::to_chars は ostream (fixed, precision 6)
        // と同じ文字列を出力するため、生成されるファイル内容は従来と同一。
        struct ObjText
        {
            std::string Data;

            void Char(char c) { Data.push_back(c); }
            void Text(const char* text) { Data.append(text); }
            void Float(float value)
            {
                char buffer[64];
                const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 6);
                Data.append(buffer, result.ptr);
            }
            void UInt(uint64_t value)
            {
                char buffer[24];
                const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
                Data.append(buffer, result.ptr);
            }
            void Face(uint64_t a, uint64_t b, uint64_t c)
            {
                Data.push_back('f');
                for (const uint64_t index : {a, b, c})
                {
                    Data.push_back(' ');
                    UInt(index);
                    Data.push_back('/');
                    UInt(index);
                    Data.push_back('/');
                    UInt(index);
                }
                Data.push_back('\n');
            }
        };

        // [0, itemCount) を chunk 件ずつ並列にテキスト化し、順番どおりにファイルへ書き出す。
        // メモリ使用量を抑えるため、一度に保持するのは workers * 2 チャンク分だけにする。
        bool WriteChunksParallel(std::ofstream& output, size_t itemCount, size_t chunk, size_t bytesPerItem,
                                 const std::function<void(size_t, size_t, ObjText&)>& format)
        {
            if (itemCount == 0)
            {
                return static_cast<bool>(output);
            }
            const size_t chunkCount = (itemCount + chunk - 1) / chunk;
            const size_t batch =
                static_cast<size_t>(std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 32)) * 2;
            std::vector<ObjText> texts(std::min(batch, chunkCount));
            for (size_t first = 0; first < chunkCount && output; first += batch)
            {
                const size_t count = std::min(batch, chunkCount - first);
                ParallelFor(static_cast<int>(count), [&](int local) {
                    const size_t begin = (first + local) * chunk;
                    const size_t end = std::min(itemCount, begin + chunk);
                    auto& text = texts[local];
                    text.Data.clear();
                    text.Data.reserve((end - begin) * bytesPerItem);
                    format(begin, end, text);
                });
                for (size_t i = 0; i < count && output; ++i)
                {
                    output.write(texts[i].Data.data(), static_cast<std::streamsize>(texts[i].Data.size()));
                }
            }
            return static_cast<bool>(output);
        }

        bool WriteMtl(const std::filesystem::path& mtlPath, const std::string& diffuseTexture)
        {
            std::ofstream mtl(mtlPath, std::ios::binary | std::ios::trunc);
            mtl << "newmtl procedural\nKd 1 1 1\nKa 0 0 0\nKs 0.04 0.04 0.04\nNs 32\n";
            if (!diffuseTexture.empty())
            {
                mtl << "map_Kd " << diffuseTexture << '\n';
            }
            mtl.close();
            return static_cast<bool>(mtl);
        }

        std::string ObjHeader(const std::filesystem::path& mtlPath)
        {
            // u8string を使う。Windows の string() は日本語など非 ASCII のファイル名で例外や文字化けを起こす。
            const auto mtlName = mtlPath.filename().u8string();
            return std::string(kProceduralObjMarker) + "\nmtllib " + std::string(mtlName.begin(), mtlName.end()) +
                   "\nusemtl procedural\n";
        }
    }

    bool WriteGridObj(const std::filesystem::path& path, const ModelParams& params, bool vertexColors,
                      float uvScale, const std::string& diffuseTexture, std::string& error)
    {
        const bool terrain = params.Kind == ModelKind::Terrain;
        if (!terrain && params.Kind != ModelKind::WaterSurface)
        {
            error = "地形または水面を指定してください";
            return false;
        }
        const int resolution = std::clamp(params.Resolution, 2, 512);
        const int mapsX = terrain ? std::clamp(params.TerrainMapsX, 1, kMaxTerrainMaps) : 1;
        const int mapsZ = terrain ? std::clamp(params.TerrainMapsZ, 1, kMaxTerrainMaps) : 1;
        const int stepsX = resolution * mapsX;
        const int stepsZ = resolution * mapsZ;
        NoiseParams noise = params.Noise;
        noise.Seed ^= params.Seed;
        TextureParams water;
        water.Kind = TextureKind::FractalWater;
        water.Noise = noise;
        water.Time = params.Time;
        water.Tileable = false;
        // 地形は侵食で格子全体を参照するため、先に全体の高さを求める。
        const std::vector<float> terrainHeights =
            terrain ? BuildTerrainHeights(params, resolution, mapsX, mapsZ) : std::vector<float>{};
        const auto position = [&](int x, int z) -> Float3 {
            const float u = static_cast<float>(x) / resolution;
            const float v = static_cast<float>(z) / resolution;
            const float h = terrain ? terrainHeights[static_cast<size_t>(z) * (stepsX + 1) + x]
                                    : (TextureScalar(water, u, v, 0, false) - 0.5f) * 2 * params.WaveAmplitude;
            return {(u - mapsX * 0.5f) * params.Size, h, (v - mapsZ * 0.5f) * params.Size};
        };
        auto mtlPath = path;
        mtlPath.replace_extension(".mtl");
        auto temporary = path;
        temporary += L".partial";
        std::ofstream obj(temporary, std::ios::binary | std::ios::trunc);
        if (!obj)
        {
            error = "OBJ を書き込めません";
            return false;
        }
        const auto header = ObjHeader(mtlPath);
        obj.write(header.data(), static_cast<std::streamsize>(header.size()));

        // 行ブロック単位で「高さ計算 → 法線 → テキスト化」をすべて並列化する。
        // 従来はノイズ評価と書式化が 1 スレッドで直列に走っていた。
        // ブロック前後 1 行ずつを余分に持ち、法線計算に必要な隣接行を参照する。
        constexpr int kBlockRows = 64;
        const size_t rowWidth = static_cast<size_t>(stepsX) + 1;
        std::vector<Float3> rows(static_cast<size_t>(kBlockRows + 2) * rowWidth);
        std::vector<ObjText> rowTexts(kBlockRows);
        for (int blockStart = 0; blockStart <= stepsZ && obj; blockStart += kBlockRows)
        {
            const int blockEnd = (std::min)(stepsZ + 1, blockStart + kBlockRows); // 出力する行 [start, end)
            const int firstRow = (std::max)(0, blockStart - 1);
            const int lastRow = (std::min)(stepsZ, blockEnd); // 参照する行 [firstRow, lastRow]
            const auto rowData = [&](int z) {
                return rows.data() + static_cast<size_t>(z - firstRow) * rowWidth;
            };
            ParallelFor(lastRow - firstRow + 1, [&](int local) {
                const int z = firstRow + local;
                Float3* row = rowData(z);
                for (int x = 0; x <= stepsX; ++x)
                {
                    row[x] = position(x, z);
                }
            });
            ParallelFor(blockEnd - blockStart, [&](int local) {
                const int z = blockStart + local;
                const Float3* previous = z > 0 ? rowData(z - 1) : nullptr;
                const Float3* current = rowData(z);
                const Float3* next = z < stepsZ ? rowData(z + 1) : nullptr;
                ObjText& text = rowTexts[local];
                text.Data.clear();
                text.Data.reserve(rowWidth * (vertexColors ? 160 : 120));
                for (int x = 0; x <= stepsX; ++x)
                {
                    const auto& p = current[x];
                    Float3 normal{};
                    // Match the in-memory grid's area-weighted normals, including its diagonal.
                    const auto addCell = [&](int cellX, const Float3* upper, const Float3* lower, bool onUpper) {
                        const auto first = Cross(Sub(lower[cellX], upper[cellX]), Sub(upper[cellX + 1], upper[cellX]));
                        const auto second =
                            Cross(Sub(lower[cellX], upper[cellX + 1]), Sub(lower[cellX + 1], upper[cellX + 1]));
                        if (onUpper || x == cellX)
                        {
                            normal = Add(normal, first);
                        }
                        if (!onUpper || x != cellX)
                        {
                            normal = Add(normal, second);
                        }
                    };
                    if (previous)
                    {
                        if (x > 0) addCell(x - 1, previous, current, false);
                        if (x < stepsX) addCell(x, previous, current, false);
                    }
                    if (next)
                    {
                        if (x > 0) addCell(x - 1, current, next, true);
                        if (x < stepsX) addCell(x, current, next, true);
                    }
                    normal = Normalize(normal);
                    text.Text("v ");
                    text.Float(p.X);
                    text.Char(' ');
                    text.Float(p.Y);
                    text.Char(' ');
                    text.Float(p.Z);
                    if (vertexColors)
                    {
                        const float wave = Saturate(p.Y / (std::max)(params.WaveAmplitude, 1e-3f) * 0.5f + 0.5f);
                        const Float3 color = terrain
                            ? (params.TerrainColors ? TerrainColor(params.Terrain, p.Y / (std::max)(params.Height, 1e-3f),
                                                                  (1 - std::fabs(normal.Y)) * 2.5f) : kWhite)
                            : Mix3(Float3{0.02f, 0.12f, 0.2f}, Float3{0.1f, 0.4f, 0.5f}, wave);
                        text.Char(' ');
                        text.Float(color.X);
                        text.Char(' ');
                        text.Float(color.Y);
                        text.Char(' ');
                        text.Float(color.Z);
                    }
                    text.Text("\nvt ");
                    text.Float(static_cast<float>(x) / resolution * uvScale);
                    text.Char(' ');
                    text.Float(1 - static_cast<float>(z) / resolution * uvScale);
                    text.Text("\nvn ");
                    text.Float(normal.X);
                    text.Char(' ');
                    text.Float(normal.Y);
                    text.Char(' ');
                    text.Float(normal.Z);
                    text.Char('\n');
                }
            });
            for (int local = 0; local < blockEnd - blockStart && obj; ++local)
            {
                obj.write(rowTexts[local].Data.data(), static_cast<std::streamsize>(rowTexts[local].Data.size()));
            }
        }
        const uint64_t stride = static_cast<uint64_t>(stepsX) + 1;
        WriteChunksParallel(obj, static_cast<size_t>(stepsZ), 16, static_cast<size_t>(stepsX) * 2 * 64,
                            [&](size_t begin, size_t end, ObjText& text) {
                                for (size_t z = begin; z < end; ++z)
                                {
                                    for (int x = 0; x < stepsX; ++x)
                                    {
                                        const uint64_t a = z * stride + x + 1;
                                        const uint64_t b = a + stride;
                                        text.Face(a, b, a + 1);
                                        text.Face(a + 1, b, b + 1);
                                    }
                                }
                            });
        obj.close();
        if (obj && WriteMtl(mtlPath, diffuseTexture) &&
            MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            return true;
        }
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        error = "地形または水面の OBJ を保存できません";
        return false;
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
        const auto header = ObjHeader(mtlPath);
        obj.write(header.data(), static_cast<std::streamsize>(header.size()));
        constexpr size_t kChunk = 16384;
        WriteChunksParallel(obj, mesh.Positions.size(), kChunk, vertexColors ? 64 : 40,
                            [&](size_t begin, size_t end, ObjText& text) {
                                for (size_t i = begin; i < end; ++i)
                                {
                                    const auto& p = mesh.Positions[i];
                                    text.Text("v ");
                                    text.Float(p.X);
                                    text.Char(' ');
                                    text.Float(p.Y);
                                    text.Char(' ');
                                    text.Float(p.Z);
                                    if (vertexColors)
                                    {
                                        const auto& c = mesh.Colors[i];
                                        text.Char(' ');
                                        text.Float(c.X);
                                        text.Char(' ');
                                        text.Float(c.Y);
                                        text.Char(' ');
                                        text.Float(c.Z);
                                    }
                                    text.Char('\n');
                                }
                            });
        WriteChunksParallel(obj, mesh.UVs.size(), kChunk, 24, [&](size_t begin, size_t end, ObjText& text) {
            for (size_t i = begin; i < end; ++i)
            {
                text.Text("vt ");
                text.Float(mesh.UVs[i].X);
                text.Char(' ');
                text.Float(1 - mesh.UVs[i].Y);
                text.Char('\n');
            }
        });
        WriteChunksParallel(obj, mesh.Normals.size(), kChunk, 32, [&](size_t begin, size_t end, ObjText& text) {
            for (size_t i = begin; i < end; ++i)
            {
                const auto& n = mesh.Normals[i];
                text.Text("vn ");
                text.Float(n.X);
                text.Char(' ');
                text.Float(n.Y);
                text.Char(' ');
                text.Float(n.Z);
                text.Char('\n');
            }
        });
        WriteChunksParallel(obj, mesh.Indices.size() / 3, kChunk, 48, [&](size_t begin, size_t end, ObjText& text) {
            for (size_t t = begin; t < end; ++t)
            {
                const size_t i = t * 3;
                text.Face(uint64_t(mesh.Indices[i]) + 1, uint64_t(mesh.Indices[i + 1]) + 1,
                          uint64_t(mesh.Indices[i + 2]) + 1);
            }
        });
        obj.close();
        if (!WriteMtl(mtlPath, diffuseTexture))
        {
            error = "MTL を書き込めません";
            return false;
        }
        if (!obj)
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

