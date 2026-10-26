#ifndef RENDER_NOISE_HLSL
#define RENDER_NOISE_HLSL

// Include-only noise library. No shader entry point or resource bindings.
namespace TextureNoise
{
    static const uint BasisPerlin = 0, BasisValue = 1, BasisWorley = 2;
    static const uint FractalFBm = 0, FractalRidged = 1, FractalBillow = 2;
    static const uint FractalTurbulence = 3, FractalHybrid = 4, FractalDomainWarp = 5;
    static const uint FractalNone = 6;

    struct NoiseSettings
    {
        float Frequency;
        int Octaves;
        float Lacunarity;
        float Gain;
        float WarpStrength;
        float RidgeOffset;
        uint Tileable;
    };

    uint Hash(uint value)
    {
        value ^= value >> 16;
        value *= 0x7feb352d;
        value ^= value >> 15;
        value *= 0x846ca68b;
        return value ^ (value >> 16);
    }

    uint Hash3(int3 cell, uint seed)
    {
        return Hash((uint)cell.x * 0x8da6b343 ^
                    Hash((uint)cell.y * 0xd8163841 ^ Hash((uint)cell.z * 0xcb1ab31f ^ seed)));
    }

    float Unit(uint h)
    {
        return (h >> 8) / 16777216.0;
    }

    int Wrap(int value, int period)
    {
        if (period <= 0)
        {
            return value;
        }
        int remainder = value % period;
        return remainder < 0 ? remainder + period : remainder;
    }

    int3 WrapCell(int3 cell, int period)
    {
        return int3(Wrap(cell.x, period), Wrap(cell.y, period), Wrap(cell.z, period));
    }

    float Smooth(float a, float b, float value)
    {
        float t = saturate((value - a) / max(b - a, 1e-6));
        return t * t * (3 - 2 * t);
    }

    float GradientDot(uint h, float3 p)
    {
        switch (h % 12)
        {
            case 0:
                return p.x + p.y;
            case 1:
                return -p.x + p.y;
            case 2:
                return p.x - p.y;
            case 3:
                return -p.x - p.y;
            case 4:
                return p.x + p.z;
            case 5:
                return -p.x + p.z;
            case 6:
                return p.x - p.z;
            case 7:
                return -p.x - p.z;
            case 8:
                return p.y + p.z;
            case 9:
                return -p.y + p.z;
            case 10:
                return p.y - p.z;
            default:
                return -p.y - p.z;
        }
    }

    float2 Worley(float3 p, uint seed, int period)
    {
        int3 cell = (int3)floor(p);
        float2 distances = 1e9;
        [loop] for (int z = -1; z <= 1; ++z)
        {
            [loop] for (int y = -1; y <= 1; ++y)
            {
                [loop] for (int x = -1; x <= 1; ++x)
                {
                    int3 neighbor = cell + int3(x, y, z);
                    uint h = Hash3(WrapCell(neighbor, period), seed);
                    float3 featurePosition =
                        neighbor + float3(Unit(h), Unit(Hash(h ^ 0x68bc21eb)), Unit(Hash(h ^ 0x02e5be93)));
                    float distance = dot(featurePosition - p, featurePosition - p);
                    if (distance < distances.x)
                    {
                        distances.y = distances.x;
                        distances.x = distance;
                    }
                    else if (distance < distances.y)
                    {
                        distances.y = distance;
                    }
                }
            }
        }
        return sqrt(distances);
    }

    float BasisNoise(uint basis, float3 p, uint seed, int period)
    {
        if (basis >= BasisWorley)
        {
            float2 distances = Worley(p, seed, period);
            float value =
                basis == BasisWorley ? saturate(distances.x * 1.2) : saturate((distances.y - distances.x) * 2);
            return value * 2 - 1;
        }
        int3 cell = (int3)floor(p);
        float3 local = p - cell;
        float3 fade = local * local * local * (local * (local * 6 - 15) + 10);
        float corners[8];
        [unroll] for (int index = 0; index < 8; ++index)
        {
            int3 offset = int3(index & 1, (index >> 1) & 1, (index >> 2) & 1);
            uint h = Hash3(WrapCell(cell + offset, period), seed);
            corners[index] = basis == BasisValue ? Unit(h) * 2 - 1 : GradientDot(h, local - offset);
        }
        float a = lerp(lerp(corners[0], corners[1], fade.x), lerp(corners[2], corners[3], fade.x), fade.y);
        float b = lerp(lerp(corners[4], corners[5], fade.x), lerp(corners[6], corners[7], fade.x), fade.y);
        float value = lerp(a, b, fade.z);
        return basis == BasisValue ? value : clamp(value * 0.95, -1, 1);
    }

