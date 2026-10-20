struct NoiseParams
{
    uint BasisType;
    uint Fractal;
    uint Seed;
    float Frequency;
    int Octaves;
    float Lacunarity;
    float Gain;
    float WarpStrength;
    float RidgeOffset;
};

struct TextureParams
{
    uint Kind;
    uint Output;
    NoiseParams Noise;
    float3 ColorA;
    float3 ColorB;
    float3 ColorC;
    bool Tileable;
    float Contrast;
    float Bias;
    float Turbulence;
    float RingCount;
    int BricksX;
    int BricksY;
    float Mortar;
    float Time;
    float Choppiness;
    float Coverage;
    float Softness;
    float CenterX;
    float CenterY;
    float Zoom;
    float JuliaX;
    float JuliaY;
    int Iterations;
    float NormalStrength;
};

static const float kPi = 3.14159265358979323846f;
ByteAddressBuffer Parameters : register(t0);
ByteAddressBuffer SourcePixels : register(t1);
RWByteAddressBuffer Output : register(u0);

TextureParams LoadParameters()
{
    TextureParams p;
    p.Kind = Parameters.Load(16);
    p.Output = Parameters.Load(20);
    p.Noise.BasisType = Parameters.Load(24);
    p.Noise.Fractal = Parameters.Load(28);
    p.Noise.Seed = Parameters.Load(32);
    p.Noise.Frequency = asfloat(Parameters.Load(36));
    p.Noise.Octaves = Parameters.Load(40);
    p.Noise.Lacunarity = asfloat(Parameters.Load(44));
    p.Noise.Gain = asfloat(Parameters.Load(48));
    p.Noise.WarpStrength = asfloat(Parameters.Load(52));
    p.Noise.RidgeOffset = asfloat(Parameters.Load(56));
    p.ColorA = asfloat(Parameters.Load3(60));
    p.ColorB = asfloat(Parameters.Load3(72));
    p.ColorC = asfloat(Parameters.Load3(84));
    p.Tileable = Parameters.Load(96);
    p.Contrast = asfloat(Parameters.Load(100));
    p.Bias = asfloat(Parameters.Load(104));
    p.Turbulence = asfloat(Parameters.Load(108));
    p.RingCount = asfloat(Parameters.Load(112));
    p.BricksX = Parameters.Load(116);
    p.BricksY = Parameters.Load(120);
    p.Mortar = asfloat(Parameters.Load(124));
    p.Time = asfloat(Parameters.Load(128));
    p.Choppiness = asfloat(Parameters.Load(132));
    p.Coverage = asfloat(Parameters.Load(136));
    p.Softness = asfloat(Parameters.Load(140));
    p.CenterX = asfloat(Parameters.Load(144));
    p.CenterY = asfloat(Parameters.Load(148));
    p.Zoom = asfloat(Parameters.Load(152));
    p.JuliaX = asfloat(Parameters.Load(156));
    p.JuliaY = asfloat(Parameters.Load(160));
    p.Iterations = Parameters.Load(164);
    p.NormalStrength = asfloat(Parameters.Load(168));
    return p;
}

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

uint Hash3(int x, int y, int z, uint seed)
{
    return Hash(uint(x) * 0x8da6b343u ^ Hash(uint(y) * 0xd8163841u ^ Hash(uint(z) * 0xcb1ab31fu ^ seed)));
}

float Unit(uint h)
{
    return float(h >> 8) / 16777216.0f;
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
    return clamp(v, 0.0f, 1.0f);
}

