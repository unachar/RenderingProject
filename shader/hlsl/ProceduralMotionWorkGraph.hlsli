// Object motion feeds authoritative transforms, so all requested samples are evaluated
// in one graph dispatch and returned together before physics consumes this frame.
bool MotionFiniteDouble(double value)
{
    uint low;
    uint high;
    asuint(value, low, high);
    return (high & 0x7ff00000u) != 0x7ff00000u;
}

// HLSL transcendental intrinsics operate on float. Reduce the double time first,
// matching std::remainder's nearest, ties-to-even quotient for periods 1 and 8192.
double MotionRemainder(double value, int periodPower)
{
    uint low;
    uint high;
    asuint(value, low, high);
    uint magnitudeHigh = high & 0x7fffffffu;
    uint exponentBits = (magnitudeHigh >> 20) & 2047u;
    if (exponentBits == 2047u)
    {
        return double(asfloat(0x7fc00000u));
    }
    int exponent = int(exponentBits) - 1023;
    int shift = 52 - exponent + periodPower;
    if (shift > 53)
    {
        return value;
    }
    if (shift <= 0)
    {
        return 0;
    }
    uint64_t significand = (uint64_t(magnitudeHigh & 0x000fffffu) << 32) | uint64_t(low);
    significand |= uint64_t(1) << 52;
    uint64_t period = uint64_t(1) << uint(shift);
    uint64_t remainderBits = significand & (period - 1);
    uint64_t quotient = significand >> uint(shift);
    double scale = asdouble(0u, uint(exponent - 52 + 1023) << 20);
    double result = double(remainderBits) * scale;
    uint64_t halfPeriod = period >> 1;
    if (remainderBits > halfPeriod || (remainderBits == halfPeriod && (quotient & 1) != 0))
    {
        result -= double(uint(1) << uint(periodPower));
    }
    if ((high & 0x80000000u) != 0)
    {
        result = -result;
    }
    return result;
}

float MotionNoise(float coordinate, float row, uint seed)
{
    return clamp(LatticeNoise(coordinate, row, seed, true), -1.0, 1.0);
}

void GenerateObjectMotion(uint index)
{
    uint address = index * 32;
    double time = asdouble(SourcePixels.Load(address), SourcePixels.Load(address + 4));
    float speed = asfloat(SourcePixels.Load(address + 8));
    uint method = SourcePixels.Load(address + 12);
    float3 amplitude = asfloat(SourcePixels.Load3(address + 16));
    uint seed = SourcePixels.Load(address + 28);
    float3 offset = 0;
    if (MotionFiniteDouble(time) && isfinite(speed))
    {
        double cycles = time * double(speed);
        float phase = float(MotionRemainder(cycles, 0) * double(2.0) * double(kPi));
        if (method == 0)
        {
            offset = amplitude * sin(phase);
        }
        else if (method == 1)
        {
            offset = amplitude * float3(cos(phase) - 1, sin(phase), sin(phase));
        }
        else if (method == 2)
        {
            float coordinate = float(MotionRemainder(cycles, 13));
            if (isfinite(coordinate))
            {
                offset.x = amplitude.x * (MotionNoise(coordinate, 11.2, seed) - MotionNoise(0, 11.2, seed));
                offset.y = amplitude.y * (MotionNoise(coordinate, 37.9, seed) - MotionNoise(0, 37.9, seed));
                offset.z = amplitude.z * (MotionNoise(coordinate, 93.1, seed) - MotionNoise(0, 93.1, seed));
            }
            else
            {
                offset = -amplitude *
                         float3(MotionNoise(0, 11.2, seed), MotionNoise(0, 37.9, seed), MotionNoise(0, 93.1, seed));
            }
        }
    }
    Output.Store3(index * 12, asuint(offset));
}
