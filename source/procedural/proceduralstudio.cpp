#include "pch.h"
#include "proceduralstudio.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <thread>

namespace ProcStudio
{
namespace
{
// Splits [0, count) over hardware threads. Generators are pure functions of their coordinates.
void ParallelFor(int count, const std::function<void(int)>& body)
{
    const int workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 32);
    if (count < 8 || workers == 1)
    {
        for (int i = 0; i < count; ++i)
        {
            body(i);
        }
        return;
    }
    std::vector<std::thread> threads;
    for (int w = 0; w < workers; ++w)
    {
        threads.emplace_back([&, w] {
            for (int i = w; i < count; i += workers)
            {
                body(i);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
}
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
    return Hash(static_cast<uint32_t>(x) * 0x8da6b343u ^ Hash(static_cast<uint32_t>(y) * 0xd8163841u ^
                                                               Hash(static_cast<uint32_t>(z) * 0xcb1ab31fu ^ seed)));
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
    // 12 cube-edge gradients (Perlin improved noise).
    switch (h % 12u)
    {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x + z;
    case 5: return -x + z;
    case 6: return x - z;
    case 7: return -x - z;
    case 8: return y + z;
    case 9: return -y + z;
    case 10: return y - z;
    default: return -y - z;
    }
}

float Perlin(float x, float y, float z, uint32_t seed, int period)
{
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y)),
              iz = static_cast<int>(std::floor(z));
    const float fx = x - ix, fy = y - iy, fz = z - iz;
    const float u = Fade(fx), v = Fade(fy), w = Fade(fz);
    auto corner = [&](int dx, int dy, int dz) {
        const uint32_t h = Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
        return GradientDot(h, fx - dx, fy - dy, fz - dz);
    };
    const float x00 = Lerp(corner(0, 0, 0), corner(1, 0, 0), u), x10 = Lerp(corner(0, 1, 0), corner(1, 1, 0), u);
    const float x01 = Lerp(corner(0, 0, 1), corner(1, 0, 1), u), x11 = Lerp(corner(0, 1, 1), corner(1, 1, 1), u);
    return std::clamp(Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w) * 0.95f, -1.0f, 1.0f);
}

float ValueNoise(float x, float y, float z, uint32_t seed, int period)
{
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y)),
              iz = static_cast<int>(std::floor(z));
    const float u = Fade(x - ix), v = Fade(y - iy), w = Fade(z - iz);
    auto corner = [&](int dx, int dy, int dz) {
        return Unit(Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed)) * 2 - 1;
    };
    const float x00 = Lerp(corner(0, 0, 0), corner(1, 0, 0), u), x10 = Lerp(corner(0, 1, 0), corner(1, 1, 0), u);
    const float x01 = Lerp(corner(0, 0, 1), corner(1, 0, 1), u), x11 = Lerp(corner(0, 1, 1), corner(1, 1, 1), u);
    return Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w);
}

void Worley(float x, float y, float z, uint32_t seed, int period, float& f1, float& f2)
{
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y)),
              iz = static_cast<int>(std::floor(z));
    f1 = f2 = 1e9f;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const uint32_t h = Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                const float px = ix + dx + Unit(h), py = iy + dy + Unit(Hash(h ^ 0x68bc21ebu)),
                            pz = iz + dz + Unit(Hash(h ^ 0x02e5be93u));
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
    f1 = std::sqrt(f1);
    f2 = std::sqrt(f2);
}
} // namespace

