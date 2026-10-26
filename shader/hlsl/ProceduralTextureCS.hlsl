// Pixel-independent procedural work. Escape fractals retain the CPU double-precision path.
cbuffer Parameters : register(b0)
{
    uint Width, Height, Kind, Output;
    uint BasisType, Fractal, Seed, Tileable;
    float Frequency;
    int Octaves;
    float Lacunarity, Gain;
    float WarpStrength, RidgeOffset, Contrast, Bias;
    float4 ColorA, ColorB, ColorC;
    float Turbulence, RingCount, Mortar, Time;
    int BricksX, BricksY, Pattern, RepeatX;
    int RepeatY;
    float PatternSize, PatternSoftness, NormalStrength;
    float Choppiness, Coverage, CloudSoftness, Padding;
    uint PreviewSize, Tiles, Padding1, Padding2;
};
RWStructuredBuffer<float4> Generated : register(u0);
StructuredBuffer<float4> ImagePixels : register(t0);
RWByteAddressBuffer PreviewPixels : register(u1);
// Enum values mirror proceduralstudio.h; GPU parity tests exercise every supported value.
static const uint BasisPerlin = 0, BasisValue = 1, BasisWorley = 2;
static const uint FractalFBm = 0, FractalRidged = 1, FractalBillow = 2;
static const uint FractalTurbulence = 3, FractalHybrid = 4, FractalDomainWarp = 5;
static const uint TextureMarble = 1, TextureWood = 2, TextureGranite = 3, TextureVoronoi = 4;
static const uint TextureBrick = 5, TextureChecker = 6, TextureWater = 7, TextureCloud = 8;
static const uint TextureGeometric = 11;
static const uint OutputColor = 0;
static const int PatternStripes = 0, PatternGrid = 1, PatternDots = 2;
static const int PatternDiamonds = 3, PatternHexagons = 4, PatternTriangles = 5;