float Smooth(float a, float b, float v)
{
    const float t = Saturate((v - a) / max(b - a, 1e-6f));
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

float GradientDot(uint h, float x, float y, float z)
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

float Perlin(float x, float y, float z, uint seed, int period)
{
    const int ix = int(floor(x));
    const int iy = int(floor(y));
    const int iz = int(floor(z));
    const float fx = x - ix;
    const float fy = y - iy;
    const float fz = z - iz;
    const float u = Fade(fx);
    const float v = Fade(fy);
    const float w = Fade(fz);
    float corners[8];
    for (int dz = 0; dz < 2; ++dz)
    {
        for (int dy = 0; dy < 2; ++dy)
        {
            for (int dx = 0; dx < 2; ++dx)
            {
                uint h = Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                corners[dz * 4 + dy * 2 + dx] = GradientDot(h, fx - dx, fy - dy, fz - dz);
            }
        }
    }
    float x00 = Lerp(corners[0], corners[1], u);
    float x10 = Lerp(corners[2], corners[3], u);
    float x01 = Lerp(corners[4], corners[5], u);
    float x11 = Lerp(corners[6], corners[7], u);
    return clamp(Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w) * 0.95f, -1.0f, 1.0f);
}

float ValueNoise(float x, float y, float z, uint seed, int period)
{
    const int ix = int(floor(x));
    const int iy = int(floor(y));
    const int iz = int(floor(z));
    const float u = Fade(x - ix);
    const float v = Fade(y - iy);
    const float w = Fade(z - iz);
    float corners[8];
    for (int dz = 0; dz < 2; ++dz)
    {
        for (int dy = 0; dy < 2; ++dy)
        {
            for (int dx = 0; dx < 2; ++dx)
            {
                uint h = Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                corners[dz * 4 + dy * 2 + dx] = Unit(h) * 2 - 1;
            }
        }
    }
    float x00 = Lerp(corners[0], corners[1], u);
    float x10 = Lerp(corners[2], corners[3], u);
    float x01 = Lerp(corners[4], corners[5], u);
    float x11 = Lerp(corners[6], corners[7], u);
    return Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w);
}

