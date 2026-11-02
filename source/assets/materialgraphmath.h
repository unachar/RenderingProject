#pragma once
// ============================================================================
// マテリアルグラフの演算定義（HLSL 生成と CPU プレビューで共有する）
//  ・単項 / 二項演算の CPU 実装と HLSL 関数名
//  ・入力ピンが未接続のときの特別な既定値（UV / Time / World Position）
//  ・Noise / Rotator / Sphere Mask の HLSL ヘルパー
// ============================================================================
#include "materialgraph.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace MaterialGraphMath
{
    using Kind = MaterialNodeKind;

    inline bool IsUnary(Kind kind)
    {
        return kind >= Kind::Abs && kind <= Kind::DDY;
    }

    inline bool IsBinary(Kind kind)
    {
        return kind >= Kind::Power && kind <= Kind::Arctangent2;
    }

    inline bool IsConstantNode(Kind kind)
    {
        return kind == Kind::Constant || kind == Kind::ScalarParameter || kind == Kind::VectorParameter ||
               kind == Kind::ColorParameter || kind == Kind::Constant2Vector || kind == Kind::Constant3Vector ||
               kind == Kind::Constant4Vector;
    }

    enum class SpecialInput
    {
        None,
        UV,
        Time,
        WorldPosition
    };

    // 未接続の入力が「定数」ではなく、ピクセル / 時間の値を使うピン（UE と同じ挙動）
    inline SpecialInput DefaultInput(Kind kind, size_t input)
    {
        if ((kind == Kind::TextureSample || kind == Kind::Panner || kind == Kind::Rotator) && input == 0)
        {
            return SpecialInput::UV;
        }
        if ((kind == Kind::Panner || kind == Kind::Rotator) && input == 1)
        {
            return SpecialInput::Time;
        }
        if (kind == Kind::Noise && input == 0)
        {
            return SpecialInput::WorldPosition;
        }
        return SpecialInput::None;
    }

    inline float Unary(Kind kind, float x)
    {
        switch (kind)
        {
            case Kind::Abs:
                return fabsf(x);
            case Kind::Ceil:
                return ceilf(x);
            case Kind::Floor:
                return floorf(x);
            case Kind::Frac:
                return x - floorf(x);
            case Kind::Round:
                return roundf(x);
            case Kind::Truncate:
                return truncf(x);
            case Kind::Sign:
                return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f);
            case Kind::OneMinus:
                return 1.0f - x;
            case Kind::Saturate:
                return std::clamp(x, 0.0f, 1.0f);
            case Kind::SquareRoot:
                return sqrtf(x);
            case Kind::Sine:
                return sinf(x);
            case Kind::Cosine:
                return cosf(x);
            case Kind::Tangent:
                return tanf(x);
            case Kind::Arcsine:
                return asinf(x);
            case Kind::Arccosine:
                return acosf(x);
            case Kind::Arctangent:
                return atanf(x);
            case Kind::Exponential:
                return expf(x);
            case Kind::Logarithm2:
                return log2f(x);
            default: // DDX / DDY は CPU では 0
                return 0.0f;
        }
    }

    inline float Binary(Kind kind, float a, float b)
    {
        switch (kind)
        {
            case Kind::Power:
                return powf(a, b);
            case Kind::Min:
                return (std::min)(a, b);
            case Kind::Max:
                return (std::max)(a, b);
            case Kind::Fmod:
                return b == 0.0f ? 0.0f : fmodf(a, b);
            case Kind::Step: // UE の Step(Y, X) = X >= Y ? 1 : 0（HLSL の step(y, x) と同じ）
                return b >= a ? 1.0f : 0.0f;
            case Kind::Arctangent2:
                return atan2f(a, b);
            default:
                return 0.0f;
        }
    }

    inline std::string UnaryHlsl(Kind kind, const std::string& x)
    {
        switch (kind)
        {
            case Kind::Abs:
                return "abs(" + x + ")";
            case Kind::Ceil:
                return "ceil(" + x + ")";
            case Kind::Floor:
                return "floor(" + x + ")";
            case Kind::Frac:
                return "frac(" + x + ")";
            case Kind::Round:
                return "round(" + x + ")";
            case Kind::Truncate:
                return "trunc(" + x + ")";
            case Kind::Sign:
                return "sign(" + x + ")";
            case Kind::OneMinus:
                return "(1-(" + x + "))";
            case Kind::Saturate:
                return "saturate(" + x + ")";
            case Kind::SquareRoot:
                return "sqrt(" + x + ")";
            case Kind::Sine:
                return "sin(" + x + ")";
            case Kind::Cosine:
                return "cos(" + x + ")";
            case Kind::Tangent:
                return "tan(" + x + ")";
            case Kind::Arcsine:
                return "asin(" + x + ")";
            case Kind::Arccosine:
                return "acos(" + x + ")";
            case Kind::Arctangent:
                return "atan(" + x + ")";
            case Kind::Exponential:
                return "exp(" + x + ")";
            case Kind::Logarithm2:
                return "log2(" + x + ")";
            case Kind::DDX:
                return "ddx(" + x + ")";
            case Kind::DDY:
                return "ddy(" + x + ")";
            default:
                return x;
        }
    }

    inline std::string BinaryHlsl(Kind kind, const std::string& a, const std::string& b)
    {
        switch (kind)
        {
            case Kind::Power:
                return "pow(" + a + "," + b + ")";
            case Kind::Min:
                return "min(" + a + "," + b + ")";
            case Kind::Max:
                return "max(" + a + "," + b + ")";
            case Kind::Fmod:
                return "fmod(" + a + "," + b + ")";
            case Kind::Step:
                return "step(" + a + "," + b + ")";
            case Kind::Arctangent2:
                return "atan2(" + a + "," + b + ")";
            default:
                return a;
        }
    }

    // ---- Noise（値ノイズ）。HLSL 側の MG_Noise と同じ計算
    inline float Hash(float x, float y, float z)
    {
        auto frac = [](float v) {
            return v - floorf(v);
        };
        x = frac(x * 0.3183099f + 0.1f) * 17.0f;
        y = frac(y * 0.3183099f + 0.1f) * 17.0f;
        z = frac(z * 0.3183099f + 0.1f) * 17.0f;
        return frac(x * y * z * (x + y + z));
    }

    inline float Noise(float x, float y, float z)
    {
        const float ix = floorf(x), iy = floorf(y), iz = floorf(z);
        float fx = x - ix, fy = y - iy, fz = z - iz;
        fx = fx * fx * (3.0f - 2.0f * fx);
        fy = fy * fy * (3.0f - 2.0f * fy);
        fz = fz * fz * (3.0f - 2.0f * fz);
        auto lerp = [](float a, float b, float t) {
            return a + (b - a) * t;
        };
        auto h = [&](float dx, float dy, float dz) {
            return Hash(ix + dx, iy + dy, iz + dz);
        };
        return lerp(lerp(lerp(h(0, 0, 0), h(1, 0, 0), fx), lerp(h(0, 1, 0), h(1, 1, 0), fx), fy),
                    lerp(lerp(h(0, 0, 1), h(1, 0, 1), fx), lerp(h(0, 1, 1), h(1, 1, 1), fx), fy), fz);
    }

    inline float SphereMask(float distance, float radius, float hardness)
    {
        const float d = distance / (std::max)(radius, 1e-5f);
        return std::clamp((1.0f - d) / (std::max)(1.0f - hardness, 1e-5f), 0.0f, 1.0f);
    }

    inline const char* HlslHelpers()
    {
        return "float MG_Hash(float3 p) { p = frac(p * 0.3183099 + 0.1) * 17.0; return frac(p.x * p.y * p.z * (p.x + "
               "p.y + p.z)); }\n"
               "float MG_Noise(float3 x) { float3 i = floor(x); float3 f = frac(x); f = f * f * (3.0 - 2.0 * f);\n"
               "  return lerp(lerp(lerp(MG_Hash(i), MG_Hash(i + float3(1,0,0)), f.x), lerp(MG_Hash(i + float3(0,1,0)), "
               "MG_Hash(i + float3(1,1,0)), f.x), f.y),\n"
               "              lerp(lerp(MG_Hash(i + float3(0,0,1)), MG_Hash(i + float3(1,0,1)), f.x), lerp(MG_Hash(i + "
               "float3(0,1,1)), MG_Hash(i + float3(1,1,1)), f.x), f.y), f.z); }\n"
               "float2 MG_Rotate(float2 uv, float2 center, float angle) { float s = sin(angle); float c = cos(angle); "
               "float2 d = uv - center; return center + float2(d.x * c - d.y * s, d.x * s + d.y * c); }\n"
               "float MG_SphereMask(float3 a, float3 b, float radius, float hardness) { float d = distance(a, b) / "
               "max(radius, 1e-5); return saturate((1.0 - d) / max(1.0 - hardness, 1e-5)); }\n";
    }
} // namespace MaterialGraphMath