    float FractalSum(float3 p, uint basis, uint fractal, uint seed, bool tileable, NoiseSettings settings)
    {
        float frequency = max(settings.Frequency, 0.001);
        if (fractal == FractalNone)
        {
            int period = tileable ? max(1, (int)floor(frequency + 0.5)) : 0;
            float scale = tileable ? (float)period : frequency;
            float value = BasisNoise(basis, p * scale, Hash(seed), period);
            return saturate(value * 0.5 + 0.5);
        }
        // CPU round uses half-away-from-zero; tile frequencies are positive.
        float lacunarity = tileable ? max(1.0, floor(settings.Lacunarity + 0.5)) : settings.Lacunarity;
        float amplitude = 1;
        float sum = 0;
        float norm = 0;
        float weight = 1;
        [loop] for (int octave = 0; octave < clamp(settings.Octaves, 1, 12); ++octave)
        {
            int period = tileable ? max(1, (int)floor(frequency + 0.5)) : 0;
            float scale = tileable ? (float)period : frequency;
            float n = BasisNoise(basis, p * scale, Hash(seed + octave * 1013), period);
            switch (fractal)
            {
                case FractalRidged:
                    n = settings.RidgeOffset - abs(n);
                    n *= n * weight;
                    weight = saturate(n * 2);
                    sum += n * amplitude;
                    break;
                case FractalBillow:
                    sum += (abs(n) * 2 - 1) * amplitude;
                    break;
                case FractalTurbulence:
                    sum += abs(n) * amplitude;
                    break;
                case FractalHybrid: {
                    float signal = (n + settings.RidgeOffset * 0.7) * amplitude;
                    sum += octave == 0 ? signal : signal * min(weight, 1.0);
                    weight = min(weight * signal * 2, 1.0);
                    break;
                }
                default:
                    sum += n * amplitude;
                    break;
            }
            norm += amplitude;
            amplitude *= settings.Gain;
            frequency *= lacunarity;
        }
        float value = sum / max(norm, 1e-5);
        switch (fractal)
        {
            case FractalRidged:
                return saturate(value / max(settings.RidgeOffset * settings.RidgeOffset, 0.25));
            case FractalTurbulence:
                return saturate(value * 1.6);
            case FractalHybrid:
                return saturate(value * 0.8);
            default:
                return saturate(value * 0.8 + 0.5);
        }
    }

    float Noise(float3 p, uint basis, uint fractal, uint seed, NoiseSettings settings)
    {
        bool tileable = settings.Tileable != 0;
        if (fractal != FractalDomainWarp)
        {
            return FractalSum(p, basis, fractal, seed, tileable, settings);
        }
        float wx = FractalSum(p + float3(5.2, 1.3, 2.7), basis, FractalFBm, seed, tileable, settings) * 2 - 1;
        float wy =
            FractalSum(p + float3(1.7, 9.2, 4.1), basis, FractalFBm, seed ^ 0x9e3779b9, tileable, settings) * 2 - 1;
        float scale = settings.WarpStrength / max(settings.Frequency, 0.001);
        return FractalSum(p + float3(wx, wy, (wx - wy) * 0.5) * scale, basis, FractalFBm, seed, tileable, settings);
    }

}

// 2D gradient noise used by vertex deformation.
namespace DeformationNoise
{
    uint NoiseHash(int2 cell, uint seed)
    {
        uint value = uint(cell.x) * 0x9e3779b9u ^ uint(cell.y) * 0x85ebca6bu ^ seed;
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        return value ^ (value >> 16);
    }

    float Gradient(int2 cell, float2 delta, uint seed)
    {
        switch (NoiseHash(cell, seed) & 7u)
        {
            case 0:
                return delta.x;
            case 1:
                return -delta.x;
            case 2:
                return delta.y;
            case 3:
                return -delta.y;
            case 4:
                return (delta.x + delta.y) * 0.70710678118f;
            case 5:
                return (delta.x - delta.y) * 0.70710678118f;
            case 6:
                return (-delta.x + delta.y) * 0.70710678118f;
            default:
                return (-delta.x - delta.y) * 0.70710678118f;
        }
    }