void Worley(float x, float y, float z, uint seed, int period, out float f1, out float f2)
{
    const int ix = int(floor(x));
    const int iy = int(floor(y));
    const int iz = int(floor(z));
    f1 = f2 = 1e9f;
    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                const uint h = Hash3(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
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
    f1 = sqrt(f1);
    f2 = sqrt(f2);
}

float BasisNoise(uint basis, float x, float y, float z, uint seed, int period)
{
    switch (basis)
    {
        case 1:
            return ValueNoise(x, y, z, seed, period);
        case 2:
        case 3: {
            float f1;
            float f2;
            Worley(x, y, z, seed, period, f1, f2);
            // 各ノイズ基底をフラクタル合成で同じように扱えるよう、[-1, 1] に変換する。
            const float value = basis == 2 ? Saturate(f1 * 1.2f) : Saturate((f2 - f1) * 2.0f);
            return value * 2 - 1;
        }
        default:
            return Perlin(x, y, z, seed, period);
    }
}

float FractalSum(NoiseParams params, float x, float y, float z, bool tileable)
{
    const int octaves = clamp(params.Octaves, 1, 12);
    float frequency = max(params.Frequency, 0.001f);
    const float lacunarity = tileable ? max(1.0f, round(params.Lacunarity)) : params.Lacunarity;
    float amplitude = 1;
    float sum = 0;
    float norm = 0;
    float weight = 1;
    for (int o = 0; o < octaves; ++o)
    {
        const int period = tileable ? max(1, int(round(frequency))) : 0;
        const float f = tileable ? float(period) : frequency;
        const uint seed = Hash(params.Seed + uint(o) * 1013u);
        float n = BasisNoise(params.BasisType, x * f, y * f, z * f, seed, period);
        switch (params.Fractal)
        {
            case 1: {
                n = params.RidgeOffset - abs(n);
                n *= n * weight;
                weight = Saturate(n * 2);
                sum += n * amplitude;
                break;
            }
            case 2:
                sum += (abs(n) * 2 - 1) * amplitude;
                break;
            case 3:
                sum += abs(n) * amplitude;
                break;
            case 4: {
                const float signal = (n + params.RidgeOffset * 0.7f) * amplitude;
                sum += o == 0 ? signal : signal * min(weight, 1.0f);
                weight = min(weight * signal * 2, 1.0f);
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
    float value = sum / max(norm, 1e-5f);
    switch (params.Fractal)
    {
        case 1:
            return Saturate(value / max(params.RidgeOffset * params.RidgeOffset, 0.25f));
        case 3:
            return Saturate(value * 1.6f);
        case 4:
            return Saturate(value * 0.8f);
        default:
            return Saturate(value * 0.8f + 0.5f);
    }
}

float FractalNoise(NoiseParams params, float x, float y, float z, bool tileable)
{
    if (params.Fractal == 5)
    {
        NoiseParams inner = params;
        inner.Fractal = 0;
        // 相関を抑えた 2 つの fBm 場で変形する。オフセットを周期の整数倍にしてタイル性を保つ。
        const float wx = FractalSum(inner, x + 5.2f, y + 1.3f, z + 2.7f, tileable) * 2 - 1;
        NoiseParams second = inner;
        second.Seed = params.Seed ^ 0x9e3779b9u;
        const float wy = FractalSum(second, x + 1.7f, y + 9.2f, z + 4.1f, tileable) * 2 - 1;
        const float s = params.WarpStrength / max(params.Frequency, 0.001f);
        return FractalSum(inner, x + wx * s, y + wy * s, z + (wx - wy) * s * 0.5f, tileable);
    }
    return FractalSum(params, x, y, z, tileable);
}

float Escape(TextureParams p, float u, float v, bool julia)
{
    const float scale = 3.0f / max(p.Zoom, 1e-4f);
    float zx = julia ? (u - 0.5) * scale : 0;
    float zy = julia ? (v - 0.5) * scale : 0;
    const float cx = julia ? p.JuliaX : p.CenterX + (u - 0.5) * scale;
    const float cy = julia ? p.JuliaY : p.CenterY + (v - 0.5) * scale;
    if (julia)
    {
        zx += p.CenterX + 0.5;
        zy += p.CenterY;
    }
    const int limit = clamp(p.Iterations, 8, 4000);
    int i = 0;
    for (; i < limit && zx * zx + zy * zy < 256.0; ++i)
    {
        const float t = zx * zx - zy * zy + cx;
        zy = 2 * zx * zy + cy;
        zx = t;
    }
    if (i >= limit)
    {
        return 0;
    }
    // 滑らかな反復回数。
    const float smoothing = log(log(sqrt(zx * zx + zy * zy)) / log(2.0)) / log(2.0);
    return Saturate(float((i + 1 - smoothing) / limit) * 4.0f);
}

float WaterHeight(TextureParams p, float x, float y, float z, bool tileable)
{
    // 波立つ fBm: 尖らせた稜線を時間とともに移動する。整数の移動量でタイル性を保つ。
    NoiseParams n = p.Noise;
    n.Fractal = 0;
    const float t = p.Time * 0.15f;
    const float a = FractalNoise(n, x + t, y + t * 0.6f, z + t * 0.3f, tileable);
    NoiseParams r = n;
    r.Fractal = 1;
    r.Seed ^= 0x51ed27u;
    const float b = FractalNoise(r, x - t * 0.7f, y + t * 0.4f, z - t * 0.2f, tileable);
    return Saturate(Lerp(a, b, p.Choppiness));
}

float TextureScalar(TextureParams p, float x, float y, float z, bool solid)
{
    const bool tile = p.Tileable && !solid;
    float value = 0;
    switch (p.Kind)
    {
        case 1: {
            const float n = FractalNoise(p.Noise, x, y, z, tile);
            // 縞の周波数を整数にし、タイル化したパターンの周期性を保つ。
            value = 0.5f + 0.5f * sin((x * round(p.RingCount) + n * p.Turbulence) * 2 * kPi);
            value = pow(value, 0.6f);
            break;
        }
        case 2: {
            const float n = FractalNoise(p.Noise, x, y, z, tile);
            const float cx = x - 0.5f;
            const float cz = (solid ? z : y) - 0.5f;
            const float r = sqrt(cx * cx + cz * cz) * p.RingCount * 2 + n * p.Turbulence * 0.5f;
            value = r - floor(r);
            value = Smooth(0.0f, 0.8f, value) * (1 - Smooth(0.85f, 1.0f, value));
            break;
        }
        case 3: {
            NoiseParams n = p.Noise;
            n.BasisType = 2;
            n.Fractal = 0;
            const float cells = FractalNoise(n, x, y, z, tile);
            const float grain = FractalNoise(p.Noise, x * 4, y * 4, z * 4, tile);
            value = Saturate(cells * 0.6f + grain * 0.4f);
            value = value > 0.62f ? 1.0f : value < 0.3f ? 0.0f : value;
            break;
        }
        case 4: {
            float f1;
            float f2;
            const int period = tile ? max(1, int(round(p.Noise.Frequency))) : 0;
            const float f = tile ? float(period) : p.Noise.Frequency;
            Worley(x * f, y * f, z * f, p.Noise.Seed, period, f1, f2);
            value = Saturate((f2 - f1) * 3.0f);
            break;
        }
        case 5: {
            const float bx = x * p.BricksX;
            const float by = y * p.BricksY;
            const int row = int(floor(by));
            const float sx = bx + (row & 1) * 0.5f;
            const float fx = sx - floor(sx);
            const float fy = by - floor(by);
            const float mx = p.Mortar * p.BricksX * 0.5f;
            const float my = p.Mortar * p.BricksY * 0.5f;
            const float edge = min(Smooth(0, mx, min(fx, 1 - fx)), Smooth(0, my, min(fy, 1 - fy)));
            const float variation =
                Unit(Hash3(Wrap(int(floor(sx)), max(p.BricksX, 1)), Wrap(row, max(p.BricksY, 1)), 0, p.Noise.Seed));
            value = edge * (0.6f + 0.4f * variation) * (0.85f + 0.15f * FractalNoise(p.Noise, x, y, z, tile));
            break;
        }
        case 6: {
            const int cx = int(floor(x * p.BricksX));
            const int cy = int(floor(y * p.BricksX));
            value = ((cx + cy) & 1) ? 1.0f : 0.0f;
            break;
        }
        case 7:
            value = WaterHeight(p, x, y, z, tile);
            break;
        case 8: {
            NoiseParams n = p.Noise;
            n.Fractal = 0;
            const float base = FractalNoise(n, x, y, z, tile);
            NoiseParams cell = n;
            cell.BasisType = 2;
            cell.Seed ^= 0x7f4a7c15u;
            const float billows = 1 - FractalNoise(cell, x, y, z, tile);
            const float shape = base * 0.65f + billows * 0.35f;
            const float threshold = 1 - p.Coverage;
            value = Smooth(threshold - p.Softness * 0.5f, threshold + p.Softness * 0.5f, shape);
            break;
        }
        case 9:
            value = Escape(p, x, y, false);
            break;
        case 10:
            value = Escape(p, x, y, true);
            break;
        default:
            value = FractalNoise(p.Noise, x, y, z, tile);
            break;
    }
    return Saturate((value - 0.5f) * p.Contrast + 0.5f + p.Bias);
}

float3 TextureColor(TextureParams p, float x, float y, float z, bool solid)
{
    const float v = TextureScalar(p, x, y, z, solid);
    switch (p.Kind)
    {
        case 5:
            // 目地には ColorB、レンガには ColorC を使い、レンガごとに陰影を変える。
            return lerp(p.ColorB, (p.ColorC * (0.6f + 0.6f * v)), Saturate(v * 4));
        case 7: {
            // 海用の固定配色。汎用ランプの色は石やノイズ向けに調整されている。
            const float3 deep = float3(0.01f, 0.07f, 0.13f);
            const float3 shallow = float3(0.04f, 0.32f, 0.42f);
            const float3 foam = float3(0.92f, 0.95f, 0.98f);
            const float3 water = lerp(deep, shallow, Smooth(0.2f, 0.75f, v));
            return lerp(water, foam, Smooth(0.72f, 0.92f, v) * p.Choppiness);
        }
        case 8: {
            const float3 sky = float3(0.32f, 0.52f, 0.86f);
            return lerp(sky, lerp(p.ColorB, float3(1, 1, 1), 0.5f), v);
        }
        case 9:
        case 10: {
            if (v <= 0)
            {
                return float3(0, 0, 0);
            }
            const float t = sqrt(v);
            const float glow = Saturate(t * 1.6f);
            return float3(0.5f + 0.5f * cos(2 * kPi * (t + 0.0f)), 0.5f + 0.5f * cos(2 * kPi * (t + 0.33f)),
                          0.5f + 0.5f * cos(2 * kPi * (t + 0.67f))) *
                   glow;
        }
        default:
            // A → B → C の 3 色ランプ。
            return v < 0.5f ? lerp(p.ColorA, p.ColorB, v * 2) : lerp(p.ColorB, p.ColorC, (v - 0.5f) * 2);
    }
}

float4 SampleImage(float2 uv)
{
    int width = Parameters.Load(172);
    int height = Parameters.Load(176);
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    float2 position = frac(uv) * float2(width, height) - 0.5;
    int2 cell = int2(floor(position));
    float2 weight = frac(position);
    float4 samples[4];
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < 2; ++column)
        {
            uint index = Wrap(cell.y + row, height) * width + Wrap(cell.x + column, width);
            samples[row * 2 + column] = asfloat(SourcePixels.Load4(index * 16));
        }
    }
    return lerp(lerp(samples[0], samples[1], weight.x), lerp(samples[2], samples[3], weight.x), weight.y);
}

struct BasicNoiseSettings
{
    uint Method;
    uint Seed;
    float Frequency;
    int Octaves;
    float Persistence;
    float Lacunarity;
    float WarpStrength;
};

BasicNoiseSettings LoadTerrainNoise(uint offset)
{
    BasicNoiseSettings p;
    p.Method = SourcePixels.Load(offset);
    p.Seed = SourcePixels.Load(offset + 4);
    p.Frequency = asfloat(SourcePixels.Load(offset + 8));
    p.Octaves = SourcePixels.Load(offset + 12);
    p.Persistence = asfloat(SourcePixels.Load(offset + 16));
    p.Lacunarity = asfloat(SourcePixels.Load(offset + 20));
    p.WarpStrength = asfloat(SourcePixels.Load(offset + 24));
    return p;
}

uint TerrainHash(int x, int y, uint seed)
{
    uint value = uint(x) * 0x9e3779b9u;
    value ^= uint(y) * 0x85ebca6bu;
    value ^= seed;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float TerrainRandom01(int x, int y, uint seed)
{
    return float(TerrainHash(x, y, seed) >> 8) / 16777216.0f;
}

float TerrainFade(float value)
{
    return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

float TerrainLerp(float first, float second, float weight)
{
    return first + (second - first) * weight;
}

float TerrainGradient(int x, int y, float offsetX, float offsetY, uint seed)
{
    const float diagonal = 0.70710678118f;
    switch (TerrainHash(x, y, seed) & 7u)
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

float LatticeNoise(float x, float y, uint seed, bool useGradient)
{
    const int cellX = int(floor(x));
    const int cellY = int(floor(y));
    const float localX = x - cellX;
    const float localY = y - cellY;
    float corners[4] = {0,0,0,0};
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < 2; ++column)
        {
            if (useGradient)
            {
                corners[row * 2 + column] =
                    TerrainGradient(cellX + column, cellY + row, localX - column, localY - row, seed) * 1.41421356237f;
            }
            else
            {
                corners[row * 2 + column] = TerrainRandom01(cellX + column, cellY + row, seed) * 2.0f - 1.0f;
            }
        }
    }
    const float lower = TerrainLerp(corners[0], corners[1], TerrainFade(localX));
    const float upper = TerrainLerp(corners[2], corners[3], TerrainFade(localX));
    return TerrainLerp(lower, upper, TerrainFade(localY));
}

float CellularNoise(float x, float y, uint seed)
{
    const int cellX = int(floor(x));
    const int cellY = int(floor(y));
    float nearestSquared = 1e30f;
    for (int row = -1; row <= 1; ++row)
    {
        for (int column = -1; column <= 1; ++column)
        {
            const int sampleX = cellX + column;
            const int sampleY = cellY + row;
            const float deltaX = sampleX + TerrainRandom01(sampleX, sampleY, seed) - x;
            const float deltaY = sampleY + TerrainRandom01(sampleX, sampleY, seed ^ 0xa511e9b3u) - y;
            nearestSquared = min(nearestSquared, deltaX * deltaX + deltaY * deltaY);
        }
    }
    return 1.0f - 2.0f * clamp(sqrt(nearestSquared), 0.0f, 1.0f);
}

float TerrainFractalNoise(float x, float y, BasicNoiseSettings settings, bool ridged)
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float amplitudeSum = 0.0f;
    const int octaves = clamp(settings.Octaves, 1, 8);
    for (int octave = 0; octave < octaves; ++octave)
    {
        float sample = LatticeNoise(x, y, settings.Seed + uint(octave) * 1013u, true);
        if (ridged)
        {
            sample = 1.0f - abs(sample);
            sample = sample * sample * 2.0f - 1.0f;
        }
        total += sample * amplitude;
        amplitudeSum += amplitude;
        amplitude *= clamp(settings.Persistence, 0.0f, 1.0f);
        x *= clamp(settings.Lacunarity, 1.0f, 4.0f);
        y *= clamp(settings.Lacunarity, 1.0f, 4.0f);
    }
    return total / amplitudeSum;
}

float SampleSingleNoise(float x, float y, BasicNoiseSettings settings)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(settings.Frequency))
    {
        return 0.0f;
    }
    x *= clamp(settings.Frequency, 0.0001f, 2.0f);
    y *= clamp(settings.Frequency, 0.0001f, 2.0f);
    // 最大オクターブでも整数セルへの変換が定義範囲内に収まるようにする。
    x = clamp(x, -10000.0f, 10000.0f);
    y = clamp(y, -10000.0f, 10000.0f);
    float result = 0.0f;
    switch (settings.Method)
    {
        case 0:
            result = LatticeNoise(x, y, settings.Seed, true);
            break;
        case 1:
            result = LatticeNoise(x, y, settings.Seed, false);
            break;
        case 2:
            result = CellularNoise(x, y, settings.Seed);
            break;
        case 3:
            result = TerrainFractalNoise(x, y, settings, false);
            break;
        case 4:
            result = TerrainFractalNoise(x, y, settings, true);
            break;
        case 5: {
            const float warp = clamp(settings.WarpStrength, 0.0f, 10.0f);
            const float warpedX = x + LatticeNoise(x + 17.3f, y + 9.2f, settings.Seed, true) * warp;
            const float warpedY = y + LatticeNoise(x - 7.1f, y + 31.7f, settings.Seed ^ 239u, true) * warp;
            result = TerrainFractalNoise(warpedX, warpedY, settings, false);
            break;
        }
        default:
            break;
    }
    return clamp(result, -1.0f, 1.0f);
}

