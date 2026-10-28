#ifndef RENDER_NOISE_HLSL
#define RENDER_NOISE_HLSL

// Include-only noise library. No shader entry point or resource bindings.
namespace TextureNoise
{
    static const uint BasisPerlin = 0;
    static const uint BasisValue = 1;
    static const uint BasisWorley = 2;
    static const uint BasisCellEdge = 3;
    static const uint BasisWhite = 4;
    static const uint BasisSimplex = 5;
    static const uint BasisVoronoi = 6;
    static const uint BasisCell = 7;
    static const uint BasisGabor = 8;
    static const uint BasisWavelet = 9;
    static const uint BasisCurl = 10;
    static const uint BasisValueGradient = 11;
    static const uint BasisSparseConvolution = 12;
    static const uint BasisSpot = 13;
    static const uint BasisLocalRandom = 14;
    static const uint BasisChevron = 15;
    static const uint BasisLatticeFree = 16;
    static const uint BasisPhasor = 17;
    static const uint BasisAnisotropicGabor = 18;
    static const uint BasisHarmonic = 19;
    static const uint BasisFlow = 20;
    static const uint BasisRidged = 21;
    static const uint FractalFBm = 0, FractalRidged = 1, FractalBillow = 2;
    static const uint FractalTurbulence = 3, FractalHybrid = 4, FractalDomainWarp = 5;
    static const uint FractalNone = 6, FractalMultifractal = 7, FractalHeterogeneous = 8;

    struct NoiseSettings
    {
        float Frequency;
        int Octaves;
        float Lacunarity;
        float Gain;
        float WarpStrength;
        float RidgeOffset;
        uint Tileable;
        float Direction, Anisotropy, KernelFrequency, Phase;
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