    float Perlin(float2 position, float frequency, uint seed)
    {
        position = clamp(position * clamp(frequency, 0.0001f, 2.0f), -10000.0f, 10000.0f);
        int2 cell = int2(floor(position));
        float2 local = position - cell;
        float2 weight = local * local * local * (local * (local * 6.0f - 15.0f) + 10.0f);
        float low =
            lerp(Gradient(cell, local, seed), Gradient(cell + int2(1, 0), local - float2(1, 0), seed), weight.x);
        float high = lerp(Gradient(cell + int2(0, 1), local - float2(0, 1), seed),
                          Gradient(cell + int2(1, 1), local - 1.0f, seed), weight.x);
        return clamp(lerp(low, high, weight.y) * 1.41421356237f, -1.0f, 1.0f);
    }

}

// Periodic cloud volume noise.
// 格子ハッシュは整数座標をラップするため、テクスチャは周期的になる。
uint CloudHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

uint LatticeHash(int3 cell, int period, uint seed)
{
    // 負の格子セルを含め、全オクターブの周期は 2 のべき乗。
    uint3 wrapped = uint3(cell) & uint(period - 1);
    return CloudHash(wrapped.x * 1597334677u ^ wrapped.y * 3812015801u ^ wrapped.z * 2798796415u ^ seed);
}

float3 HashVector(uint value)
{
    return float3(CloudHash(value), CloudHash(value + 17u), CloudHash(value + 31u)) / 4294967296.0;
}

float PeriodicPerlin(float3 uv, int period, uint seed)
{
    float3 p = uv * period;
    int3 cell = int3(floor(p));
    float3 f = frac(p);
    float3 blend = f * f * f * (f * (f * 6 - 15) + 10);
    float result = 0;
    [unroll] for (int z = 0; z < 2; ++z)
    {
        [unroll] for (int y = 0; y < 2; ++y)
        {
            [unroll] for (int x = 0; x < 2; ++x)
            {
                int3 corner = int3(x, y, z);
                float3 gradient = HashVector(LatticeHash(cell + corner, period, seed)) * 2 - 1;
                gradient *= rsqrt(max(dot(gradient, gradient), 0.0001));
                float3 weight = lerp(1 - blend, blend, float3(corner));
                result += dot(gradient, f - corner) * weight.x * weight.y * weight.z;
            }
        }
    }
    return saturate(result * 0.75 + 0.5);
}

float PeriodicWorley(float3 uv, int period, uint seed)
{
    float3 p = uv * period;
    int3 cell = int3(floor(p));
    float nearest = 3;
    [loop] for (int z = -1; z <= 1; ++z)
    {
        [loop] for (int y = -1; y <= 1; ++y)
        {
            [loop] for (int x = -1; x <= 1; ++x)
            {
                int3 neighbor = cell + int3(x, y, z);
                float3 offset = neighbor + HashVector(LatticeHash(neighbor, period, seed)) - p;
                nearest = min(nearest, dot(offset, offset));
            }
        }
    }
    return saturate(sqrt(nearest));
}

float WorleyFbm(float3 uv, uint seed)
{
    return PeriodicWorley(uv, 4, seed) * 0.625 + PeriodicWorley(uv, 8, seed + 1) * 0.25 +
           PeriodicWorley(uv, 16, seed + 2) * 0.125;
}

float Hash13Common(float3 p)
{
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float ValueNoiseCommon(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);

    float n000 = Hash13Common(i + float3(0.0f, 0.0f, 0.0f));
    float n100 = Hash13Common(i + float3(1.0f, 0.0f, 0.0f));
    float n010 = Hash13Common(i + float3(0.0f, 1.0f, 0.0f));
    float n110 = Hash13Common(i + float3(1.0f, 1.0f, 0.0f));
    float n001 = Hash13Common(i + float3(0.0f, 0.0f, 1.0f));
    float n101 = Hash13Common(i + float3(1.0f, 0.0f, 1.0f));
    float n011 = Hash13Common(i + float3(0.0f, 1.0f, 1.0f));
    float n111 = Hash13Common(i + float3(1.0f, 1.0f, 1.0f));

    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);
    float nxy0 = lerp(nx00, nx10, f.y);
    float nxy1 = lerp(nx01, nx11, f.y);
    return lerp(nxy0, nxy1, f.z);
}

// Screen-space hashes for weather, lens effects and contact shadow sampling.
float Hash21(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float ContactShadowPixelNoise(float2 pixelPosition)
{
    return frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
}

float WeatherValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(Hash21(i), Hash21(i + float2(1, 0)), u.x),
                lerp(Hash21(i + float2(0, 1)), Hash21(i + float2(1, 1)), u.x), u.y);
}

float WeatherFbm(float2 p)
{
    return WeatherValueNoise(p) * 0.55f + WeatherValueNoise(p * 2.03f + 7.1f) * 0.3f +
           WeatherValueNoise(p * 4.11f + 3.7f) * 0.15f;
}

#endif