float TerrainHeight(uint index, uint side)
{
    float size = clamp(asfloat(SourcePixels.Load(196)), 1.0, 500.0);
    float height = clamp(asfloat(SourcePixels.Load(200)), 0.0, 100.0);
    float baseHeight = asfloat(SourcePixels.Load(204));
    float2 coordinate = size * (float2(index % side, index / side) / float(side - 1) - 0.5);
    BasicNoiseSettings settings = LoadTerrainNoise(0);
    float noise = 0;
    if (settings.Method != 6)
    {
        noise = SampleSingleNoise(coordinate.x, coordinate.y, settings);
    }
    else
    {
        int count = clamp(int(SourcePixels.Load(28)), 2, 5);
        float total = 0;
        float weightSum = 0;
        for (int layer = 0; layer < count; ++layer)
        {
            uint offset = 32 + layer * 32;
            float weight = asfloat(SourcePixels.Load(offset + 28));
            if (!isfinite(weight) || weight <= 0)
            {
                continue;
            }
            total += SampleSingleNoise(coordinate.x, coordinate.y, LoadTerrainNoise(offset)) * weight;
            weightSum += weight;
        }
        if (weightSum > 0)
        {
            noise = total / weightSum;
        }
    }
    return baseHeight + noise * height;
}


float3 GridPosition(uint vertex, uint resolution, uint mode, TextureParams p)
{
    float2 uv=float2(vertex%(resolution+1),vertex/(resolution+1))/resolution;
    float height=0;
    if(mode==5)
    {
        height=FractalNoise(p.Noise,uv.x,uv.y,0.37,false)*p.Turbulence;
    }
    else if(mode==6)
    {
        height=(TextureScalar(p,uv.x,uv.y,0,false)-0.5)*2*p.Coverage;
    }
    return float3((uv.x-0.5)*p.RingCount,height,(uv.y-0.5)*p.RingCount);
}
float3 GridTerrainColor(float height,float slope)
{
    float3 rock=float3(0.42,0.38,0.34);
    float3 color=lerp(float3(0.76,0.70,0.50),float3(0.22,0.45,0.16),Smooth(0.15,0.25,height));
    color=lerp(color,rock,Smooth(0.55,0.7,height));
    color=lerp(color,float3(0.95,0.96,0.98),Smooth(0.78,0.88,height));
    return lerp(color,rock,Smooth(0.55,0.8,slope)*(1-Smooth(0.85,0.95,height)));
}
void GenerateGridVertex(uint index,uint mode,TextureParams p)
{
    uint resolution=Parameters.Load(172);
    uint side=resolution+1;
    uint vertexCount=side*side;
    uint indexCount=resolution*resolution*6;
    if(index<indexCount)
    {
        uint cell=index/6;
        uint a=(cell/resolution)*side+cell%resolution;
        uint corners[6]={a,a+side,a+1,a+1,a+side,a+side+1};
        Output.Store(vertexCount*44+index*4,corners[index%6]);
    }
    if(index>=vertexCount)
    {
        return;
    }
    float3 position=GridPosition(index,resolution,mode,p);
    float3 normal=0;
    int2 cell=int2(index%side,index/side);
    for(int row=max(cell.y-1,0);row<=min(cell.y,int(resolution)-1);++row)
    {
        for(int column=max(cell.x-1,0);column<=min(cell.x,int(resolution)-1);++column)
        {
            uint a=row*side+column;
            uint triangles[6]={a,a+side,a+1,a+1,a+side,a+side+1};
            for(uint faceIndex=0;faceIndex<2;++faceIndex)
            {
                uint3 vertices=uint3(triangles[faceIndex*3],triangles[faceIndex*3+1],triangles[faceIndex*3+2]);
                if(any(vertices==index))
                {
                    float3 first=GridPosition(vertices.x,resolution,mode,p);
                    float3 second=GridPosition(vertices.y,resolution,mode,p);
                    float3 third=GridPosition(vertices.z,resolution,mode,p);
                    normal+=cross(second-first,third-first);
                }
            }
        }
    }
    if(length(normal)>1e-12)
    {
        normal=normalize(normal);
    }
    else
    {
        normal=float3(0,1,0);
    }
    float3 color=1;
    if(mode==5 && p.Output!=0)
    {
        color=GridTerrainColor(position.y/max(p.Turbulence,1e-3),(1-abs(normal.y))*2.5);
    }
    else if(mode==6)
    {
        float weight=saturate(position.y/max(p.Coverage,1e-3)*0.5+0.5);
        color=lerp(float3(0.02,0.12,0.2),float3(0.1,0.4,0.5),weight);
    }
    uint offset=index*44;
    Output.Store3(offset,asuint(position));Output.Store3(offset+12,asuint(normal));
    Output.Store3(offset+24,asuint(color));
    Output.Store2(offset+36,asuint(float2(cell)/resolution));
}
#include "ProceduralModelWorkGraph.hlsli"
#include "ProceduralRockWorkGraph.hlsli"
struct GenerationRecord
{
    uint3 Grid : SV_DispatchGrid;
};
[Shader("node")][NodeLaunch("broadcasting")][NodeIsProgramEntry][NodeMaxDispatchGrid(4096, 4095, 1)][NumThreads(64, 1,
                                                                                                                1)] void