float BasisNoise(Basis basis, float x, float y, float z, uint32_t seed, int period)
{
    switch (basis)
    {
    case Basis::Value: return ValueNoise(x, y, z, seed, period);
    case Basis::Worley:
    case Basis::CellEdge:
    {
        float f1, f2;
        Worley(x, y, z, seed, period, f1, f2);
        // Map to [-1, 1] so fractal combiners treat all bases alike.
        const float value = basis == Basis::Worley ? Saturate(f1 * 1.2f) : Saturate((f2 - f1) * 2.0f);
        return value * 2 - 1;
    }
    default: return Perlin(x, y, z, seed, period);
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
        // Warp with two decorrelated fBm fields; offsets are whole periods so tiling survives.
        const float wx = FractalNoise(inner, x + 5.2f, y + 1.3f, z + 2.7f, tileable) * 2 - 1;
        NoiseParams second = inner;
        second.Seed = params.Seed ^ 0x9e3779b9u;
        const float wy = FractalNoise(second, x + 1.7f, y + 9.2f, z + 4.1f, tileable) * 2 - 1;
        const float s = params.WarpStrength / (std::max)(params.Frequency, 0.001f);
        return FractalNoise(inner, x + wx * s, y + wy * s, z + (wx - wy) * s * 0.5f, tileable);
    }
    float amplitude = 1, sum = 0, norm = 0, weight = 1;
    for (int o = 0; o < octaves; ++o)
    {
        const int period = tileable ? (std::max)(1, static_cast<int>(std::round(frequency))) : 0;
        const float f = tileable ? static_cast<float>(period) : frequency;
        const uint32_t seed = Hash(params.Seed + static_cast<uint32_t>(o) * 1013u);
        float n = BasisNoise(params.BasisType, x * f, y * f, z * f, seed, period);
        switch (params.Fractal)
        {
        case FractalType::Ridged:
        {
            n = params.RidgeOffset - std::fabs(n);
            n *= n * weight;
            weight = Saturate(n * 2);
            sum += n * amplitude;
            break;
        }
        case FractalType::Billow: sum += (std::fabs(n) * 2 - 1) * amplitude; break;
        case FractalType::Turbulence: sum += std::fabs(n) * amplitude; break;
        case FractalType::HybridMultifractal:
        {
            const float signal = (n + params.RidgeOffset * 0.7f) * amplitude;
            sum += o == 0 ? signal : signal * (std::min)(weight, 1.0f);
            weight = (std::min)(weight * signal * 2, 1.0f);
            break;
        }
        default: sum += n * amplitude; break;
        }
        norm += amplitude;
        amplitude *= params.Gain;
        frequency *= lacunarity;
    }
    float value = sum / (std::max)(norm, 1e-5f);
    switch (params.Fractal)
    {
    case FractalType::Ridged: return Saturate(value / (std::max)(params.RidgeOffset * params.RidgeOffset, 0.25f));
    case FractalType::Turbulence: return Saturate(value * 1.6f);
    case FractalType::HybridMultifractal: return Saturate(value * 0.8f);
    default: return Saturate(value * 0.8f + 0.5f);
    }
}

Float4 Image::Sample(float u, float v) const
{
    if (Width <= 0 || Height <= 0)
    {
        return {};
    }
    const float x = (u - std::floor(u)) * Width - 0.5f, y = (v - std::floor(v)) * Height - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float tx = x - x0, ty = y - y0;
    auto at = [&](int px, int py) { return Pixels[Wrap(py, Height) * Width + Wrap(px, Width)]; };
    const Float4 a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    auto mix = [&](float p, float q, float r, float s) { return Lerp(Lerp(p, q, tx), Lerp(r, s, tx), ty); };
    return {mix(a.X, b.X, c.X, d.X), mix(a.Y, b.Y, c.Y, d.Y), mix(a.Z, b.Z, c.Z, d.Z), mix(a.W, b.W, c.W, d.W)};
}

const char* TextureKindName(TextureKind kind)
{
    static const char* names[] = {"フラクタルノイズ", "大理石 (Marble)",     "木目 (Wood)",       "御影石 (Granite)",
                                  "ボロノイ",         "レンガ",              "チェッカー",        "フラクタル水面",
                                  "フラクタル雲",     "マンデルブロ集合",    "ジュリア集合"};
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
    double zx = julia ? (u - 0.5) * scale : 0, zy = julia ? (v - 0.5) * scale : 0;
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
    // Smooth iteration count.
    const double log = std::log(std::log(std::sqrt(zx * zx + zy * zy)) / std::log(2.0)) / std::log(2.0);
    return Saturate(static_cast<float>((i + 1 - log) / limit) * 4.0f);
}

float WaterHeight(const TextureParams& p, float x, float y, float z, bool tileable)
{
    // Choppy fBm: sharpened ridges travelling with time; integer drift keeps tiling.
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
} // namespace