static const float Pi = 3.14159265358979323846;

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
    return Hash((uint)cell.x * 0x8da6b343 ^ Hash((uint)cell.y * 0xd8163841 ^ Hash((uint)cell.z * 0xcb1ab31f ^ seed)));
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
    [loop]
    for (int z = -1; z <= 1; ++z)
    {
        [loop]
        for (int y = -1; y <= 1; ++y)
        {
            [loop]
            for (int x = -1; x <= 1; ++x)
            {
                int3 neighbor = cell + int3(x, y, z);
                uint h = Hash3(WrapCell(neighbor, period), seed);
                float3 featurePosition = neighbor + float3(Unit(h), Unit(Hash(h ^ 0x68bc21eb)), Unit(Hash(h ^ 0x02e5be93)));
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
        float value = basis == BasisWorley ? saturate(distances.x * 1.2) : saturate((distances.y - distances.x) * 2);
        return value * 2 - 1;
    }
    int3 cell = (int3)floor(p);
    float3 local = p - cell;
    float3 fade = local * local * local * (local * (local * 6 - 15) + 10);
    float corners[8];
    [unroll]
    for (int index = 0; index < 8; ++index)
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

float FractalSum(float3 p, uint basis, uint fractal, uint seed, bool tileable)
{
    float frequency = max(Frequency, 0.001);
    // CPU round uses half-away-from-zero; tile frequencies are positive.
    float lacunarity = tileable ? max(1.0, floor(Lacunarity + 0.5)) : Lacunarity;
    float amplitude = 1;
    float sum = 0;
    float norm = 0;
    float weight = 1;
    [loop]
    for (int octave = 0; octave < clamp(Octaves, 1, 12); ++octave)
    {
        int period = tileable ? max(1, (int)floor(frequency + 0.5)) : 0;
        float scale = tileable ? (float)period : frequency;
        float n = BasisNoise(basis, p * scale, Hash(seed + octave * 1013), period);
        switch (fractal)
        {
            case FractalRidged:
                n = RidgeOffset - abs(n);
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
            case FractalHybrid:
            {
                float signal = (n + RidgeOffset * 0.7) * amplitude;
                sum += octave == 0 ? signal : signal * min(weight, 1.0);
                weight = min(weight * signal * 2, 1.0);
                break;
            }
            default:
                sum += n * amplitude;
                break;
        }
        norm += amplitude;
        amplitude *= Gain;
        frequency *= lacunarity;
    }
    float value = sum / max(norm, 1e-5);
    switch (fractal)
    {
        case FractalRidged:
            return saturate(value / max(RidgeOffset * RidgeOffset, 0.25));
        case FractalTurbulence:
            return saturate(value * 1.6);
        case FractalHybrid:
            return saturate(value * 0.8);
        default:
            return saturate(value * 0.8 + 0.5);
    }
}

float Noise(float3 p, uint basis, uint fractal, uint seed)
{
    bool tileable = Tileable != 0;
    if (fractal != FractalDomainWarp)
    {
        return FractalSum(p, basis, fractal, seed, tileable);
    }
    float wx = FractalSum(p + float3(5.2, 1.3, 2.7), basis, FractalFBm, seed, tileable) * 2 - 1;
    float wy = FractalSum(p + float3(1.7, 9.2, 4.1), basis, FractalFBm, seed ^ 0x9e3779b9, tileable) * 2 - 1;
    float scale = WarpStrength / max(Frequency, 0.001);
    return FractalSum(p + float3(wx, wy, (wx - wy) * 0.5) * scale, basis, FractalFBm, seed, tileable);
}

float GeometricMask(float2 uv)
{
    float2 local = frac(uv * clamp(int2(RepeatX, RepeatY), 1, 128)) - 0.5;
    float size = clamp(PatternSize, 0.05, 0.95);
    float softness = clamp(PatternSoftness, 0.001, 0.15);
    float distance = 0;
    switch (Pattern)
    {
        case PatternStripes:
            distance = abs(local.x) - size * 0.5;
            break;
        case PatternGrid:
            distance = min(0.5 - abs(local.x), 0.5 - abs(local.y)) - size * 0.5;
            break;
        case PatternDots:
            distance = length(local) - size * 0.5;
            break;
        case PatternDiamonds:
            distance = (abs(local.x) + abs(local.y) - size * 0.5) / sqrt(2.0);
            break;
        case PatternHexagons:
            distance = max(abs(local.x) * 0.8660254 + abs(local.y) * 0.5, abs(local.y)) - size * 0.4330127;
            break;
        case PatternTriangles:
            distance = 1;
            [unroll]
            for (int y = -1; y <= 1; ++y)
            {
                [unroll]
                for (int x = -1; x <= 1; ++x)
                {
                    float2 triangleLocal = local + int2(x, y);
                    distance = min(distance, max(abs(triangleLocal.x) * 0.8660254 + triangleLocal.y * 0.5, -triangleLocal.y) - size * 0.25);
                }
            }
            break;
        default:
            return 0;
    }
    return 1 - Smooth(-softness * 0.5, softness * 0.5, distance);
}

float Scalar(float2 uv)
{
    float3 p = float3(uv, 0);
    float2 noiseValues = 0;
    int noiseCount = (Kind == TextureVoronoi || Kind == TextureChecker || Kind == TextureGeometric) ? 0 : 1;
    if (Kind == TextureGranite || Kind == TextureWater || Kind == TextureCloud)
    {
        noiseCount = 2;
    }
    // A shared call site keeps the GPU program small instead of inlining the full
    // fractal evaluator separately into every material branch.
    [loop]
    for (int sampleIndex = 0; sampleIndex < noiseCount; ++sampleIndex)
    {
        float3 position = p;
        uint basis = BasisType;
        uint fractal = Fractal;
        uint seed = Seed;
        if (Kind == TextureGranite)
        {
            if (sampleIndex == 0)
            {
                basis = BasisWorley;
                fractal = FractalFBm;
            }
            else
            {
                position *= 4;
            }
        }
        else if (Kind == TextureWater)
        {
            float time = Time * 0.15;
            fractal = sampleIndex == 0 ? FractalFBm : FractalRidged;
            position += sampleIndex == 0 ? float3(time, time * 0.6, time * 0.3)
                                        : float3(-time * 0.7, time * 0.4, -time * 0.2);
            if (sampleIndex == 1)
            {
                seed ^= 0x51ed27;
            }
        }
        else if (Kind == TextureCloud)
        {
            fractal = FractalFBm;
            if (sampleIndex == 1)
            {
                basis = BasisWorley;
                seed ^= 0x7f4a7c15;
            }
        }
        noiseValues[sampleIndex] = Noise(position, basis, fractal, seed);
    }
    float value = 0;
    switch (Kind)
    {
        case TextureGeometric:
            value = GeometricMask(uv);
            break;
        case TextureMarble:
            value = 0.5 + 0.5 * sin((uv.x * floor(RingCount + 0.5) + noiseValues.x * Turbulence) * 2 * Pi);
            value = pow(value, 0.6);
            break;
        case TextureWood:
        {
            float radius = length(uv - 0.5) * RingCount * 2 + noiseValues.x * Turbulence * 0.5;
            value = frac(radius);
            value = Smooth(0, 0.8, value) * (1 - Smooth(0.85, 1, value));
            break;
        }
        case TextureGranite:
            value = saturate(noiseValues.x * 0.6 + noiseValues.y * 0.4);
            value = value > 0.62 ? 1 : value < 0.3 ? 0 : value;
            break;
        case TextureVoronoi:
        {
            int period = Tileable != 0 ? max(1, (int)floor(Frequency + 0.5)) : 0;
            float scale = Tileable != 0 ? (float)period : Frequency;
            float2 distances = Worley(p * scale, Seed, period);
            value = saturate((distances.y - distances.x) * 3);
            break;
        }
        case TextureBrick:
        {
            float2 brick = uv * int2(BricksX, BricksY);
            int row = (int)floor(brick.y);
            float shifted = brick.x + (row & 1) * 0.5;
            float2 local = float2(frac(shifted), frac(brick.y));
            float edge = min(Smooth(0, Mortar * BricksX * 0.5, min(local.x, 1 - local.x)),
                             Smooth(0, Mortar * BricksY * 0.5, min(local.y, 1 - local.y)));
            float variation = Unit(Hash3(int3(Wrap((int)floor(shifted), max(BricksX, 1)), Wrap(row, max(BricksY, 1)), 0), Seed));
            value = edge * (0.6 + 0.4 * variation) * (0.85 + 0.15 * noiseValues.x);
            break;
        }
        case TextureChecker:
            value = (((int)floor(uv.x * BricksX) + (int)floor(uv.y * BricksX)) & 1) != 0 ? 1 : 0;
            break;
        case TextureWater:
        {
            value = saturate(lerp(noiseValues.x, noiseValues.y, Choppiness));
            break;
        }
        case TextureCloud:
        {
            float shape = noiseValues.x * 0.65 + (1 - noiseValues.y) * 0.35;
            value = Smooth(1 - Coverage - CloudSoftness * 0.5, 1 - Coverage + CloudSoftness * 0.5, shape);
            break;
        }
        default:
            value = noiseValues.x;
            break;
    }
    return saturate((value - 0.5) * Contrast + 0.5 + Bias);
}

float3 Color(float value)
{
    switch (Kind)
    {
        case TextureGeometric:
            return lerp(ColorA.rgb, ColorB.rgb, value);
        case TextureBrick:
            return lerp(ColorB.rgb, ColorC.rgb * (0.6 + 0.6 * value), saturate(value * 4));
        case TextureWater:
        {
            float3 water = lerp(float3(0.01, 0.07, 0.13), float3(0.04, 0.32, 0.42), Smooth(0.2, 0.75, value));
            return lerp(water, float3(0.92, 0.95, 0.98), Smooth(0.72, 0.92, value) * Choppiness);
        }
        case TextureCloud:
            return lerp(float3(0.32, 0.52, 0.86), lerp(ColorB.rgb, 1.0, 0.5), value);
        default:
            return value < 0.5 ? lerp(ColorA.rgb, ColorB.rgb, value * 2) : lerp(ColorB.rgb, ColorC.rgb, (value - 0.5) * 2);
    }
}

float HeightAt(int2 pixel)
{
    int2 wrapped = int2(Wrap(pixel.x, Width), Wrap(pixel.y, Height));
    return ImagePixels[wrapped.y * Width + wrapped.x].x;
}

[numthreads(8, 8, 1)]
void Generate(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= Width || thread.y >= Height)
    {
        return;
    }
    float value = Scalar((thread.xy + 0.5) / float2(Width, Height));
    if (Kind == TextureChecker)
    {
        // Integer cell addressing avoids reciprocal rounding at hard checker boundaries.
        uint cellX = ((thread.x * 2 + 1) * (uint)BricksX) / (Width * 2);
        uint cellY = ((thread.y * 2 + 1) * (uint)BricksX) / (Height * 2);
        value = saturate((((cellX + cellY) & 1) - 0.5) * Contrast + 0.5 + Bias);
    }
    float3 color = Output == OutputColor ? Color(value) : value.xxx;
    Generated[thread.y * Width + thread.x] = float4(color, 1);
}

[numthreads(8, 8, 1)]
void Normals(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= Width || thread.y >= Height)
    {
        return;
    }
    int2 pixel = int2(thread.xy);
    float dx = (HeightAt(pixel + int2(1, 0)) - HeightAt(pixel - int2(1, 0))) * NormalStrength;
    float dy = (HeightAt(pixel + int2(0, 1)) - HeightAt(pixel - int2(0, 1))) * NormalStrength;
    float3 normal = normalize(float3(-dx, dy, 1));
    Generated[thread.y * Width + thread.x] = float4(normal * 0.5 + 0.5, 1);
}

