#pragma once
// Test-only HLSL vector subset. Production shader math is included unchanged.
#include <algorithm>
#include <cmath>
struct float3
{
    float x, y, z;
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct float4
{
    float x, y, z, w;
    float4() = default;
    float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    float4(float3 v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
};
inline float3 operator+(float3 a, float3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline float3 operator-(float3 a, float3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline float3 operator*(float a, float3 b) { return {a*b.x,a*b.y,a*b.z}; }
inline float3 operator*(float3 a, float b) { return b*a; }
inline float4 operator+(float4 a, float4 b) { return {a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w}; }
inline float4 operator-(float4 a, float4 b) { return {a.x-b.x,a.y-b.y,a.z-b.z,a.w-b.w}; }
inline float4 operator-(float4 a) { return {-a.x,-a.y,-a.z,-a.w}; }
inline float4 operator*(float a, float4 b) { return {a*b.x,a*b.y,a*b.z,a*b.w}; }
inline float4 operator*(float4 a, float b) { return b*a; }
inline float4 operator/(float4 a, float b) { return a*(1/b); }
inline float dot(float3 a, float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline float dot(float4 a, float4 b) { return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w; }
inline float3 cross(float3 a, float3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline float4 lerp(float4 a, float4 b, float t) { return a*(1-t)+b*t; }
inline float rsqrt(float x) { return 1/std::sqrt(x); }
inline float clamp(float x, float a, float b) { return std::clamp(x,a,b); }
using std::max;
using std::sqrt;
using std::sin;
using std::acos;
using std::abs;
using std::isfinite;
struct float3x3
{
    float rows[3][3];
    float3x3(float a,float b,float c,float d,float e,float f,float g,float h,float i)
        : rows{{a,b,c},{d,e,f},{g,h,i}} {}
    const float* operator[](int i) const { return rows[i]; }
};
struct float4x4
{
    float rows[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    const float* operator[](int i) const { return rows[i]; }
    float* operator[](int i) { return rows[i]; }
    explicit operator float3x3() const
    { return {rows[0][0],rows[0][1],rows[0][2],rows[1][0],rows[1][1],rows[1][2],rows[2][0],rows[2][1],rows[2][2]}; }
};
inline float3 mul(float3 v, float3x3 m)
{
    return {v.x*m[0][0]+v.y*m[1][0]+v.z*m[2][0],
            v.x*m[0][1]+v.y*m[1][1]+v.z*m[2][1],
            v.x*m[0][2]+v.y*m[1][2]+v.z*m[2][2]};
}