float TextureScalar(const TextureParams& p, float x, float y, float z, bool solid)
{
    const bool tile = p.Tileable && !solid;
    float value = 0;
    switch (p.Kind)
    {
    case TextureKind::Marble:
    {
        const float n = FractalNoise(p.Noise, x, y, z, tile);
        // Integer stripe frequency keeps the pattern periodic when tiling.
        value = 0.5f + 0.5f * std::sin((x * std::round(p.RingCount) + n * p.Turbulence) * 2 * kPi);
        value = std::pow(value, 0.6f);
        break;
    }
    case TextureKind::Wood:
    {
        const float n = FractalNoise(p.Noise, x, y, z, tile);
        const float cx = x - 0.5f, cz = (solid ? z : y) - 0.5f;
        const float r = std::sqrt(cx * cx + cz * cz) * p.RingCount * 2 + n * p.Turbulence * 0.5f;
        value = r - std::floor(r);
        value = Smooth(0.0f, 0.8f, value) * (1 - Smooth(0.85f, 1.0f, value));
        break;
    }
    case TextureKind::Granite:
    {
        NoiseParams n = p.Noise;
        n.BasisType = Basis::Worley;
        n.Fractal = FractalType::FBm;
        const float cells = FractalNoise(n, x, y, z, tile);
        const float grain = FractalNoise(p.Noise, x * 4, y * 4, z * 4, tile);
        value = Saturate(cells * 0.6f + grain * 0.4f);
        value = value > 0.62f ? 1.0f : value < 0.3f ? 0.0f : value;
        break;
    }
    case TextureKind::Voronoi:
    {
        float f1, f2;
        const int period = tile ? (std::max)(1, static_cast<int>(std::round(p.Noise.Frequency))) : 0;
        const float f = tile ? static_cast<float>(period) : p.Noise.Frequency;
        Worley(x * f, y * f, z * f, p.Noise.Seed, period, f1, f2);
        value = Saturate((f2 - f1) * 3.0f);
        break;
    }
    case TextureKind::Brick:
    {
        const float bx = x * p.BricksX, by = y * p.BricksY;
        const int row = static_cast<int>(std::floor(by));
        const float sx = bx + (row & 1) * 0.5f;
        const float fx = sx - std::floor(sx), fy = by - std::floor(by);
        const float mx = p.Mortar * p.BricksX * 0.5f, my = p.Mortar * p.BricksY * 0.5f;
        const float edge = (std::min)(Smooth(0, mx, (std::min)(fx, 1 - fx)), Smooth(0, my, (std::min)(fy, 1 - fy)));
        const float variation = Unit(Hash3(Wrap(static_cast<int>(std::floor(sx)), (std::max)(p.BricksX, 1)),
                                          Wrap(row, (std::max)(p.BricksY, 1)), 0, p.Noise.Seed));
        value = edge * (0.6f + 0.4f * variation) * (0.85f + 0.15f * FractalNoise(p.Noise, x, y, z, tile));
        break;
    }
    case TextureKind::Checker:
    {
        const int cx = static_cast<int>(std::floor(x * p.BricksX)), cy = static_cast<int>(std::floor(y * p.BricksX));
        value = ((cx + cy) & 1) ? 1.0f : 0.0f;
        break;
    }
    case TextureKind::FractalWater: value = WaterHeight(p, x, y, z, tile); break;
    case TextureKind::FractalCloud:
    {
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
    case TextureKind::Mandelbrot: value = Escape(p, x, y, false); break;
    case TextureKind::Julia: value = Escape(p, x, y, true); break;
    default: value = FractalNoise(p.Noise, x, y, z, tile); break;
    }
    return Saturate((value - 0.5f) * p.Contrast + 0.5f + p.Bias);
}

Float3 TextureColor(const TextureParams& p, float x, float y, float z, bool solid)
{
    const float v = TextureScalar(p, x, y, z, solid);
    switch (p.Kind)
    {
    case TextureKind::Brick:
        // Mortar is ColorB, brick ColorC with per-brick shading.
        return Mix3(ToF3(p.ColorB), Mul(ToF3(p.ColorC), 0.6f + 0.6f * v), Saturate(v * 4));
    case TextureKind::FractalWater:
    {
        // Fixed ocean palette (the generic ramp colours are tuned for stone / noise).
        const Float3 deep{0.01f, 0.07f, 0.13f}, shallow{0.04f, 0.32f, 0.42f}, foam{0.92f, 0.95f, 0.98f};
        const Float3 water = Mix3(deep, shallow, Smooth(0.2f, 0.75f, v));
        return Mix3(water, foam, Smooth(0.72f, 0.92f, v) * p.Choppiness);
    }
    case TextureKind::FractalCloud:
    {
        const Float3 sky{0.32f, 0.52f, 0.86f};
        return Mix3(sky, Mix(p.ColorB, std::array<float, 3>{1, 1, 1}, 0.5f), v);
    }
    case TextureKind::Mandelbrot:
    case TextureKind::Julia:
    {
        if (v <= 0)
        {
            return {0, 0, 0};
        }
        const float t = std::sqrt(v);
        const float glow = Saturate(t * 1.6f);
        return Mul(Float3{0.5f + 0.5f * std::cos(2 * kPi * (t + 0.0f)), 0.5f + 0.5f * std::cos(2 * kPi * (t + 0.33f)),
                      0.5f + 0.5f * std::cos(2 * kPi * (t + 0.67f))}, glow);
    }
    default:
        // Three-stop ramp A -> B -> C.
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
            const float u = (x + 0.5f) / image.Width, v = (y + 0.5f) / image.Height;
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
        // Tangent-space normal map (OpenGL / +Y convention) from wrapped central differences.
        for (int y = 0; y < image.Height; ++y)
        {
            for (int x = 0; x < image.Width; ++x)
            {
                auto h = [&](int px, int py) { return heights[Wrap(py, image.Height) * image.Width + Wrap(px, image.Width)]; };
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
            for (size_t x = 0; x < n; ++x)
            {
                const float fx = (x + 0.5f) / n, fy = (y + 0.5f) / n, fz = (z + 0.5f) / n;
                // Tileable in all three axes: evaluate the 2D scalar with the 3rd coordinate as a lattice axis.
                volume.Density[(z * n + y) * n + x] = TextureScalar(params, fx, fy, fz, false);
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
                const float fx = (x + 0.5f) / n, fy = (y + 0.5f) / n, fz = (z + 0.5f) / n;
                float sum = 0, norm = 0, amplitude = 1;
                for (int o = 0; o < octaves; ++o)
                {
                    const float scale = static_cast<float>(1 << o);
                    // Shear the horizontal lookup with height; integer offsets keep the volume periodic.
                    const uint32_t h = Hash(seed + static_cast<uint32_t>(o) * 977u);
                    const float angle = fy * 2 * kPi;
                    const float ox = std::round(verticalVariation * 2) * std::cos(angle) * 0.5f / scale + Unit(h);
                    const float oz = std::round(verticalVariation * 2) * std::sin(angle) * 0.5f / scale + Unit(Hash(h));
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
    const size_t n = static_cast<size_t>(std::clamp(extent, 8, 256));
    std::vector<Float4> voxels(n * n * n);
    ParallelFor(static_cast<int>(n), [&](int zi) {
        const size_t z = static_cast<size_t>(zi);
        for (size_t y = 0; y < n; ++y)
            for (size_t x = 0; x < n; ++x)
            {
                const Float3 c = TextureColor(p, (x + 0.5f) / n, (y + 0.5f) / n, (z + 0.5f) / n, true);
                voxels[(z * n + y) * n + x] = {c.X, c.Y, c.Z, 1};
            }
    });
    return voxels;
}

// ------------------------------------------------------------------------------------------------
// Meshes
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
    for (size_t i = 0; i + 2 < Indices.size(); i += 3)
    {
        const uint32_t a = Indices[i], b = Indices[i + 1], c = Indices[i + 2];
        const Float3 n = Cross(Sub(Positions[b], Positions[a]), Sub(Positions[c], Positions[a])); // area weighted
        Normals[a] = Add(Normals[a], n);
        Normals[b] = Add(Normals[b], n);
        Normals[c] = Add(Normals[c], n);
    }
    for (auto& n : Normals)
    {
        n = Normalize(n);
    }
}

void Mesh::Bounds(Float3& minimum, Float3& maximum) const
{
    minimum = {1e30f, 1e30f, 1e30f};
    maximum = {-1e30f, -1e30f, -1e30f};
    for (const auto& p : Positions)
    {
        minimum = {(std::min)(minimum.X, p.X), (std::min)(minimum.Y, p.Y), (std::min)(minimum.Z, p.Z)};
        maximum = {(std::max)(maximum.X, p.X), (std::max)(maximum.Y, p.Y), (std::max)(maximum.Z, p.Z)};
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
        const float v = static_cast<float>(r) / rings, phi = v * kPi;
        for (int s = 0; s <= segments; ++s)
        {
            const float u = static_cast<float>(s) / segments, theta = u * 2 * kPi;
            const Float3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            mesh.AddVertex(Mul(n, radius), kWhite, {u, v});
        }
    }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s)
        {
            const uint32_t a = r * (segments + 1) + s, b = a + segments + 1;
            mesh.Indices.insert(mesh.Indices.end(), {a, a + 1, b, a + 1, b + 1, b});
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
        const float su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
        for (int i = 0; i < 4; ++i)
        {
            const Float3 p = Add(base, Add(Mul(right, su[i] * half), Mul(up, sv[i] * half)));
            const Float2 uv = uvPerFace ? Float2{su[i] * 0.5f + 0.5f, 0.5f - sv[i] * 0.5f}
                                        : Float2{p.X + p.Z, p.Y + p.Z};
            v[i] = mesh.AddVertex(p, color, uv);
        }
        // Counter-clockwise seen from outside (right-handed OBJ convention, outward normals).
        AddQuad(mesh, v[0], v[1], v[2], v[3]);
    }
}

Float3 TerrainColor(float h, float slope)
{
    const Float3 sand{0.76f, 0.70f, 0.50f}, grass{0.22f, 0.45f, 0.16f}, rock{0.42f, 0.38f, 0.34f},
        snow{0.95f, 0.96f, 0.98f};
    Float3 c = Mix3(sand, grass, Smooth(0.15f, 0.25f, h));
    c = Mix3(c, rock, Smooth(0.55f, 0.7f, h));
    c = Mix3(c, snow, Smooth(0.78f, 0.88f, h));
    return Mix3(c, rock, Smooth(0.55f, 0.8f, slope) * (1 - Smooth(0.85f, 0.95f, h)));
}

void BuildGrid(Mesh& mesh, int resolution, float size, const std::function<float(float, float)>& height,
               const std::function<Float3(float, float, float)>& color)
{
    resolution = std::clamp(resolution, 2, 512);
    for (int z = 0; z <= resolution; ++z)
        for (int x = 0; x <= resolution; ++x)
        {
            const float u = static_cast<float>(x) / resolution, v = static_cast<float>(z) / resolution;
            const float h = height(u, v);
            mesh.AddVertex({(u - 0.5f) * size, h, (v - 0.5f) * size}, color(u, v, h), {u, v});
        }
    for (int z = 0; z < resolution; ++z)
        for (int x = 0; x < resolution; ++x)
        {
            const uint32_t a = z * (resolution + 1) + x, b = a + resolution + 1;
            mesh.Indices.insert(mesh.Indices.end(), {a, b, a + 1, a + 1, b, b + 1});
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

Float3 Rotate(Float3 v, Float3 axis, float angle)
{
    // Rodrigues rotation.
    const float c = std::cos(angle), s = std::sin(angle);
    return Add(Add(Mul(v, c), Mul(Cross(axis, v), s)), Mul(axis, Dot(axis, v) * (1 - c)));
}

void AddBranch(Mesh& mesh, Float3 a, Float3 b, float ra, float rb, int segments, Float3 color, float v0, float v1)
{
    const Float3 axis = Normalize(Sub(b, a));
    const Float3 helper = std::fabs(axis.Y) < 0.95f ? Float3{0, 1, 0} : Float3{1, 0, 0};
    const Float3 x = Normalize(Cross(helper, axis)), y = Cross(axis, x);
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
        // Outward (counter-clockwise from outside) winding.
        mesh.Indices.insert(mesh.Indices.end(), {p, p + 2, p + 1, p + 2, p + 3, p + 1});
    }
}

// ------------------------------------------------------------------------------------------------
// Fractal leaves. A leaf is a rachis (midrib) carrying children that are scaled copies of the whole
// leaf (self-similar) until the recursion depth is exhausted; the last level is a blade.
struct LeafBuilder
{
    const LeafParams& P;
    Random Rng;
    Mesh& Out;

    // Blade in the leaf plane (z = 0 before droop/fold): base, unit direction, length, half width.
    void Blade(Float2 base, Float2 dir, float length, float halfWidth, float tint)
    {
        const int segments = std::clamp(P.Segments, 2, 16);
        const Float2 side{-dir.Y, dir.X};
        const Float3 baseColor{P.Color[0], P.Color[1], P.Color[2]};
        const Float3 tipColor{P.TipColor[0], P.TipColor[1], P.TipColor[2]};
        // Three columns (left edge, midrib, right edge) x (segments + 1) rows, front and back.
        std::vector<Float3> positions;
        std::vector<Float3> colors;
        std::vector<Float2> uvs;
        for (int row = 0; row <= segments; ++row)
        {
            const float t = static_cast<float>(row) / segments;
            // Lanceolate outline: widest at ~35 %, pointed tip, narrow petiole.
            // sin(pi) is slightly negative in float; pow of a negative base is NaN (broke the tip row).
            const float outline = std::pow((std::max)(std::sin(kPi * std::pow(t, 0.75f)), 0.0f), 0.9f);
            const float w = halfWidth * outline;
            for (int column = -1; column <= 1; ++column)
            {
                const Float2 p{base.X + dir.X * length * t + side.X * w * column,
                               base.Y + dir.Y * length * t + side.Y * w * column};
                positions.push_back({p.X, p.Y, 0});
                const Float3 c = Mix3(baseColor, tipColor, t * 0.8f);
                colors.push_back(Mul(c, tint * (column == 0 ? 0.85f : 1.0f))); // darker midrib
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
                    const uint32_t a = first + row * 3 + column, b = a + 1, c = a + 3, d = a + 4;
                    // Front face +Z, back face -Z (separate vertices so normals do not cancel).
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

    // Thin strip for the rachis of compound leaves.
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
            // corners run right-handed around +Z when side is to the left of dir.
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
        const float c = std::cos(angle), s = std::sin(angle);
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
            // Lobes fan out from the base; the central lobe continues the leaf.
            const int lobes = count | 1; // odd, symmetric
            for (int i = 0; i < lobes; ++i)
            {
                const float u = lobes == 1 ? 0.0f : (static_cast<float>(i) / (lobes - 1)) * 2 - 1; // -1..1
                // Side lobes are shorter; deeper levels shrink so sub-lobes stay inside the parent outline.
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
            // Pinnae start a short way up the rachis; fern pinnae alternate sides.
            const float t = 0.12f + 0.78f * (static_cast<float>(i) + 0.5f) / pairs;
            const float scale = P.LeafletScale * (1 - P.Taper * t) * (fern ? 1.2f : 1.0f);
            const Float2 at{base.X + dir.X * length * t, base.Y + dir.Y * length * t};
            const float childLength = length * scale;
            for (int sideSign = -1; sideSign <= 1; sideSign += 2)
            {
                if (fern && ((i & 1) ? sideSign < 0 : sideSign > 0) && depth == 1)
                {
                    // Alternate leaflets at the finest level keep the frond from looking mirrored.
                    const Float2 offset{at.X + dir.X * length * 0.5f / pairs, at.Y + dir.Y * length * 0.5f / pairs};
                    Grow(offset, Turn(dir, sideSign * angle), childLength, depth - 1);
                    continue;
                }
                Grow(at, Turn(dir, sideSign * angle + Rng.Signed() * 0.04f), childLength, depth - 1);
            }
        }
        // Terminal leaflet.
        const float terminal = length * (fern ? 0.9f : 1.0f) * P.LeafletScale;
        const Float2 tip{base.X + dir.X * (length - terminal * 0.2f), base.Y + dir.Y * (length - terminal * 0.2f)};
        Grow(tip, dir, terminal, depth - 1);
    }
};
} // namespace

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
        // Rectangle card: every row spans the full width (vertex order is row-major, 3 columns).
        for (size_t i = 0; i < mesh.Positions.size(); ++i)
        {
            const int column = static_cast<int>(i % 3); // 0 = +X edge, 1 = midrib, 2 = -X edge
            mesh.Positions[i].X = (1 - column) * 0.3f;
            mesh.UVs[i].X = 0.5f - (1 - column) * 0.5f;
        }
    }
    else
    {
        builder.Grow({0, 0}, {0, 1}, 1.0f, std::clamp(params.Depth, 0, 4));
    }
    // Normalize the whole leaf to unit length and apply droop / fold in 3D.
    Float3 minimum, maximum;
    mesh.Bounds(minimum, maximum);
    const float extent = (std::max)(maximum.Y - minimum.Y, 1e-4f);
    for (auto& p : mesh.Positions)
    {
        p = Mul(p, 1.0f / extent);
        const float along = Saturate(p.Y);
        p.Z -= params.Droop * along * along;          // tip bends down / back
        p.Z += params.Fold * std::fabs(p.X);          // V-shaped cross section
    }
    mesh.ComputeNormals();
    return mesh;
}

namespace
{
struct LeafPlacement
{
    Float3 Origin, Forward, Normal;
    float Tint;
};

void AppendLeaf(Mesh& mesh, const Mesh& leaf, const LeafPlacement& placement, float size)
{
    const Float3 forward = Normalize(placement.Forward);
    Float3 side = Cross(placement.Normal, forward);
    if (Dot(side, side) < 1e-8f)
    {
        side = Cross(std::fabs(forward.Y) < 0.95f ? Float3{0, 1, 0} : Float3{1, 0, 0}, forward);
    }
    side = Normalize(side);
    const Float3 normal = Cross(forward, side); // right-handed: (side, forward, normal) ~ (X, Y, Z)
    const uint32_t first = static_cast<uint32_t>(mesh.Positions.size());
    for (size_t i = 0; i < leaf.Positions.size(); ++i)
    {
        const Float3& p = leaf.Positions[i];
        const Float3 world =
            Add(placement.Origin, Mul(Add(Add(Mul(side, p.X), Mul(forward, p.Y)), Mul(normal, p.Z)), size));
        mesh.AddVertex(world, Mul(leaf.Colors[i], placement.Tint), leaf.UVs[i]);
    }
    for (uint32_t index : leaf.Indices)
    {
        mesh.Indices.push_back(first + index);
    }
}

void GrowTree(Mesh& mesh, const ModelParams& p, Random& random, Float3 origin, Float3 direction, float length,
              float radius, int depth, std::vector<LeafPlacement>& leaves)
{
    // Tropism bends towards +Y (light); jitter from the seeded RNG keeps the result deterministic.
    direction = Normalize(Add(direction, Float3{random.Signed() * p.Jitter * 0.3f, p.Tropism,
                                                random.Signed() * p.Jitter * 0.3f}));
    const Float3 end = Add(origin, Mul(direction, length));
    const float endRadius = radius * (depth <= 1 ? 0.35f : p.RadiusRatio + (1 - p.RadiusRatio) * 0.25f);
    const float bark = 0.6f + 0.4f * random.Next();
    AddBranch(mesh, origin, end, radius, endRadius, (std::max)(3, p.RadialSegments - (p.Depth - depth)),
              {0.36f * bark, 0.25f * bark, 0.16f * bark}, 0, length);
    if (depth <= 1)
    {
        if (p.Leaves)
        {
            // Placements are collected first so the leaf detail can be fitted to the triangle budget.
            for (int i = 0; i < std::clamp(p.LeavesPerTip, 1, 12); ++i)
            {
                const Float3 jitter{random.Signed(), random.Signed() * 0.5f, random.Signed()};
                const Float3 forward = Normalize(Add(direction, Mul(jitter, 1.2f)));
                // Leaf faces roughly upward (towards light), twisted randomly around its axis.
                const Float3 normal = Normalize(Add(Float3{0, 1, 0}, Mul(Float3{random.Signed(), 0, random.Signed()}, 0.6f)));
                leaves.push_back({Add(end, Mul(jitter, p.LeafSize * 0.3f)), forward, normal,
                                  0.75f + 0.25f * random.Next()});
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
    // Apical continuation keeps a dominant leader.
    if (depth > 2)
    {
        GrowTree(mesh, p, random, end, direction, length * p.LengthRatio * 0.9f, endRadius * 0.95f, depth - 2, leaves);
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
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                if ((x == 0) + (y == 0) + (z == 0) >= 2)
                {
                    continue;
                }
                const Float3 tint{0.5f + 0.25f * (x + 1) * 0.5f, 0.55f + 0.2f * (y + 1) * 0.5f,
                                  0.7f + 0.15f * (z + 1) * 0.5f};
                Menger(mesh, Add(center, Float3{x * child * 2, y * child * 2, z * child * 2}), child, level - 1,
                       level == 1 ? Float3{color.X * tint.X * 1.3f, color.Y * tint.Y * 1.3f, color.Z * tint.Z * 1.3f}
                                  : color);
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
            // Orient each facet away from the tetrahedron centre.
            const Float3 normal = Cross(Sub(face[1], face[0]), Sub(face[2], face[0]));
            if (Dot(normal, Sub(face[0], centroid)) < 0)
            {
                std::swap(face[1], face[2]);
            }
            // Unshared vertices give flat facets.
            const uint32_t i0 = mesh.AddVertex(face[0], color), i1 = mesh.AddVertex(face[1], color),
                           i2 = mesh.AddVertex(face[2], color);
            mesh.Indices.insert(mesh.Indices.end(), {i0, i1, i2});
        }
        return;
    }
    auto mid = [](Float3 p, Float3 q) { return Mul(Add(p, q), 0.5f); };
    const Float3 ab = mid(a, b), ac = mid(a, c), ad = mid(a, d), bc = mid(b, c), bd = mid(b, d), cd = mid(c, d);
    const Float3 warm{color.X * 1.05f, color.Y * 0.95f, color.Z * 0.9f};
    Sierpinski(mesh, a, ab, ac, ad, level - 1, color);
    Sierpinski(mesh, ab, b, bc, bd, level - 1, warm);
    Sierpinski(mesh, ac, bc, c, cd, level - 1, color);
    Sierpinski(mesh, ad, bd, cd, d, level - 1, warm);
}

void Icosphere(Mesh& mesh, int subdivisions)
{
    const float t = (1 + std::sqrt(5.0f)) / 2;
    std::vector<Float3> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                             {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    std::vector<uint32_t> f = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6,
                               7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6,
                               7, 9, 8, 1};
    for (auto& p : v)
    {
        p = Normalize(p);
    }
    for (int s = 0; s < std::clamp(subdivisions, 0, 6); ++s)
    {
        std::vector<uint32_t> next;
        // Edge midpoints bucketed by the smaller vertex index.
        std::vector<std::vector<std::pair<uint32_t, uint32_t>>> buckets(v.size());
        auto fast = [&](uint32_t a, uint32_t b) {
            const uint32_t lo = (std::min)(a, b), hi = (std::max)(a, b);
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
            const uint32_t a = fast(f[i], f[i + 1]), b = fast(f[i + 1], f[i + 2]), c = fast(f[i + 2], f[i]);
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
} // namespace

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
        BuildGrid(mesh, 1, 2.5f, [](float, float) { return 0.0f; }, [](float, float, float) { return kWhite; });
        break;
    case ModelKind::Terrain:
    {
        NoiseParams n = p.Noise;
        n.Seed ^= p.Seed;
        BuildGrid(
            mesh, p.Resolution, p.Size,
            [&](float u, float v) { return FractalNoise(n, u, v, 0.37f, false) * p.Height; },
            [&](float, float, float h) { return p.TerrainColors ? TerrainColor(h / (std::max)(p.Height, 1e-3f), 0) : kWhite; });
        if (p.TerrainColors)
        {
            for (size_t i = 0; i < mesh.Positions.size(); ++i)
            {
                const float slope = 1 - std::fabs(mesh.Normals[i].Y);
                mesh.Colors[i] = TerrainColor(mesh.Positions[i].Y / (std::max)(p.Height, 1e-3f), slope * 2.5f);
            }
        }
        break;
    }
    case ModelKind::FractalTree:
    {
        Random random{Hash(p.Seed)};
        std::vector<LeafPlacement> leaves;
        GrowTree(mesh, p, random, {0, 0, 0}, {0, 1, 0}, p.TrunkLength, p.TrunkRadius, std::clamp(p.Depth, 1, 9),
                 leaves);
        mesh.ComputeNormals();
        if (!leaves.empty())
        {
            // One leaf template, instanced at every placement. Reduce recursion until the budget fits.
            LeafParams leafParams = p.Leaf;
            Mesh leaf = GenerateLeaf(leafParams, p.Seed);
            // Coarsen the blades first (keeps the fractal structure), then drop recursion levels.
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
            // Leaf normals are rigidly transformed from the template (no full recompute).
            mesh.Normals.resize(barkVertices);
            for (const auto& placement : leaves)
            {
                const Float3 forward = Normalize(placement.Forward);
                Float3 side = Cross(placement.Normal, forward);
                if (Dot(side, side) < 1e-8f)
                {
                    side = Cross(std::fabs(forward.Y) < 0.95f ? Float3{0, 1, 0} : Float3{1, 0, 0}, forward);
                }
                side = Normalize(side);
                const Float3 normal = Cross(forward, side);
                for (const auto& n : leaf.Normals)
                {
                    mesh.Normals.push_back(Normalize(Add(Add(Mul(side, n.X), Mul(forward, n.Y)), Mul(normal, n.Z))));
                }
            }
        }
        break;
    }
    case ModelKind::FractalLeaf:
        mesh = GenerateLeaf(p.Leaf, p.Seed);
        // Lay the standalone leaf flat (front face up, tip towards -Z); proper rotation keeps winding.
        for (auto* list : {&mesh.Positions, &mesh.Normals})
        {
            for (auto& v : *list)
            {
                v = {v.X, v.Z, -v.Y};
            }
        }
        break;
    case ModelKind::Rock:
    {
        Icosphere(mesh, p.Subdivisions);
        NoiseParams n = p.Noise;
        n.Seed ^= p.Seed;
        for (auto& position : mesh.Positions)
        {
            const float d = FractalNoise(n, position.X * 0.5f + 7, position.Y * 0.5f + 3, position.Z * 0.5f + 5, false);
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
    case ModelKind::WaterSurface:
    {
        TextureParams water;
        water.Kind = TextureKind::FractalWater;
        water.Noise = p.Noise;
        water.Noise.Seed ^= p.Seed;
        water.Time = p.Time;
        water.Tileable = false;
        BuildGrid(
            mesh, p.Resolution, p.Size,
            [&](float u, float v) { return (TextureScalar(water, u, v, 0, false) - 0.5f) * 2 * p.WaveAmplitude; },
            [&](float, float, float h) {
                const float t = Saturate(h / (std::max)(p.WaveAmplitude, 1e-3f) * 0.5f + 0.5f);
                return Mix3(Float3{0.02f, 0.12f, 0.2f}, Float3{0.1f, 0.4f, 0.5f}, t);
            });
        break;
    }
    case ModelKind::MengerSponge: Menger(mesh, {0, 0, 0}, 1.0f, std::clamp(p.Level, 0, 3), {0.8f, 0.78f, 0.72f}); break;
    case ModelKind::SierpinskiPyramid:
    {
        const float s = 1.2f;
        Sierpinski(mesh, {-s, -s * 0.7f, -s * 0.58f}, {s, -s * 0.7f, -s * 0.58f}, {0, -s * 0.7f, s * 1.15f},
                   {0, s * 1.0f, 0}, std::clamp(p.Level, 0, 7), {0.85f, 0.65f, 0.3f});
        mesh.ComputeNormals();
        break;
    }
    default: break;
    }
    if (p.Kind == ModelKind::MengerSponge)
    {
        mesh.ComputeNormals();
    }
    return mesh;
}

bool WriteObj(const std::filesystem::path& path, const Mesh& mesh, bool vertexColors, const std::string& diffuseTexture,
              std::string& error)
{
    std::ofstream obj(path, std::ios::binary | std::ios::trunc);
    if (!obj)
    {
        error = "OBJ を書き込めません";
        return false;
    }
    auto mtlPath = path;
    mtlPath.replace_extension(".mtl");
    // First line is the marker the editor uses to place these models unconverted at scale 1.
    // u8string: string() throws / mangles non-ASCII (Japanese) file names on Windows.
    const auto mtlName = mtlPath.filename().u8string();
    obj << kProceduralObjMarker << "\nmtllib " << std::string(mtlName.begin(), mtlName.end()) << "\nusemtl procedural\n";
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
} // namespace ProcStudio


namespace ProcStudio
{
bool IsProceduralObj(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::string line;
    return std::getline(input, line) && line.rfind("# Procedural Studio", 0) == 0;
}
} // namespace ProcStudio