    float LatticeBasisNoise(uint basis, float3 p, uint seed, int period)
    {
        if (basis == BasisWorley || basis == BasisCellEdge)
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

    float Fade(float t)
    {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }

    uint HashXYZ(int x, int y, int z, uint seed)
    {
        return Hash3(int3(x, y, z), seed);
    }

    float Perlin(float x, float y, float z, uint seed, int period)
    {
        return LatticeBasisNoise(BasisPerlin, float3(x, y, z), seed, period);
    }

    float ValueNoise(float x, float y, float z, uint seed, int period)
    {
        return LatticeBasisNoise(BasisValue, float3(x, y, z), seed, period);
    }

    // Compact kernels share a bounded neighborhood and wrapped *hashes*, not wrapped
    // offsets: this is essential to preserve both continuity and tileability.
    float SimplexRaw(float x, float y, float z, uint seed)
    {
        const float skew = (x + y + z) / 3.0f;
        const int i = int(floor(x + skew));
        const int j = int(floor(y + skew));
        const int k = int(floor(z + skew));
        const float unskew = (i + j + k) / 6.0f;
        const float a = x - (i - unskew), b = y - (j - unskew), c = z - (k - unskew);
        const int ix = a >= b && a >= c ? 1 : 0;
        const int iy = b > a && b >= c ? 1 : 0;
        const int iz = c > a && c > b ? 1 : 0;
        const int jx = a >= b || a >= c ? 1 : 0;
        const int jy = b > a || b >= c ? 1 : 0;
        const int jz = c > a || c > b ? 1 : 0;
        float sum = 0;
        [loop] for (int corner = 0; corner < 4; ++corner)
        {
            const int dx = corner == 0 ? 0 : (corner == 1 ? ix : (corner == 2 ? jx : 1));
            const int dy = corner == 0 ? 0 : (corner == 1 ? iy : (corner == 2 ? jy : 1));
            const int dz = corner == 0 ? 0 : (corner == 1 ? iz : (corner == 2 ? jz : 1));
            const float px = a - dx + corner / 6.0f;
            const float py = b - dy + corner / 6.0f;
            const float pz = c - dz + corner / 6.0f;
            const float t = max(0.0f, 0.6f - px * px - py * py - pz * pz);
            sum += t * t * t * t * GradientDot(HashXYZ(i + dx, j + dy, k + dz, seed), float3(px, py, pz));
        }
        return clamp(32.0f * sum, -1.0f, 1.0f);
    }

    float Simplex(float x, float y, float z, uint seed, int period)
    {
        if (period <= 0)
        {
            return SimplexRaw(x, y, z, seed);
        }
        // Blend translated simplex fields with quintic weights. Skewed simplex
        // lattice coordinates cannot be wrapped as Cartesian lattice cells.
        const float px = x - floor(x / period) * period;
        const float py = y - floor(y / period) * period;
        const float pz = z - floor(z / period) * period;
        const float u = Fade(px / period), v = Fade(py / period), w = Fade(pz / period);
        float sum = 0;
        [loop] for (int dz = 0; dz < 2; ++dz)
        {
            [loop] for (int dy = 0; dy < 2; ++dy)
            {
                [loop] for (int dx = 0; dx < 2; ++dx)
                {
                    sum += SimplexRaw(px - dx * period, py - dy * period, pz - dz * period, seed) *
                           (dx == 0 ? 1 - u : u) * (dy == 0 ? 1 - v : v) * (dz == 0 ? 1 - w : w);
                }
            }
        }
        return sum;
    }

    float VoronoiCell(float x, float y, float z, uint seed, int period)
    {
        const int ix = int(floor(x)), iy = int(floor(y));
        const int iz = int(floor(z));
        uint nearestHash = HashXYZ(Wrap(ix, period), Wrap(iy, period), Wrap(iz, period), seed);
        const float centerX = ix + Unit(nearestHash) - x;
        const float centerY = iy + Unit(Hash(nearestHash ^ 0x68bc21ebu)) - y;
        const float centerZ = iz + Unit(Hash(nearestHash ^ 0x02e5be93u)) - z;
        float nearest = centerX * centerX + centerY * centerY + centerZ * centerZ;
        int nearestOrder = 62; // Center of the original 5x5x5 traversal; preserve ties.
        // Keep the radius-two search, but reject cells whose entire box is farther
        // than an already known feature. No UV quantization or baked image is used.
        [loop] for (int dz = -2; dz <= 2; ++dz)
        {
            const float lowerZ = max(0.0f, max(iz + dz - z, z - (iz + dz + 1)));
            const float distanceZ = lowerZ * lowerZ;
            // A small conservative margin keeps rounded distance bounds from rejecting ties.
            [branch] if (distanceZ > nearest + 1e-6f)
            {
                continue;
            }
            const int wrappedZ = Wrap(iz + dz, period);
            const uint zHash = Hash((uint)wrappedZ * 0xcb1ab31fu ^ seed);
            [loop] for (int dy = -2; dy <= 2; ++dy)
            {
                const float lowerY = max(0.0f, max(iy + dy - y, y - (iy + dy + 1)));
                const float distanceYZ = lowerY * lowerY + distanceZ;
                [branch] if (distanceYZ > nearest + 1e-6f)
                {
                    continue;
                }
                const int wrappedY = Wrap(iy + dy, period);
                const uint yzHash = Hash((uint)wrappedY * 0xd8163841u ^ zHash);
                [loop] for (int dx = -2; dx <= 2; ++dx)
                {
                    const float lowerX = max(0.0f, max(ix + dx - x, x - (ix + dx + 1)));
                    [branch] if (lowerX * lowerX + distanceYZ > nearest + 1e-6f)
                    {
                        continue;
                    }
                    // The Y/Z hash is fixed for this row; reuse it for every X cell.
                    const uint h = Hash((uint)Wrap(ix + dx, period) * 0x8da6b343u ^ yzHash);
                    const float a = ix + dx + Unit(h) - x;
                    const float b = iy + dy + Unit(Hash(h ^ 0x68bc21ebu)) - y;
                    const float c = iz + dz + Unit(Hash(h ^ 0x02e5be93u)) - z;
                    const float d = a * a + b * b + c * c;
                    const int order = (dz + 2) * 25 + (dy + 2) * 5 + dx + 2;
                    if (d < nearest || (d == nearest && order < nearestOrder))
                    {
                        nearest = d;
                        nearestHash = h;
                        nearestOrder = order;
                    }
                }
            }
        }
        return Unit(Hash(nearestHash ^ 0xb5297a4du)) * 2 - 1;
    }

    float Flow(float x, float y, float z, uint seed, int period, float phase)
    {
        const int ix = int(floor(x)), iy = int(floor(y));
        const int iz = int(floor(z));
        const float a = x - ix, b = y - iy, c = z - iz;
        const float u = Fade(a), v = Fade(b), w = Fade(c);
        const float co = cos(phase), si = sin(phase);
        float sum = 0;
        [loop] for (int dz = 0; dz < 2; ++dz)
        {
            [loop] for (int dy = 0; dy < 2; ++dy)
            {
                [loop] for (int dx = 0; dx < 2; ++dx)
                {
                    const uint h = HashXYZ(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                    // Rotate each lattice gradient, rather than translating the field.
                    const float px = a - dx, py = b - dy, pz = c - dz;
                    const float g = GradientDot(h, float3(co * px + si * py, -si * px + co * py, pz));
                    sum += g * (dx == 0 ? 1 - u : u) * (dy == 0 ? 1 - v : v) * (dz == 0 ? 1 - w : w);
                }
            }
        }
        return clamp(sum * 0.95f, -1.0f, 1.0f);
    }

    float3 CurlVector(float x, float y, float z, uint seed, int period)
    {
        // Curl of three independent scalar potentials; do not normalize the
        // vector, since doing so would destroy its divergence-free property.
        const float e = 0.002f;
        const uint sy = seed ^ 0x68bc21ebu, sz = seed ^ 0x02e5be93u;
        const float cx = (Perlin(x, y + e, z, sz, period) - Perlin(x, y - e, z, sz, period) -
                          Perlin(x, y, z + e, sy, period) + Perlin(x, y, z - e, sy, period)) /
                         (2 * e);
        const float cy = (Perlin(x, y, z + e, seed, period) - Perlin(x, y, z - e, seed, period) -
                          Perlin(x + e, y, z, sz, period) + Perlin(x - e, y, z, sz, period)) /
                         (2 * e);
        const float cz = (Perlin(x + e, y, z, sy, period) - Perlin(x - e, y, z, sy, period) -
                          Perlin(x, y + e, z, seed, period) + Perlin(x, y - e, z, seed, period)) /
                         (2 * e);
        return float3(cx, cy, cz);
    }

    float Spectral(float x, float y, float z, uint seed, int period, bool harmonic)
    {
        const float tau = 6.28318530718f;
        float sum = 0, norm = 0;
        // No spatial lattice. Integer wavevectors for periodic synthesis, random
        // continuous wavevectors otherwise. Harmonic uses one fundamental + overtones.
        const uint fundamental = Hash(seed);
        [loop] for (int wave = 0; wave < 16; ++wave)
        {
            const uint h = harmonic ? fundamental : Hash(seed + uint(wave) * 1013u);
            float a = Unit(h) * 2 - 1;
            float b = Unit(Hash(h ^ 0x68bc21ebu)) * 2 - 1;
            float c = Unit(Hash(h ^ 0x02e5be93u)) * 2 - 1;
            if (period > 0)
            {
                a = floor(a * period + 0.5f) / period;
                b = floor(b * period + 0.5f) / period;
                c = floor(c * period + 0.5f) / period;
                if (a == 0 && b == 0 && c == 0)
                {
                    a = 1.0f / period;
                }
            }
            const float multiple = harmonic ? float(wave + 1) : 1.0f;
            const float amplitude = harmonic ? 1.0f / (multiple * multiple) : 1.0f;
            const float phase = tau * Unit(Hash(seed + uint(wave) * 7919u));
            sum += sin(tau * multiple * (a * x + b * y + c * z) + phase) * amplitude;
            norm += amplitude;
        }
        return sum / norm;
    }

    float CompactWavelet(float r2)
    {
        // Laplacian of the compact radial bump (1-r^2)^5 in 3D, rescaled.
        // Its integral is zero; both value and slope vanish at the support boundary.
        const float t = max(0.0f, 1.0f - r2);
        return t * t * t * (1.0f - (11.0f / 3.0f) * r2);
    }

    float KernelNoise(uint basis, float x, float y, float z, uint seed, int period, float direction, float anisotropy,
                      float kernelFrequency, float phase)
    {
        const float tau = 6.28318530718f;
        const int ix = int(floor(x)), iy = int(floor(y));
        const int iz = int(floor(z));
        float real = 0, imaginary = 0, spots = 0;
        const float stretch = clamp(anisotropy, 1.0f, 8.0f);
        const float frequency = clamp(kernelFrequency, 0.25f, 8.0f);
        [loop] for (int dz = -1; dz <= 1; ++dz)
        {
            [loop] for (int dy = -1; dy <= 1; ++dy)
            {
                [loop] for (int dx = -1; dx <= 1; ++dx)
                {
                    const uint cellHash =
                        HashXYZ(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed);
                    // Two stratified random impulses per cell: bounded work and no empty support holes.
                    [loop] for (int impulse = 0; impulse < 2; ++impulse)
                    {
                        const uint h = Hash(cellHash + uint(impulse) * 0x9e3779b9u);
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
                        if (basis == BasisWavelet)
                        {
                            real += weight * CompactWavelet(r2);
                            continue;
                        }
                        if (basis == BasisSparseConvolution)
                        {
                            real += weight * t * t * t;
                            continue;
                        }
                        if (basis == BasisSpot)
                        {
                            const float radius = 0.2f + 0.35f * Unit(Hash(h ^ 0x1b56c4e9u));
                            spots = max(spots, 1.0f - Smooth(radius * 0.6f, radius, sqrt(r2)));
                            continue;
                        }
                        const bool directed = basis == BasisAnisotropicGabor || basis == BasisPhasor;
                        const float angle = directed ? direction : tau * Unit(Hash(h ^ 0x1b56c4e9u));
                        const float along = a * cos(angle) + b * sin(angle);
                        const float across = -a * sin(angle) + b * cos(angle);
                        const float envelope =
                            exp(-4.0f * (along * along + across * across * (directed ? stretch : 1.0f) + c * c)) * t *
                            t;
                        const float theta = tau * frequency * along + phase + tau * Unit(Hash(h ^ 0x967a889bu));
                        if (basis == BasisLocalRandom)
                        {
                            // Local random-phase synthesis with multiple independent frequencies.
                            real += envelope * (cos(theta) + 0.5f * cos(theta * 1.93f + h % 17u));
                        }
                        else
                        {
                            real += envelope * cos(theta);
                            imaginary += envelope * sin(theta);
                        }
                    }
                }
            }
        }
        if (basis == BasisSpot)
        {
            return spots * 2 - 1;
        }
        if (basis == BasisPhasor)
        {
            return real / sqrt(max(real * real + imaginary * imaginary, 1e-12f));
        }
        return real / (1.0f + abs(real));
    }

    float BasisNoiseConfigured(uint basis, float x, float y, float z, uint seed, int period, float direction,
                               float anisotropy, float kernelFrequency, float phase)
    {
        switch (basis)
        {
            case BasisValue:
                return ValueNoise(x, y, z, seed, period);
            case BasisWorley:
            case BasisCellEdge:
                return LatticeBasisNoise(basis, float3(x, y, z), seed, period);
            case BasisWhite: {
                const int ix = int(floor(x)), iy = int(floor(y));
                const int iz = int(floor(z));
                const uint cell = HashXYZ(Wrap(ix, period), Wrap(iy, period), Wrap(iz, period), seed);
                return Unit(HashXYZ(int((x - ix) * 256), int((y - iy) * 256), int((z - iz) * 256), cell)) * 2 - 1;
            }
            case BasisSimplex:
                return Simplex(x, y, z, seed, period);
            case BasisVoronoi:
                return VoronoiCell(x, y, z, seed, period);
            case BasisCell:
                return Unit(HashXYZ(Wrap(int(floor(x)), period), Wrap(int(floor(y)), period),
                                    Wrap(int(floor(z)), period), seed)) *
                           2 -
                       1;
            case BasisGabor:
            case BasisAnisotropicGabor:
            case BasisWavelet:
            case BasisSparseConvolution:
            case BasisSpot:
            case BasisLocalRandom:
            case BasisPhasor:
                return KernelNoise(basis, x, y, z, seed, period, direction, anisotropy, kernelFrequency, phase);
            case BasisCurl: {
                const float3 curl = CurlVector(x, y, z, seed, period);
                return curl[2] / (1.0f + abs(curl[2]));
            }
            case BasisValueGradient:
                return 0.5f * (ValueNoise(x, y, z, seed, period) + Perlin(x, y, z, seed, period));
            case BasisChevron: {
                const float tau = 6.28318530718f;
                const float offset = Unit(Hash(seed));
                const float v = y + offset;
                const float chevronWave = abs((v - floor(v)) * 2 - 1);
                return sin(tau * (x + chevronWave + offset + phase / tau));
            }
            case BasisLatticeFree:
            case BasisHarmonic:
                return Spectral(x, y, z, seed, period, basis == BasisHarmonic);
            case BasisFlow:
                return Flow(x, y, z, seed, period, phase);
            case BasisRidged:
                return 1.0f - 2.0f * abs(Perlin(x, y, z, seed, period));
            default:
                return Perlin(x, y, z, seed, period);
        }
    }

    float BasisNoise(uint basis, float3 p, uint seed, int period)
    {
        return BasisNoiseConfigured(basis, p.x, p.y, p.z, seed, period, 0, 4, 2, 0);
    }

    float FractalSum(float3 p, uint basis, uint fractal, uint seed, bool tileable, NoiseSettings settings)
    {
        float frequency = max(settings.Frequency, 0.001);
        // CPU round uses half-away-from-zero; tile frequencies are positive.
        float lacunarity = tileable ? max(1.0, floor(settings.Lacunarity + 0.5)) : settings.Lacunarity;
        float amplitude = 1;
        float sum = 0;
        float norm = 0;
        float weight = 1;
        // Keep the full basis evaluator at one call site, including unfractalized noise.
        int octaveCount = fractal == FractalNone ? 1 : clamp(settings.Octaves, 1, 12);
        [loop] for (int octave = 0; octave < octaveCount; ++octave)
        {
            int period = tileable ? max(1, (int)floor(frequency + 0.5)) : 0;
            float scale = tileable ? (float)period : frequency;
            float n =
                BasisNoiseConfigured(basis, p.x * scale, p.y * scale, p.z * scale, Hash(seed + octave * 1013), period,
                                     settings.Direction, settings.Anisotropy, settings.KernelFrequency, settings.Phase);
            switch (fractal)
            {
                case FractalRidged:
                    n = settings.RidgeOffset - abs(n);
                    n *= n * weight;
                    weight = saturate(n * 2);
                    sum += n * amplitude;
                    break;
                case FractalMultifractal:
                    weight *= 1.0 + n * amplitude;
                    sum = weight - 1.0;
                    break;
                case FractalHeterogeneous: {
                    float signal = (n + settings.RidgeOffset) * amplitude;
                    sum += octave == 0 ? signal : signal * sum;
                    break;
                }
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
        if (fractal == FractalNone)
        {
            return saturate(sum * 0.5 + 0.5);
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
            case FractalMultifractal:
                return saturate(0.5 + 0.5 * sum / (1.0 + abs(sum)));
            case FractalHeterogeneous:
                return saturate(sum / (1.0 + abs(sum)));
            default:
                return saturate(value * 0.8 + 0.5);
        }
    }

    float Noise(float3 p, uint basis, uint fractal, uint seed, NoiseSettings settings)
    {
        bool tileable = settings.Tileable != 0;
        bool domainWarp = fractal == FractalDomainWarp;
        int sampleCount = domainWarp ? 3 : 1;
        float2 warp = 0;
        float result = 0;
        // Share the basis evaluator across the two warp samples and the final sample.
        [loop] for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            float3 position = p;
            uint sampleSeed = seed;
            uint sampleFractal = fractal;
            if (domainWarp)
            {
                sampleFractal = FractalFBm;
                if (sampleIndex == 0)
                {
                    position += float3(5.2, 1.3, 2.7);
                }
                else if (sampleIndex == 1)
                {
                    position += float3(1.7, 9.2, 4.1);
                    sampleSeed ^= 0x9e3779b9;
                }
                else
                {
                    float scale = settings.WarpStrength / max(settings.Frequency, 0.001);
                    position += float3(warp.x, warp.y, (warp.x - warp.y) * 0.5) * scale;
                }
            }
            result = FractalSum(position, basis, sampleFractal, sampleSeed, tileable, settings);
            if (sampleIndex == 0)
            {
                warp.x = result * 2 - 1;
            }
            if (sampleIndex == 1)
            {
                warp.y = result * 2 - 1;
            }
        }
        return result;
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
