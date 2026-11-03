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
        if ((kind == Kind::LinearGradient || kind == Kind::RadialGradient || kind == Kind::Voronoi) && input == 0)
        {
            return SpecialInput::UV;
        }
        if (kind == Kind::BumpOffset && input == 3)
        {
            return SpecialInput::UV;
        }
        if (kind == Kind::RotateAboutAxis && input == 3)
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

    // ---- 2026-11 追加ノードの CPU 実装（HLSL 側の MG_* と同じ計算）
    inline float Frac(float v)
    {
        return v - floorf(v);
    }

    inline float Saturate(float v)
    {
        return std::clamp(v, 0.0f, 1.0f);
    }

    // Blend（Photoshop 系の描画モード）。a = Base, b = Blend（各成分 0〜1 想定）
    inline float BlendChannel(int mode, float a, float b)
    {
        switch (static_cast<MaterialBlendMode>(mode))
        {
            case MaterialBlendMode::Multiply:
                return a * b;
            case MaterialBlendMode::Screen:
                return 1.0f - (1.0f - a) * (1.0f - b);
            case MaterialBlendMode::Overlay:
                return a < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
            case MaterialBlendMode::SoftLight:
                return b < 0.5f ? 2.0f * a * b + a * a * (1.0f - 2.0f * b)
                                : sqrtf((std::max)(a, 0.0f)) * (2.0f * b - 1.0f) + 2.0f * a * (1.0f - b);
            case MaterialBlendMode::HardLight:
                return b < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
            case MaterialBlendMode::ColorDodge:
                return (std::min)(a / (std::max)(1.0f - b, 1e-5f), 1.0f);
            case MaterialBlendMode::ColorBurn:
                return 1.0f - (std::min)((1.0f - a) / (std::max)(b, 1e-5f), 1.0f);
            case MaterialBlendMode::LinearDodge:
                return a + b;
            case MaterialBlendMode::LinearBurn:
                return a + b - 1.0f;
            case MaterialBlendMode::Difference:
                return fabsf(a - b);
            case MaterialBlendMode::Darken:
                return (std::min)(a, b);
            case MaterialBlendMode::Lighten:
                return (std::max)(a, b);
            case MaterialBlendMode::Subtract:
                return a - b;
            default: // Normal
                return b;
        }
    }

    inline const char* BlendModeName(int mode)
    {
        static const char* names[] = {"Normal",      "Multiply",    "Screen",      "Overlay",    "Soft Light",
                                      "Hard Light",  "Color Dodge", "Color Burn",  "Linear Dodge", "Linear Burn",
                                      "Difference",  "Darken",      "Lighten",     "Subtract"};
        return mode >= 0 && mode < static_cast<int>(MaterialBlendMode::Count) ? names[mode] : names[0];
    }

    inline void RgbToHsv(const float* rgb, float* hsv)
    {
        const float maxValue = (std::max)({rgb[0], rgb[1], rgb[2]});
        const float minValue = (std::min)({rgb[0], rgb[1], rgb[2]});
        const float delta = maxValue - minValue;
        float hue = 0.0f;
        if (delta > 1e-6f)
        {
            if (maxValue == rgb[0])
            {
                hue = (rgb[1] - rgb[2]) / delta;
            }
            else if (maxValue == rgb[1])
            {
                hue = 2.0f + (rgb[2] - rgb[0]) / delta;
            }
            else
            {
                hue = 4.0f + (rgb[0] - rgb[1]) / delta;
            }
            hue = Frac(hue / 6.0f);
        }
        hsv[0] = hue;
        hsv[1] = maxValue > 1e-6f ? delta / maxValue : 0.0f;
        hsv[2] = maxValue;
    }

    inline void HsvToRgb(const float* hsv, float* rgb)
    {
        for (int i = 0; i < 3; ++i)
        {
            const float k = Frac(hsv[0] + (3.0f - static_cast<float>(i)) / 3.0f) * 6.0f; // r:k=h+1, g:h+2/3, b:h+1/3
            const float p = Saturate(fabsf(k - 3.0f) - 1.0f);
            rgb[i] = hsv[2] * (1.0f + (p - 1.0f) * hsv[1]);
        }
    }

    inline float HashCell(float x, float y, float k)
    {
        return Hash(x, y, k + 0.5f);
    }

    // Voronoi（F1 距離 / セル値）。uv はスケール済みの座標
    inline float Voronoi(float u, float v, float jitter, bool cellValue)
    {
        const float cx = floorf(u), cy = floorf(v);
        float best = 1e9f;
        float bestValue = 0.0f;
        for (int j = -1; j <= 1; ++j)
        {
            for (int i = -1; i <= 1; ++i)
            {
                const float gx = cx + static_cast<float>(i), gy = cy + static_cast<float>(j);
                const float px = gx + 0.5f + (HashCell(gx, gy, 1.0f) - 0.5f) * jitter;
                const float py = gy + 0.5f + (HashCell(gx, gy, 2.0f) - 0.5f) * jitter;
                const float dx = px - u, dy = py - v;
                const float d = dx * dx + dy * dy;
                if (d < best)
                {
                    best = d;
                    bestValue = HashCell(gx, gy, 3.0f);
                }
            }
        }
        return cellValue ? bestValue : sqrtf(best);
    }

    // 黒体放射の色（線形 RGB、Tanner Helland の近似を線形化）
    inline void BlackBody(float kelvin, float* rgb)
    {
        const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
        float r, g, b;
        if (t <= 66.0f)
        {
            r = 255.0f;
            g = 99.4708025861f * logf(t) - 161.1195681661f;
            b = t <= 19.0f ? 0.0f : 138.5177312231f * logf(t - 10.0f) - 305.0447927307f;
        }
        else
        {
            r = 329.698727446f * powf(t - 60.0f, -0.1332047592f);
            g = 288.1221695283f * powf(t - 60.0f, -0.0755148492f);
            b = 255.0f;
        }
        const float srgb[3] = {Saturate(r / 255.0f), Saturate(g / 255.0f), Saturate(b / 255.0f)};
        for (int i = 0; i < 3; ++i)
        {
            rgb[i] = srgb[i] <= 0.04045f ? srgb[i] / 12.92f : powf((srgb[i] + 0.055f) / 1.055f, 2.4f);
        }
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
               "max(radius, 1e-5); return saturate((1.0 - d) / max(1.0 - hardness, 1e-5)); }\n"
               // ---- 2026-11 追加ノード
               "float3 MG_Blend(int mode, float3 a, float3 b) {\n"
               "  float3 screen2 = 1-2*(1-a)*(1-b); float3 overlay = lerp(2*a*b, screen2, step(0.5, a)); float3 hard = lerp(2*a*b, screen2, step(0.5, b));\n"
               "  float3 soft = lerp(2*a*b + a*a*(1-2*b), sqrt(max(a,0))*(2*b-1) + 2*a*(1-b), step(0.5, b));\n"
               "  float3 dodge = min(a / max(1-b,1e-5), 1); float3 burn = 1 - min((1-a) / max(b,1e-5), 1);\n"
               "  switch (mode) { case 1: return a*b; case 2: return 1-(1-a)*(1-b); case 3: return overlay; case 4: return soft;\n"
               "    case 5: return hard; case 6: return dodge; case 7: return burn; case 8: return a+b; case 9: return a+b-1;\n"
               "    case 10: return abs(a-b); case 11: return min(a,b); case 12: return max(a,b); case 13: return a-b; default: return b; } }\n"
               "float3 MG_RgbToHsv(float3 c) { float mx = max(c.r,max(c.g,c.b)); float mn = min(c.r,min(c.g,c.b)); float d = mx-mn; float h = 0;\n"
               "  if (d > 1e-6) { h = mx == c.r ? (c.g-c.b)/d : (mx == c.g ? 2+(c.b-c.r)/d : 4+(c.r-c.g)/d); h = frac(h/6); }\n"
               "  return float3(h, mx > 1e-6 ? d/mx : 0, mx); }\n"
               "float3 MG_HsvToRgb(float3 c) { float3 k = frac(c.x + float3(1.0, 2.0/3.0, 1.0/3.0)) * 6; float3 p = saturate(abs(k-3)-1);\n"
               "  return c.z * (1 + (p-1) * c.y); }\n"
               "float3 MG_HueShift(float3 c, float shift) { float3 hsv = MG_RgbToHsv(c); hsv.x = frac(hsv.x + shift); return MG_HsvToRgb(hsv); }\n"
               "float MG_HashCell(float2 c, float k) { return MG_Hash(float3(c, k + 0.5)); }\n"
               "float MG_Voronoi(float2 uv, float jitter, int cellValue) { float2 c = floor(uv); float best = 1e9; float value = 0;\n"
               "  [unroll] for (int j = -1; j <= 1; ++j) [unroll] for (int i = -1; i <= 1; ++i) { float2 g = c + float2(i,j);\n"
               "    float2 p = g + 0.5 + (float2(MG_HashCell(g,1), MG_HashCell(g,2)) - 0.5) * jitter; float2 d = p - uv; float dd = dot(d,d);\n"
               "    if (dd < best) { best = dd; value = MG_HashCell(g,3); } }\n"
               "  return cellValue != 0 ? value : sqrt(best); }\n"
               "float3 MG_NormalFromHeight(float h, float2 uv, float strength) { float2 dh = float2(ddx(h), ddy(h)); float2 dx = ddx(uv); float2 dy = ddy(uv);\n"
               "  float det = dx.x*dy.y - dx.y*dy.x; if (abs(det) < 1e-12) return float3(0,0,1);\n"
               "  float dhdu = (dh.x*dy.y - dh.y*dx.y) / det; float dhdv = (dh.y*dx.x - dh.x*dy.x) / det;\n"
               "  return normalize(float3(-dhdu*strength, -dhdv*strength, 1)); }\n"
               "float3 MG_BlendAngleCorrectedNormals(float3 base, float3 detail) { float3 t = base + float3(0,0,1); float3 u = detail * float3(-1,-1,1);\n"
               "  return normalize(t * dot(t,u) / max(t.z,1e-5) - u); }\n"
               "float3 MG_RotateAboutAxis(float3 axis, float angle, float3 pivot, float3 position) { axis = normalize(axis);\n"
               "  float3 closest = pivot + axis * dot(axis, position - pivot); float3 u = position - closest; float3 v = cross(axis, u);\n"
               "  float a = angle * 6.28318531; return closest + u * cos(a) + v * sin(a) - position; }\n"
               "float3 MG_BlackBody(float kelvin) { float t = clamp(kelvin, 1000, 40000) / 100; float3 c;\n"
               "  if (t <= 66) c = float3(255, 99.4708025861*log(t) - 161.1195681661, t <= 19 ? 0 : 138.5177312231*log(t-10) - 305.0447927307);\n"
               "  else c = float3(329.698727446*pow(t-60,-0.1332047592), 288.1221695283*pow(t-60,-0.0755148492), 255);\n"
               "  c = saturate(c / 255); return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }\n"
               "float4 MG_UnpackNormal(float4 v) { float3 n = v.xyz * 2 - 1; if (v.z <= 0) n.z = sqrt(saturate(1 - dot(n.xy,n.xy))); return float4(n, 1); }\n";
    }
} // namespace MaterialGraphMath

