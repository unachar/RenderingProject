#pragma once
// Execute the production HLSL helpers unchanged with a small test-only vector API.
#include "skinning_hlsl_math_shim.h"
struct float2
{
    float x, y;
    float2(float a, float b) : x(a), y(b) {}
};
inline float3 operator*(float3 a, float3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
inline float3 operator/(float3 a, float b) { return a*(1/b); }
inline float3 lerp(float3 a, float3 b, float t) { return a*(1-t)+b*t; }
inline float lerp(float a, float b, float t) { return a*(1-t)+b*t; }
inline float saturate(float x) { return std::clamp(x,0.0f,1.0f); }
inline float smoothstep(float a, float b, float x)
{
    const float t=saturate((x-a)/(b-a));
    return t*t*(3-2*t);
}
using std::min;
using std::pow;