float4 Pixel(int2 pixel)
{
    return ImagePixels[Wrap(pixel.y, Height) * Width + Wrap(pixel.x, Width)];
}

[numthreads(8, 8, 1)]
void Preview(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= PreviewSize || thread.y >= PreviewSize)
    {
        return;
    }
    float2 uv = (thread.xy + 0.5) / (float)PreviewSize * Tiles;
    float2 pixel = frac(uv) * float2(Width, Height) - 0.5;
    int2 start = (int2)floor(pixel);
    float2 blend = frac(pixel);
    float3 color = lerp(lerp(Pixel(start), Pixel(start + int2(1, 0)), blend.x),
                        lerp(Pixel(start + int2(0, 1)), Pixel(start + int2(1, 1)), blend.x), blend.y).rgb;
    if (Output == OutputColor)
    {
        color = saturate(color);
        color = float3(color.r <= 0.0031308 ? color.r * 12.92 : 1.055 * pow(color.r, 1.0 / 2.4) - 0.055,
                       color.g <= 0.0031308 ? color.g * 12.92 : 1.055 * pow(color.g, 1.0 / 2.4) - 0.055,
                       color.b <= 0.0031308 ? color.b * 12.92 : 1.055 * pow(color.b, 1.0 / 2.4) - 0.055);
    }
    uint3 packed = (uint3)(saturate(color) * 255 + 0.5);
    uint rowPitch = (PreviewSize * 4 + 255) & ~255;
    PreviewPixels.Store(thread.y * rowPitch + thread.x * 4, packed.x | (packed.y << 8) | (packed.z << 16) | 0xff000000);
}
