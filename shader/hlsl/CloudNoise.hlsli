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
