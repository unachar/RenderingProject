#pragma once
#ifdef __cplusplus
#include <cmath>
#include <cstdint>
namespace StableSampling
{
using SampleUint = std::uint32_t;
#else
typedef uint SampleUint;
#endif

struct SamplePair { float x; float y; };

inline SampleUint SampleHash(SampleUint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

inline float SampleFraction(float value)
{
#ifdef __cplusplus
    return value - std::floor(value);
#else
    return frac(value);
#endif
}

// A screen-stable scramble and bounded R2/R4 sequences, independent of wall time.
// R4 joins emitter selection and triangle coordinates into one integration domain.
inline SamplePair SampleSequence(SampleUint x, SampleUint y, SampleUint index, SampleUint domain)
{
    SampleUint scramble = SampleHash(x ^ SampleHash(y + domain * 0x9e3779b9u));
    float phase = float((index & 4095u) + 1u);
    float frequencyX = .754877666f, frequencyY = .569840291f;
    // Emitter/triangle choice and barycentric coordinates are a single 4D
    // integration problem. Reusing an R2 pair with a shifted scramble would
    // correlate the selected triangle with the sampled part of its surface.
    if (domain == 1u) { frequencyX = .856674884f; frequencyY = .733891856f; }
    if (domain == 2u) { frequencyX = .628706722f; frequencyY = .538597257f; }
    SamplePair result;
    result.x = SampleFraction(float(scramble & 65535u) / 65536.0f + phase * frequencyX);
    result.y = SampleFraction(float(scramble >> 16) / 65536.0f + phase * frequencyY);
    return result;
}

inline SampleUint PixelSampleIndex(SampleUint frame, bool checkerboard)
{
    // A checkerboard pixel is traced only every other frame. Using frame directly
    // would skip half of its low-discrepancy sequence.
    return checkerboard ? frame >> 1 : frame;
}
#ifdef __cplusplus
}
#endif