Generate(DispatchNodeInputRecord<GenerationRecord> record, uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint index = (group.y * 4096 + group.x) * 64 + thread;
    uint mode = Parameters.Load(0);
    uint width = Parameters.Load(4);
    uint height = Parameters.Load(8);
    uint depth = Parameters.Load(12);
    if (index >= width * height * depth)
    {
        return;
    }
    if(mode==12)
    {
        if(index==0)
        {
            GenerateRock(LoadParameters());
        }
        return;
    }
    if(mode>=8 && mode<=11)
    {
        GenerateAnalyticVertex(index,mode);
        return;
    }
    if(mode>=5 && mode<=7)
    {
        GenerateGridVertex(index,mode,LoadParameters());
        return;
    }
    if (mode == 4)
    {
        Output.Store(index * 4, asuint(TerrainHeight(index, width)));
        return;
    }
    uint3 pixel = uint3(index % width, (index / width) % height, index / (width * height));
    float3 coordinate = (float3(pixel) + 0.5) / float3(width, height, depth);
    TextureParams p = LoadParameters();
    if (mode == 3)
    {
        float sum = 0;
        float norm = 0;
        float amplitude = 1;
        int octaves = clamp(int(Parameters.Load(180)), 1, 6);
        float variation = asfloat(Parameters.Load(184));
        uint seed = Parameters.Load(188);
        for (int octave = 0; octave < octaves; ++octave)
        {
            float scale = float(1u << octave);
            uint h = Hash(seed + uint(octave) * 977u);
            float angle = coordinate.y * 2 * kPi;
            float ox = round(variation * 2) * cos(angle) * 0.5 / scale + Unit(h);
            float oz = round(variation * 2) * sin(angle) * 0.5 / scale + Unit(Hash(h));
            float3 a = SampleImage(float2(coordinate.x * scale + ox, coordinate.z * scale + oz)).rgb;
            float3 b = SampleImage(float2(coordinate.z * scale - oz + 0.37, coordinate.y * scale + ox)).rgb;
            sum += (dot(a, 1.0 / 3.0) * 0.7 + dot(b, 1.0 / 3.0) * 0.3) * amplitude;
            norm += amplitude;
            amplitude *= 0.5;
        }
        Output.Store(index * 4, asuint(saturate(sum / norm)));
        return;
    }
    if (mode == 1)
    {
        p.Tileable = true;
        Output.Store(index * 4, asuint(TextureScalar(p, coordinate.x, coordinate.y, coordinate.z, false)));
        return;
    }
    float4 color;
    if (mode == 2 || p.Output == 0)
    {
        float z = coordinate.z;
        if (mode == 0)
        {
            z = 0;
        }
        color = float4(TextureColor(p, coordinate.x, coordinate.y, z, mode == 2), 1);
    }
    else if (p.Output == 1)
    {
        float scalar = TextureScalar(p, coordinate.x, coordinate.y, 0, false);
        color = float4(scalar, scalar, scalar, 1);
    }
    else
    {
        float left = (Wrap(int(pixel.x) - 1, width) + 0.5) / width;
        float right = (Wrap(int(pixel.x) + 1, width) + 0.5) / width;
        float lower = (Wrap(int(pixel.y) - 1, height) + 0.5) / height;
        float upper = (Wrap(int(pixel.y) + 1, height) + 0.5) / height;
        float dx = (TextureScalar(p, right, coordinate.y, 0, false) - TextureScalar(p, left, coordinate.y, 0, false)) *
                   p.NormalStrength;
        float dy = (TextureScalar(p, coordinate.x, upper, 0, false) - TextureScalar(p, coordinate.x, lower, 0, false)) *
                   p.NormalStrength;
        color = float4(normalize(float3(-dx, dy, 1)) * 0.5 + 0.5, 1);
    }
    Output.Store4(index * 16, asuint(color));
}
