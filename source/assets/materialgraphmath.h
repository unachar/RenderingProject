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
        WorldPosition,
        // ---- 2026-11 追加ノードの既定入力（未接続のとき、定数ではなく画素 / ライトの値を使うピン）
        WorldNormal,    // ジオメトリの法線（ワールド）
        CameraVector,   // 表面 → カメラ（ワールド）
        LightDirection, // 表面 → メインライト（ワールド）
        Tangent,        // 接線（ワールド。スキニング後の位置 / UV の微分から求めるので変形後も正しい）
        ViewNormal,     // ビュー空間の法線（MatCap）
        NdotV,          // dot(法線, 視線)
        ScreenUV,       // 画面上の 0〜1 座標
        Curvature,      // 画素の曲率（正 = 凸）
        AudioTime       // 音声クロック（秒）。Beat Phase の既定
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
        if ((kind == Kind::LinearGradient || kind == Kind::RadialGradient || kind == Kind::Voronoi ||
             kind == Kind::ProceduralNoise || kind == Kind::FractalNoise || IsConvertTextureNode(kind)) &&
            input == 0)
        {
            return SpecialInput::UV;
        }
        // 立体テクスチャの Position（未接続 = 位置を 0〜1 にしたもの。ノード側で * 0.5 + 0.5 する）
        if ((kind == Kind::SolidTexture || kind == Kind::SolidPattern || kind == Kind::SolidCoordinate) && input == 0)
        {
            return SpecialInput::WorldPosition;
        }
        if (kind == Kind::ConvertToSolid && input == 0)
        {
            return SpecialInput::UV; // 未変換：UV で評価した入力（= テクスチャに作る 2D の値）
        }
        if (kind == Kind::ConvertToSolid && input == 1)
        {
            return SpecialInput::WorldPosition;
        }
        if (kind == Kind::TriplanarTexture && input == 0)
        {
            return SpecialInput::WorldPosition;
        }
        if (kind == Kind::ConvertToTexture && input == 1)
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
        switch (kind)
        {
            // ---- 2026-11 追加ノード
            case Kind::ToonLightRamp:
                return input == 1 ? SpecialInput::WorldNormal : input == 2 ? SpecialInput::LightDirection : SpecialInput::None;
            case Kind::FaceSdfShadow:
                return input == 0 ? SpecialInput::UV : input == 1 ? SpecialInput::LightDirection : SpecialInput::None;
            case Kind::HairAnisotropicHighlight:
                return input == 0   ? SpecialInput::Tangent
                       : input == 1 ? SpecialInput::WorldNormal
                       : input == 2 ? SpecialInput::CameraVector
                       : input == 3 ? SpecialInput::LightDirection
                                    : SpecialInput::None;
            case Kind::DirectionalRimLight:
                return input == 0 ? SpecialInput::WorldNormal : input == 1 ? SpecialInput::CameraVector
                       : input == 2 ? SpecialInput::LightDirection : SpecialInput::None;
            case Kind::MatCap:
                return input == 0 ? SpecialInput::ViewNormal : SpecialInput::None;
            case Kind::MekakureMask:
                return input == 5 ? SpecialInput::WorldNormal : input == 6 ? SpecialInput::CameraVector : SpecialInput::None;
            case Kind::EyeHighlight:
                return input == 0 ? SpecialInput::UV : input == 1 ? SpecialInput::CameraVector
                       : input == 2 ? SpecialInput::LightDirection : SpecialInput::None;
            case Kind::TextureParameter2D:
            case Kind::PackedTextureChannels:
            case Kind::ThicknessMap:
                return input == 0 ? SpecialInput::UV : SpecialInput::None;
            case Kind::FlipbookUV:
                return input == 0 ? SpecialInput::UV : input == 5 ? SpecialInput::Time : SpecialInput::None;
            case Kind::FlowMap:
                return input == 0 ? SpecialInput::UV : input == 1 ? SpecialInput::Time : SpecialInput::None;
            case Kind::TriplanarNormalCorrected:
                return input == 0 ? SpecialInput::WorldPosition : input == 1 ? SpecialInput::WorldNormal : SpecialInput::None;
            case Kind::CurvatureMask:
                return input == 0 ? SpecialInput::Curvature : SpecialInput::None;
            case Kind::ThinFilmInterference:
                return input == 2 ? SpecialInput::NdotV : SpecialInput::None;
            case Kind::AnisotropyDirection:
                return input == 0 ? SpecialInput::Tangent : input == 4 ? SpecialInput::WorldNormal : SpecialInput::None;
            case Kind::SceneDepth:
            case Kind::SceneColor:
                return input == 0 ? SpecialInput::ScreenUV : SpecialInput::None;
            case Kind::BeatPhase:
                return input == 3 ? SpecialInput::AudioTime : SpecialInput::None;
            default:
                return SpecialInput::None;
        }
    }

    // ---- プロシージャルスタジオのノイズ（名前は ProcStudio::Basis / FractalType の並び）
    inline constexpr int kNoiseBasisCount = 22;
    inline constexpr int kNoiseFractalCount = 9;
    inline const char* NoiseBasisName(int index)
    {
        static const char* names[kNoiseBasisCount] = {
            "Perlin",  "Value",          "Worley",     "Cell Edge",   "White",          "Simplex",
            "Voronoi", "Cell",           "Gabor",      "Wavelet",     "Curl",           "Value Gradient",
            "Sparse Convolution",        "Spot",       "Local Random", "Chevron",       "Lattice Free",
            "Phasor",  "Anisotropic Gabor",            "Harmonic",    "Flow",           "Ridged"};
        return names[std::clamp(index, 0, kNoiseBasisCount - 1)];
    }
    inline const char* NoiseFractalName(int index)
    {
        static const char* names[kNoiseFractalCount] = {"fBm",         "Ridged",      "Billow",
                                                        "Turbulence",  "Hybrid Multifractal", "Domain Warp",
                                                        "None",        "Multifractal", "Heterogeneous Multifractal"};
        return names[std::clamp(index, 0, kNoiseFractalCount - 1)];
    }
    inline const char* NoiseCombineModeName(int index)
    {
        static const char* names[] = {"Mix",    "Add", "Subtract", "Multiply",   "Screen",
                                      "Overlay", "Min", "Max",      "Difference", "Height Blend"};
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(MaterialNoiseCombineMode::Count));
        return names[std::clamp(index, 0, static_cast<int>(MaterialNoiseCombineMode::Count) - 1)];
    }

    // Noise Combine（HLSL 側の MG_NoiseCombine と同じ計算）
    //  Mix 以外は「A に B を合成した結果」へ Factor（不透明度）で寄せる。Height Blend は Factor = 境界の幅
    inline float NoiseCombine(int mode, float a, float b, float factor, bool clampResult)
    {
        const auto saturate = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
        float result = a;
        switch (static_cast<MaterialNoiseCombineMode>(mode))
        {
            case MaterialNoiseCombineMode::Mix:
                result = a + (b - a) * factor;
                break;
            case MaterialNoiseCombineMode::HeightBlend: {
                const float width = (std::max)(factor, 1e-4f);
                float t = saturate((b - a) / width * 0.5f + 0.5f);
                t = t * t * (3.0f - 2.0f * t);
                result = a + (b - a) * t;
                break;
            }
            default: {
                float op = a;
                switch (static_cast<MaterialNoiseCombineMode>(mode))
                {
                    case MaterialNoiseCombineMode::Add:
                        op = a + b;
                        break;
                    case MaterialNoiseCombineMode::Subtract:
                        op = a - b;
                        break;
                    case MaterialNoiseCombineMode::Multiply:
                        op = a * b;
                        break;
                    case MaterialNoiseCombineMode::Screen:
                        op = 1.0f - (1.0f - a) * (1.0f - b);
                        break;
                    case MaterialNoiseCombineMode::Overlay:
                        op = a < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
                        break;
                    case MaterialNoiseCombineMode::Min:
                        op = (std::min)(a, b);
                        break;
                    case MaterialNoiseCombineMode::Max:
                        op = (std::max)(a, b);
                        break;
                    case MaterialNoiseCombineMode::Difference:
                        op = std::fabs(a - b);
                        break;
                    default:
                        break;
                }
                result = a + (op - a) * factor;
                break;
            }
        }
        return clampResult ? saturate(result) : result;
    }

    // ---- Convert To Normal / Height（プロシージャルスタジオ ImageMaps と同じ式）
    inline constexpr int kConvertChannelCount = 7;
    inline const char* ConvertChannelName(int index)
    {
        static const char* names[kConvertChannelCount] = {"輝度", "平均", "R", "G", "B", "A", "最大"};
        return names[std::clamp(index, 0, kConvertChannelCount - 1)];
    }
    // 格納値（8 ビット画像なら sRGB 値）からチャンネルを取り出す
    inline float ConvertChannel(const float* c, int channel)
    {
        switch (channel)
        {
            case 1:
                return (c[0] + c[1] + c[2]) / 3.0f;
            case 2:
                return c[0];
            case 3:
                return c[1];
            case 4:
                return c[2];
            case 5:
                return c[3];
            case 6:
                return (std::max)({c[0], c[1], c[2]});
            default:
                return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        }
    }
    // 高さ：反転 → (v - 0.5) * Contrast + 0.5 + Bias → 0..1
    inline float ConvertHeight(const float* c, int channel, bool invert, float contrast, float bias)
    {
        float v = std::clamp(ConvertChannel(c, channel), 0.0f, 1.0f);
        v = invert ? 1.0f - v : v;
        return std::clamp((v - 0.5f) * contrast + 0.5f + bias, 0.0f, 1.0f);
    }

    // ---- ソリッドテクスチャ
    inline const char* SolidKindName(int index)
    {
        static const char* names[] = {"フラクタルノイズ", "大理石 (Marble)", "木目 (Wood)", "御影石 (Granite)",
                                      "ボロノイ",         "フラクタル雲"};
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(MaterialSolidKind::Count));
        return names[std::clamp(index, 0, static_cast<int>(MaterialSolidKind::Count) - 1)];
    }
    // 3 色ランプ（スタジオの TextureColor の既定配色）
    inline void ThreeColorRamp(float t, const float* a, const float* b, const float* c, float* out)
    {
        for (int i = 0; i < 3; ++i)
        {
            out[i] = t < 0.5f ? a[i] + (b[i] - a[i]) * (t * 2.0f) : b[i] + (c[i] - b[i]) * ((t - 0.5f) * 2.0f);
        }
    }

    inline const char* SolidConvertMethodName(int index)
    {
        static const char* names[] = {"ストキャスティック（繰り返しなし）", "トライプラナー", "ボリューム（スタジオ互換）"};
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(MaterialSolidConvertMethod::Count));
        return names[std::clamp(index, 0, static_cast<int>(MaterialSolidConvertMethod::Count) - 1)];
    }

    // ---- オブジェクト / カメラのノードの選択肢
    inline const char* ObjectBoundsOutputName(int index)
    {
        static const char* names[] = {"大きさ", "中心", "最小", "最大"};
        return names[std::clamp(index, 0, 3)];
    }
    inline const char* TransformVectorModeName(int index)
    {
        static const char* names[] = {"ローカル → ワールド", "ワールド → ローカル", "ワールド → ビュー",
                                      "ビュー → ワールド"};
        return names[std::clamp(index, 0, 3)];
    }
    inline const char* TransformPositionModeName(int index)
    {
        static const char* names[] = {"ローカル → ワールド", "ワールド → ローカル"};
        return names[std::clamp(index, 0, 1)];
    }
    inline const char* HeightGradientSpaceName(int index)
    {
        static const char* names[] = {"ワールド Y", "ローカル Y", "範囲内 0〜1"};
        return names[std::clamp(index, 0, 2)];
    }
    // 4x4 の順序ディザの閾値（0〜1）
    inline float BayerThreshold(int x, int y)
    {
        static const int table[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        return (table[((y & 3) << 2) | (x & 3)] + 0.5f) / 16.0f;
    }

    // ---- ヘックス タイル（Heitz & Neyret 2018 の三角格子）。MaterialGraphVM.hlsli の MGVM_HexWeights と同じ
    struct HexTiles
    {
        int Vertex[3][2];
        float Weight[3];
    };
    inline HexTiles HexGrid(float u, float v)
    {
        const float sx = u * 3.4641016f;
        const float sy = v * 3.4641016f;
        const float kx = sx;
        const float ky = -0.57735027f * sx + 1.15470054f * sy;
        const int bx = static_cast<int>(floorf(kx));
        const int by = static_cast<int>(floorf(ky));
        const float tx = kx - bx;
        const float ty = ky - by;
        const float tz = 1.0f - tx - ty;
        const float s = tz < 0.0f ? 1.0f : 0.0f;
        const float s2 = 2.0f * s - 1.0f;
        const int is = static_cast<int>(s);
        HexTiles tiles;
        tiles.Weight[0] = -tz * s2;
        tiles.Weight[1] = s - ty * s2;
        tiles.Weight[2] = s - tx * s2;
        tiles.Vertex[0][0] = bx + is;
        tiles.Vertex[0][1] = by + is;
        tiles.Vertex[1][0] = bx + is;
        tiles.Vertex[1][1] = by + 1 - is;
        tiles.Vertex[2][0] = bx + 1 - is;
        tiles.Vertex[2][1] = by + is;
        float total = 0.0f;
        for (float& w : tiles.Weight)
        {
            w = powf((std::max)(w, 0.0f), 4.0f); // 境界を締めて、混ぜた部分のコントラスト低下を抑える
            total += w;
        }
        for (float& w : tiles.Weight)
        {
            w /= (std::max)(total, 1e-6f);
        }
        return tiles;
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
               "float MG_NoiseCombine(int mode, float a, float b, float factor, int clampResult) { float result = a;\n"
               "  if (mode == 0) { result = lerp(a, b, factor); }\n"
               "  else if (mode == 9) { float t = saturate((b - a) / max(factor, 1e-4) * 0.5 + 0.5); t = t * t * (3 - 2 * t); result = lerp(a, b, t); }\n"
               "  else { float op = a; if (mode == 1) op = a + b; else if (mode == 2) op = a - b; else if (mode == 3) op = a * b;\n"
               "    else if (mode == 4) op = 1 - (1 - a) * (1 - b); else if (mode == 5) op = a < 0.5 ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b);\n"
               "    else if (mode == 6) op = min(a, b); else if (mode == 7) op = max(a, b); else if (mode == 8) op = abs(a - b);\n"
               "    result = lerp(a, op, factor); }\n"
               "  return clampResult != 0 ? saturate(result) : result; }\n"
               "float MG_ConvertHeight(float4 c, int channel, int invert, float contrast, float bias) {\n"
               "  float v = channel == 1 ? (c.r + c.g + c.b) / 3 : channel == 2 ? c.r : channel == 3 ? c.g : channel == 4 ? c.b :\n"
               "            channel == 5 ? c.a : channel == 6 ? max(c.r, max(c.g, c.b)) : dot(c.rgb, float3(0.2126, 0.7152, 0.0722));\n"
               "  v = saturate(v); v = invert != 0 ? 1 - v : v; return saturate((v - 0.5) * contrast + 0.5 + bias); }\n"
               "float MG_ConvertTap(Texture2D<float4> t, SamplerState s, float2 uv, int channel, int invert, float contrast, float bias) {\n"
               "  return MG_ConvertHeight(t.SampleLevel(s, uv, 0), channel, invert, contrast, bias); }\n"
               "float3 MG_ConvertNormal(Texture2D<float4> t, SamplerState s, float2 uv, int channel, int invert, float contrast, float bias,\n"
               "  float strength, float radius, int sobel) { float r = max(radius, 1e-5); float dx, dy;\n"
               "  float l = MG_ConvertTap(t, s, uv + float2(-r, 0), channel, invert, contrast, bias); float rr = MG_ConvertTap(t, s, uv + float2(r, 0), channel, invert, contrast, bias);\n"
               "  float u = MG_ConvertTap(t, s, uv + float2(0, -r), channel, invert, contrast, bias); float d = MG_ConvertTap(t, s, uv + float2(0, r), channel, invert, contrast, bias);\n"
               "  if (sobel != 0) {\n"
               "    float lu = MG_ConvertTap(t, s, uv + float2(-r, -r), channel, invert, contrast, bias); float ru = MG_ConvertTap(t, s, uv + float2(r, -r), channel, invert, contrast, bias);\n"
               "    float ld = MG_ConvertTap(t, s, uv + float2(-r, r), channel, invert, contrast, bias); float rd = MG_ConvertTap(t, s, uv + float2(r, r), channel, invert, contrast, bias);\n"
               "    dx = ((ru - lu) + (rr - l) * 2 + (rd - ld)) * 0.25; dy = ((ld - lu) + (d - u) * 2 + (rd - ru)) * 0.25; }\n"
               "  else { dx = rr - l; dy = d - u; }\n"
               "  return normalize(float3(-dx * strength, -dy * strength, 1)); }\n"
               "float3 MG_ThreeColorRamp(float t, float3 a, float3 b, float3 c) { return t < 0.5 ? lerp(a, b, t * 2) : lerp(b, c, (t - 0.5) * 2); }\n"
               "float4 MG_SolidTriplanar(Texture2D<float4> t, SamplerState s, float3 p, float3 n, float sharpness) {\n"
               "  float3 w = pow(abs(n) + 1e-5, max(sharpness, 0.01)); w /= dot(w, 1);\n"
               "  return t.Sample(s, p.yz) * w.x + t.Sample(s, p.xz) * w.y + t.Sample(s, p.xy) * w.z; }\n"
               "float MG_SlopeMask(float3 n, float3 up, float threshold, float softness) { float s = max(softness, 1e-4);\n"
               "  float t = saturate((dot(n, normalize(up)) - (threshold - s)) / (2 * s)); return t * t * (3 - 2 * t); }\n"
               "float MG_HeightLerpAlpha(float height, float phase, float contrast) {\n"
               "  float x = saturate(height + phase * 2 - 1); return saturate(lerp(-contrast, 1 + contrast, x)); }\n"
               "float3 MG_ColorAdjust(float3 c, float brightness, float contrast, float saturation) {\n"
               "  c = (c * brightness - 0.5) * contrast + 0.5; float l = dot(c, float3(0.2126, 0.7152, 0.0722)); return lerp(l.xxx, c, saturation); }\n"
               "float MG_Bayer(float2 pixel) { uint2 p = (uint2)pixel & 3u; uint t[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};\n"
               "  return (t[(p.y << 2) | p.x] + 0.5) / 16.0; }\n"
               "float3 MG_BlackBody(float kelvin) { float t = clamp(kelvin, 1000, 40000) / 100; float3 c;\n"
               "  if (t <= 66) c = float3(255, 99.4708025861*log(t) - 161.1195681661, t <= 19 ? 0 : 138.5177312231*log(t-10) - 305.0447927307);\n"
               "  else c = float3(329.698727446*pow(t-60,-0.1332047592), 288.1221695283*pow(t-60,-0.0755148492), 255);\n"
               "  c = saturate(c / 255); return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }\n"
               "float4 MG_UnpackNormal(float4 v) { float3 n = v.xyz * 2 - 1; if (v.z <= 0) n.z = sqrt(saturate(1 - dot(n.xy,n.xy))); return float4(n, 1); }\n";
    }
} // namespace MaterialGraphMath





