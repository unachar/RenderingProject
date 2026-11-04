#pragma once
// ============================================================================
// マテリアルグラフ 追加ノード 33 種のメタデータと「純粋な計算」（2026-11）
//  ・ImGui / D3D12 / エバリュエーターに依存しない。CPU 評価（materialgraphext.cpp）、テスト、ベイクが共有する
//  ・ここの式は shader/hlsl/MaterialGraphNodesExt.hlsli（MGX_*）と 1 対 1 で対応する。
//    式を変えるときは必ず両方を同じ順序で直す（scripts/check-material-node-sync.py が関数名の対応を検査する）
//  ・パラメーターの置き場所（MaterialGraphNode）
//      Value[0..3] … 列挙値 / フラグ（ノードごとに下の表）
//      Params      … 可変長（ランプのキー、カーブの制御点、関数のシグネチャ）
//      Text        … Texture Parameter の名前 / Custom HLSL の式 / Function Input・Output の名前
//      PinNames    … Custom HLSL の入力名
//      Texture     … テクスチャ（Face SDF / MatCap / Flow / Packed / Thickness / Curve Atlas の画像など）
//      Function    … Material Function のアセット
// ============================================================================
#include "materialgraph.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace MaterialGraphNodes
{
    using Kind = MaterialNodeKind;

    inline constexpr int kMinRampKeys = 4;
    inline constexpr int kMaxRampKeys = 16;
    inline constexpr int kMaxCurvePoints = 8;   // RGB Curves：1 本のカーブの制御点（2〜8）
    inline constexpr int kCurveStride = 1 + 2 * kMaxCurvePoints; // [count, x0, y0, ...]
    inline constexpr int kMaxCurveRows = 4;     // Curve Atlas（内蔵）の行数
    inline constexpr int kMaxCurveKeys = 16;    // Curve Atlas：1 行のキー数
    inline constexpr int kAudioBands = 32;
    inline constexpr int kMaxFunctionInputs = 8;
    inline constexpr int kMaxFunctionOutputs = 4;
    inline constexpr int kMaxCustomInputs = 4;

    inline bool IsExtKind(Kind kind)
    {
        return kind >= Kind::ToonLightRamp && kind <= Kind::FunctionOutput;
    }
    // 追加メニューに出す 33 種（Function Input / Output は Function グラフの中だけ）
    inline bool IsPublicExtKind(Kind kind)
    {
        return kind >= Kind::ToonLightRamp && kind <= Kind::CurveAtlas;
    }

    enum class Interp : int
    {
        Linear = 0,
        Smooth = 1,   // 区間ごとの smoothstep（ランプ）/ 単調 3 次エルミート（カーブ）
        Constant = 2, // 階段
        Count
    };

    // MatCap / Toon の素材クラス（node.Value[1]）
    enum class ToonClass : int
    {
        Cloth = 0,
        Hair = 1,
        Skin = 2,
        Count
    };

    // ---------------------------------------------------------------- 基本関数
    inline float Sat(float v)
    {
        return std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f);
    }
    inline float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }
    inline float Smooth(float e0, float e1, float x)
    {
        const float d = e1 - e0;
        const float t = Sat(std::fabs(d) < 1e-12f ? (x >= e1 ? 1.0f : 0.0f) : (x - e0) / d);
        return t * t * (3.0f - 2.0f * t);
    }
    inline float Frac(float v)
    {
        return v - std::floor(v);
    }
    inline float SafeDiv(float a, float b)
    {
        return std::fabs(b) < 1e-8f ? 0.0f : a / b;
    }
    inline float Dot3(const float* a, const float* b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }
    inline void Norm3(float* v, const float* fallback)
    {
        const float l2 = Dot3(v, v);
        if (!(l2 > 1e-12f))
        {
            v[0] = fallback[0];
            v[1] = fallback[1];
            v[2] = fallback[2];
            return;
        }
        const float inv = 1.0f / std::sqrt(l2);
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    }

    // ---------------------------------------------------------------- キー列（Multi Color Ramp / Curve Atlas / RGB Curves 共通）
    // 昇順の (x, y0..y2) を線形 / スムーズ / 階段 / 単調 3 次で評価する。x は範囲外ならクランプ（ループは呼び出し側で畳む）
    struct Key
    {
        float X = 0.0f;
        float Y[3]{};
    };

    // 単調 3 次（Fritsch–Butland）の接線。keys は昇順
    inline float MonotoneSlope(const std::vector<Key>& k, size_t i, int channel)
    {
        const size_t n = k.size();
        if (n < 2)
        {
            return 0.0f;
        }
        const auto delta = [&](size_t a) {
            return SafeDiv(k[a + 1].Y[channel] - k[a].Y[channel], k[a + 1].X - k[a].X);
        };
        if (i == 0)
        {
            return delta(0);
        }
        if (i == n - 1)
        {
            return delta(n - 2);
        }
        const float d0 = delta(i - 1);
        const float d1 = delta(i);
        if (d0 * d1 <= 0.0f)
        {
            return 0.0f;
        }
        return 2.0f * d0 * d1 / (d0 + d1);
    }

    // channels = 評価するチャンネル数（1〜3）。keys が空なら 0
    inline void EvalKeys(const std::vector<Key>& keys, float x, Interp mode, int channels, float* out)
    {
        for (int c = 0; c < 3; ++c)
        {
            out[c] = 0.0f;
        }
        const size_t n = keys.size();
        if (n == 0)
        {
            return;
        }
        if (!std::isfinite(x))
        {
            x = 0.0f;
        }
        if (n == 1 || x <= keys.front().X)
        {
            for (int c = 0; c < channels; ++c)
            {
                out[c] = keys.front().Y[c];
            }
            return;
        }
        if (x >= keys.back().X)
        {
            for (int c = 0; c < channels; ++c)
            {
                out[c] = keys.back().Y[c];
            }
            return;
        }
        size_t i = 0;
        while (i + 2 < n && x >= keys[i + 1].X)
        {
            ++i;
        }
        const float span = keys[i + 1].X - keys[i].X;
        const float t = span < 1e-8f ? 0.0f : (x - keys[i].X) / span;
        for (int c = 0; c < channels; ++c)
        {
            const float a = keys[i].Y[c];
            const float b = keys[i + 1].Y[c];
            switch (mode)
            {
                case Interp::Constant:
                    out[c] = a;
                    break;
                case Interp::Smooth: {
                    // カーブは単調 3 次エルミート。ランプ（色）は t の smoothstep（MultiColorRamp 側で別に呼ぶ）
                    const float m0 = MonotoneSlope(keys, i, c) * span;
                    const float m1 = MonotoneSlope(keys, i + 1, c) * span;
                    const float t2 = t * t;
                    const float t3 = t2 * t;
                    out[c] = (2 * t3 - 3 * t2 + 1) * a + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * b + (t3 - t2) * m1;
                    break;
                }
                default:
                    out[c] = a + (b - a) * t;
                    break;
            }
        }
    }

    // ---------------------------------------------------------------- Multi Color Ramp
    // Params = [count, mode, (position, r, g, b) * count]。position は昇順に整えて使う
    inline std::vector<float> DefaultRampParams()
    {
        // 虹色の 4 キー（既存 Three Color Ramp の既定とは別。互換性は Three Color Ramp ノードが保つ）
        return {4, 0, 0.0f, 0.05f, 0.07f, 0.12f, 0.33f, 0.85f, 0.82f, 0.76f, 0.66f, 0.45f, 0.28f, 0.12f, 1.0f, 1.0f, 1.0f, 1.0f};
    }
    inline int RampKeyCount(const MaterialGraphNode& node)
    {
        if (node.Params.size() < 2)
        {
            return 0;
        }
        const int declared = static_cast<int>(std::clamp(node.Params[0], 0.0f, static_cast<float>(kMaxRampKeys)));
        const int available = static_cast<int>((node.Params.size() - 2) / 4);
        return (std::min)(declared, available);
    }
    inline Interp RampMode(const MaterialGraphNode& node)
    {
        return node.Params.size() < 2 ? Interp::Linear
                                      : static_cast<Interp>(std::clamp(static_cast<int>(node.Params[1]), 0, static_cast<int>(Interp::Count) - 1));
    }
    inline std::vector<Key> RampKeys(const MaterialGraphNode& node)
    {
        std::vector<Key> keys;
        const int count = RampKeyCount(node);
        for (int i = 0; i < count; ++i)
        {
            const float* p = &node.Params[2 + static_cast<size_t>(i) * 4];
            Key key;
            key.X = std::isfinite(p[0]) ? p[0] : 0.0f;
            for (int c = 0; c < 3; ++c)
            {
                key.Y[c] = std::isfinite(p[1 + c]) ? p[1 + c] : 0.0f;
            }
            keys.push_back(key);
        }
        std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.X < b.X; });
        return keys;
    }
    inline void SetRampKeys(MaterialGraphNode& node, const std::vector<Key>& keys, Interp mode)
    {
        node.Params.clear();
        const int count = std::clamp(static_cast<int>(keys.size()), 0, kMaxRampKeys);
        node.Params.push_back(static_cast<float>(count));
        node.Params.push_back(static_cast<float>(mode));
        for (int i = 0; i < count; ++i)
        {
            node.Params.push_back(keys[static_cast<size_t>(i)].X);
            for (int c = 0; c < 3; ++c)
            {
                node.Params.push_back(keys[static_cast<size_t>(i)].Y[c]);
            }
        }
    }
    inline void EvalRamp(const std::vector<Key>& keys, Interp mode, float t, float* rgb)
    {
        // 色は区間ごとに Linear / Smooth（t の smoothstep）/ Constant
        const size_t n = keys.size();
        if (n == 0)
        {
            rgb[0] = rgb[1] = rgb[2] = 0.0f;
            return;
        }
        if (!std::isfinite(t))
        {
            t = 0.0f;
        }
        if (n == 1 || t <= keys.front().X)
        {
            for (int c = 0; c < 3; ++c)
            {
                rgb[c] = keys.front().Y[c];
            }
            return;
        }
        if (t >= keys.back().X)
        {
            for (int c = 0; c < 3; ++c)
            {
                rgb[c] = keys.back().Y[c];
            }
            return;
        }
        size_t i = 0;
        while (i + 2 < n && t >= keys[i + 1].X)
        {
            ++i;
        }
        const float span = keys[i + 1].X - keys[i].X;
        float u = span < 1e-8f ? 0.0f : (t - keys[i].X) / span;
        if (mode == Interp::Smooth)
        {
            u = u * u * (3.0f - 2.0f * u);
        }
        else if (mode == Interp::Constant)
        {
            u = 0.0f;
        }
        for (int c = 0; c < 3; ++c)
        {
            rgb[c] = keys[i].Y[c] + (keys[i + 1].Y[c] - keys[i].Y[c]) * u;
        }
    }
    inline std::vector<float> DefaultRampFromThree(const float* a, const float* b, const float* c)
    {
        // 既存 Three Color Ramp（0 → A、0.5 → B、1 → C の線形）と同じ結果になる 3 キー + 末尾キーの重複（4 キー最小）
        return {4, 0, 0.0f, a[0], a[1], a[2], 0.5f, b[0], b[1], b[2], 0.75f, (b[0] + c[0]) * 0.5f, (b[1] + c[1]) * 0.5f, (b[2] + c[2]) * 0.5f,
                1.0f, c[0], c[1], c[2]};
    }

    // ---------------------------------------------------------------- RGB Curves
    // Params = 4 本（Master, R, G, B）× [count, x0, y0, x1, y1, ... （最大 8 点）]。Value[0] = 補間（0 線形 / 1 スムーズ）
    inline std::vector<float> DefaultCurveParams()
    {
        std::vector<float> params(static_cast<size_t>(4 * kCurveStride), 0.0f);
        for (int curve = 0; curve < 4; ++curve)
        {
            float* p = &params[static_cast<size_t>(curve * kCurveStride)];
            p[0] = 2;
            p[1] = 0.0f;
            p[2] = 0.0f;
            p[3] = 1.0f;
            p[4] = 1.0f;
        }
        return params;
    }
    inline std::vector<Key> CurveKeys(const std::vector<float>& params, int curve)
    {
        std::vector<Key> keys;
        const size_t base = static_cast<size_t>(curve) * kCurveStride;
        if (curve < 0 || base + kCurveStride > params.size())
        {
            return keys;
        }
        const int count = std::clamp(static_cast<int>(params[base]), 0, kMaxCurvePoints);
        for (int i = 0; i < count; ++i)
        {
            Key key;
            key.X = params[base + 1 + static_cast<size_t>(i) * 2];
            key.Y[0] = params[base + 2 + static_cast<size_t>(i) * 2];
            keys.push_back(key);
        }
        std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.X < b.X; });
        return keys;
    }
    inline float EvalCurve(const std::vector<Key>& keys, float x, Interp mode)
    {
        if (keys.empty())
        {
            return x;
        }
        float out[3];
        EvalKeys(keys, x, mode, 1, out);
        return out[0];
    }

    // ---------------------------------------------------------------- Curve Atlas（内蔵カーブ）
    // Params = [rowCount, keyCountRow0, x, y, ..., keyCountRow1, ...]（行ごとに最大 16 キー）。Value[0] = 補間、Value[1] = ループ
    enum class LoopMode : int
    {
        Clamp = 0,
        Repeat = 1,
        PingPong = 2,
        Count
    };
    inline float ApplyLoop(float x, LoopMode mode, float start, float end)
    {
        const float span = end - start;
        if (!(span > 1e-8f))
        {
            return start;
        }
        switch (mode)
        {
            case LoopMode::Repeat:
                return start + Frac((x - start) / span) * span;
            case LoopMode::PingPong: {
                const float p = std::fmod(std::fabs(x - start) / span, 2.0f);
                return start + (p > 1.0f ? 2.0f - p : p) * span;
            }
            default:
                return std::clamp(x, start, end);
        }
    }
    inline std::vector<float> DefaultAtlasParams()
    {
        // 4 行：0 = ease in/out、1 = linear、2 = ease in、3 = ease out
        return {4, 3, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 3, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 3, 0.0f, 0.0f, 0.5f, 0.25f, 1.0f, 1.0f,
                3, 0.0f, 0.0f, 0.5f, 0.75f, 1.0f, 1.0f};
    }
    inline std::vector<std::vector<Key>> AtlasRows(const MaterialGraphNode& node)
    {
        std::vector<std::vector<Key>> rows;
        if (node.Params.empty())
        {
            return rows;
        }
        const int rowCount = std::clamp(static_cast<int>(node.Params[0]), 0, kMaxCurveRows);
        size_t cursor = 1;
        for (int r = 0; r < rowCount && cursor < node.Params.size(); ++r)
        {
            const int count = std::clamp(static_cast<int>(node.Params[cursor++]), 0, kMaxCurveKeys);
            std::vector<Key> keys;
            for (int i = 0; i < count && cursor + 1 < node.Params.size() + 0; ++i)
            {
                Key key;
                key.X = node.Params[cursor++];
                key.Y[0] = cursor < node.Params.size() ? node.Params[cursor] : 0.0f;
                ++cursor;
                keys.push_back(key);
            }
            std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.X < b.X; });
            rows.push_back(std::move(keys));
        }
        return rows;
    }
    inline void SetAtlasRows(MaterialGraphNode& node, const std::vector<std::vector<Key>>& rows)
    {
        node.Params.clear();
        const int rowCount = std::clamp(static_cast<int>(rows.size()), 0, kMaxCurveRows);
        node.Params.push_back(static_cast<float>(rowCount));
        for (int r = 0; r < rowCount; ++r)
        {
            const auto& keys = rows[static_cast<size_t>(r)];
            const int count = std::clamp(static_cast<int>(keys.size()), 0, kMaxCurveKeys);
            node.Params.push_back(static_cast<float>(count));
            for (int i = 0; i < count; ++i)
            {
                node.Params.push_back(keys[static_cast<size_t>(i)].X);
                node.Params.push_back(keys[static_cast<size_t>(i)].Y[0]);
            }
        }
    }
    inline void SetCurveKeys(std::vector<float>& params, int curve, const std::vector<Key>& keys)
    {
        if (params.size() < static_cast<size_t>(4 * kCurveStride))
        {
            params = DefaultCurveParams();
        }
        float* p = &params[static_cast<size_t>(curve) * kCurveStride];
        const int count = std::clamp(static_cast<int>(keys.size()), 0, kMaxCurvePoints);
        p[0] = static_cast<float>(count);
        for (int i = 0; i < count; ++i)
        {
            p[1 + i * 2] = keys[static_cast<size_t>(i)].X;
            p[2 + i * 2] = keys[static_cast<size_t>(i)].Y[0];
        }
    }
    // 行の重み（row が 1.2 なら行 1 に 0.8、行 2 に 0.2）。HLSL 側（MGX_RowWeight）と同じ
    inline float RowWeight(float row, int index)
    {
        return Sat(1.0f - std::fabs(row - static_cast<float>(index)));
    }

    // ---------------------------------------------------------------- Toon
    inline float ToonWrap(int toonClass)
    {
        return toonClass >= static_cast<int>(ToonClass::Skin) ? 0.38f : 0.12f; // ToonLighting.hlsli / LiveToonStyle.hlsli と同じ
    }
    // 段階ランプ。戻り値は帯の位置 q（0〜1）。rgb は 3 色ランプ
    inline float ToonBand(float nDotL, float wrap, float steps, float threshold, float smoothness)
    {
        const float c = Sat((nDotL + wrap) / (1.0f + wrap));
        const int n = std::clamp(static_cast<int>(std::floor(steps + 0.5f)), 1, 8);
        const float w = (std::max)(smoothness, 0.001f);
        float b = 0.0f;
        for (int k = 1; k < n; ++k)
        {
            const float edge = threshold + (static_cast<float>(k) - static_cast<float>(n) * 0.5f) / static_cast<float>(n);
            b += Smooth(edge - w, edge + w, c);
        }
        return n > 1 ? b / static_cast<float>(n - 1) : 1.0f;
    }
    inline void ToonRampColor(float q, const float* c1, const float* c2, const float* c3, float* out)
    {
        for (int i = 0; i < 3; ++i)
        {
            out[i] = q < 0.5f ? Lerp(c1[i], c2[i], q * 2.0f) : Lerp(c2[i], c3[i], (q - 0.5f) * 2.0f);
        }
    }
    inline float ToonRim(const float* n, const float* v, const float* l, float width, float power, float directionalMask)
    {
        const float edge = 1.0f - Sat(Dot3(n, v));
        const float rim = std::pow(Smooth(1.0f - std::clamp(width, 0.01f, 1.0f), 1.0f, edge), (std::max)(power, 0.01f));
        const float facing = Sat(Dot3(n, l) * 0.5f + 0.5f);
        const float back = Smooth(-0.4f, 0.2f, -Dot3(v, l)); // LiveToonBand(-dot(V, L), -0.1, 0.3)
        return rim * Lerp(1.0f, facing * back, Sat(directionalMask));
    }
    inline float HairHighlight(const float* tangent, const float* n, const float* v, const float* l, float anisotropy, float roughness,
                               float width, float shift)
    {
        float h[3] = {l[0] + v[0], l[1] + v[1], l[2] + v[2]};
        const float up[3] = {0, 1, 0};
        Norm3(h, n);
        float t[3] = {tangent[0] - n[0] * Dot3(n, tangent), tangent[1] - n[1] * Dot3(n, tangent), tangent[2] - n[2] * Dot3(n, tangent)};
        Norm3(t, up);
        float ts[3] = {t[0] + n[0] * shift, t[1] + n[1] * shift, t[2] + n[2] * shift};
        Norm3(ts, t);
        const float tDotH = Dot3(ts, h);
        const float sinTH = std::sqrt(Sat(1.0f - tDotH * tDotH));
        const float e = Lerp(256.0f, 8.0f, Sat(roughness));
        const float kk = std::pow(sinTH, e) * Smooth(-1.0f, 0.0f, tDotH);
        const float iso = std::pow(Sat(Dot3(n, h)), e * 0.5f);
        const float spec = Lerp(iso, kk, Sat(anisotropy));
        const float w = std::clamp(width, 0.01f, 1.0f);
        return Smooth(1.0f - w, 1.0f - w * 0.5f, spec) * Sat(Dot3(n, l) + 0.2f);
    }
    // Face SDF。sdfA = 通常の UV、sdfB = 左右反転した UV の SDF 値（0〜1）。戻り値 = 光が当たる量（1 = 明るい）
    inline float FaceSdfLit(float sdfA, float sdfB, const float* forward, const float* right, const float* light, float threshold,
                            float smoothness, bool flip)
    {
        float lf[2] = {light[0], light[2]};
        float ff[2] = {forward[0], forward[2]};
        float rf[2] = {right[0], right[2]};
        const auto norm2 = [](float* v) {
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1]);
            if (l < 1e-6f)
            {
                v[0] = 0.0f;
                v[1] = 1.0f;
                return;
            }
            v[0] /= l;
            v[1] /= l;
        };
        norm2(lf);
        norm2(ff);
        norm2(rf);
        const float fDotL = ff[0] * lf[0] + ff[1] * lf[1];
        const float rDotL = rf[0] * lf[0] + rf[1] * lf[1];
        const bool rightSide = flip ? rDotL <= 0.0f : rDotL > 0.0f;
        const float s = (rightSide ? sdfA : sdfB) + (threshold - 0.5f);
        const float angle = 0.5f - 0.5f * fDotL; // 0 = 正面から、1 = 真後ろから
        const float w = (std::max)(smoothness, 0.001f);
        return Smooth(angle - w, angle + w, s);
    }
    // 瞳ハイライト（スーパー楕円）。q = UV 上の中心からの差（ライト / 視線のずれ込み）
    inline float EyeHighlightShape(float qx, float qy, float size, float shape)
    {
        const float p = Lerp(2.0f, 6.0f, Sat(shape));
        const float d = std::pow(std::pow(std::fabs(qx), p) + std::pow(std::fabs(qy), p), 1.0f / p);
        const float s = (std::max)(size, 1e-4f);
        return 1.0f - Smooth(s * 0.85f, s, d);
    }

    // ---------------------------------------------------------------- PBR / 色
    inline void HsvAdjust(const float* rgb, float hue, float saturation, float value, float* out)
    {
        // MaterialGraphMath の RgbToHsv / HsvToRgb と同じ（materialgraphmath.h をここから参照しないよう再掲）
        const float mx = (std::max)({rgb[0], rgb[1], rgb[2]});
        const float mn = (std::min)({rgb[0], rgb[1], rgb[2]});
        const float d = mx - mn;
        float h = 0.0f;
        if (d > 1e-6f)
        {
            h = mx == rgb[0] ? (rgb[1] - rgb[2]) / d : (mx == rgb[1] ? 2.0f + (rgb[2] - rgb[0]) / d : 4.0f + (rgb[0] - rgb[1]) / d);
            h = Frac(h / 6.0f);
        }
        float s = mx > 1e-6f ? d / mx : 0.0f;
        float v = mx;
        h = Frac(h + hue);
        s = Sat(s * saturation);
        v = (std::max)(v * value, 0.0f);
        for (int i = 0; i < 3; ++i)
        {
            const float k = Frac(h + (3.0f - static_cast<float>(i)) / 3.0f) * 6.0f;
            const float p = Sat(std::fabs(k - 3.0f) - 1.0f);
            out[i] = v * (1.0f + (p - 1.0f) * s);
        }
    }
    // 薄膜干渉。cosTheta = N・V、thickness = nm、ior = 膜の屈折率
    inline void ThinFilm(float cosTheta, float thickness, float ior, float strength, float* rgb)
    {
        const float n = (std::max)(ior, 1.0f);
        const float ci = Sat(cosTheta);
        const float si2 = 1.0f - ci * ci;
        const float ct = std::sqrt((std::max)(1.0f - si2 / (n * n), 0.0f));
        const float opd = 2.0f * n * (std::max)(thickness, 0.0f) * ct; // 光路差（nm）
        const float lambda[3] = {650.0f, 532.0f, 450.0f};
        for (int c = 0; c < 3; ++c)
        {
            const float phase = 6.28318531f * opd / lambda[c] + 3.14159265f; // 低屈折 → 高屈折の反射で π ずれる
            rgb[c] = Lerp(1.0f, 0.5f + 0.5f * std::cos(phase), Sat(strength));
        }
    }
    // Triplanar 法線（Ben Golus の whiteout 方式）。nx / ny / nz は各面（YZ / XZ / XY）の接空間法線、nWorld = ジオメトリの法線
    // 戻り値はワールド空間の法線（接空間へは呼び出し側が T / B / N で変換する）
    inline void TriplanarNormalWorld(const float* nx, const float* ny, const float* nz, const float* nWorld, const float* weights,
                                     float* out)
    {
        const float sx = nWorld[0] < 0.0f ? -1.0f : 1.0f;
        const float sy = nWorld[1] < 0.0f ? -1.0f : 1.0f;
        const float sz = nWorld[2] < 0.0f ? -1.0f : 1.0f;
        // 負の軸側から見たときに U が反転する分を戻す
        const float tx[3] = {nx[0] * sx + nWorld[2], nx[1] + nWorld[1], std::fabs(nx[2]) * nWorld[0]};
        const float ty[3] = {ny[0] * sy + nWorld[0], ny[1] + nWorld[2], std::fabs(ny[2]) * nWorld[1]};
        const float tz[3] = {nz[0] * -sz + nWorld[0], nz[1] + nWorld[1], std::fabs(nz[2]) * nWorld[2]};
        // 面の接空間 → ワールドの軸へ並べ替え：X 面 .zyx、Y 面 .xzy、Z 面 .xyz
        out[0] = tx[2] * weights[0] + ty[0] * weights[1] + tz[0] * weights[2];
        out[1] = tx[1] * weights[0] + ty[2] * weights[1] + tz[1] * weights[2];
        out[2] = tx[0] * weights[0] + ty[1] * weights[1] + tz[2] * weights[2];
        const float fallback[3] = {nWorld[0], nWorld[1], nWorld[2]};
        Norm3(out, fallback);
    }

    // ---------------------------------------------------------------- Flipbook / Flow
    inline int FlipbookFrame(float time, float fps, float frameOffset, int frames, LoopMode loop)
    {
        const double scaled = std::floor(static_cast<double>(time) * static_cast<double>(fps) + static_cast<double>(frameOffset) + 1e-4);
        const long long raw = static_cast<long long>(std::clamp(std::isfinite(scaled) ? scaled : 0.0, -2.0e9, 2.0e9)); // int の範囲に収める（GPU は int）
        if (frames <= 1)
        {
            return 0;
        }
        switch (loop)
        {
            case LoopMode::Repeat: {
                long long m = raw % frames;
                return static_cast<int>(m < 0 ? m + frames : m);
            }
            case LoopMode::PingPong: {
                const long long period = 2LL * (frames - 1);
                long long m = raw % period;
                if (m < 0)
                {
                    m += period;
                }
                return static_cast<int>(m < frames ? m : period - m);
            }
            default:
                return static_cast<int>(std::clamp<long long>(raw, 0, frames - 1));
        }
    }

    // ---------------------------------------------------------------- Beat Phase（CPU / GPU の動的スロットで共有）
    struct BeatResult
    {
        float Phase = 0.0f;     // 0〜1（細分化した拍の中）
        float BeatIndex = 0.0f; // 細分化した拍の通し番号（2^24 で折り返す。float で正確に扱える範囲）
        float BeatTime = 0.0f;  // 拍の頭からの秒数
        float Pulse = 0.0f;     // 拍の頭で 1 → 0 へ指数減衰
    };
    // time = 音声クロック（秒、double）。offset = 拍。BPM は一定（テンポマップは LiveMusic::TempoMap を使う呼び出し側が renormalize する）
    inline BeatResult ComputeBeat(double time, double bpm, double offsetBeats, double subdivision)
    {
        BeatResult r;
        if (!(bpm > 0.0) || !std::isfinite(time) || !std::isfinite(bpm))
        {
            return r;
        }
        const double sub = (std::max)(subdivision, 1.0e-3);
        const double beats = (time * bpm / 60.0 + offsetBeats) * sub; // double で積算して、float へ落とすのは小数部だけ
        const double whole = std::floor(beats);
        const double frac = beats - whole;
        r.Phase = static_cast<float>(frac);
        r.BeatIndex = static_cast<float>(std::fmod(whole, 16777216.0));
        if (r.BeatIndex < 0.0f)
        {
            r.BeatIndex += 16777216.0f;
        }
        r.BeatTime = static_cast<float>(frac * 60.0 / (bpm * sub));
        r.Pulse = std::exp(-6.0f * r.Phase);
        return r;
    }

    // ---------------------------------------------------------------- 曲率（メッシュに曲率が無いとき：前処理ベイク）
    // 頂点ごとの平均曲率（正 = 凸）。法線の変化 / 辺の長さの平均（cotangent 重みは使わない近似。ベイク用）
    inline std::vector<float> BakeVertexCurvature(const std::vector<std::array<float, 3>>& positions,
                                                  const std::vector<std::array<float, 3>>& normals,
                                                  const std::vector<uint32_t>& indices)
    {
        std::vector<float> sum(positions.size(), 0.0f);
        std::vector<float> weight(positions.size(), 0.0f);
        const auto edge = [&](uint32_t a, uint32_t b) {
            if (a >= positions.size() || b >= positions.size() || a >= normals.size() || b >= normals.size())
            {
                return;
            }
            float d[3];
            float dn[3];
            for (int i = 0; i < 3; ++i)
            {
                d[i] = positions[b][static_cast<size_t>(i)] - positions[a][static_cast<size_t>(i)];
                dn[i] = normals[b][static_cast<size_t>(i)] - normals[a][static_cast<size_t>(i)];
            }
            const float len2 = Dot3(d, d);
            if (len2 < 1e-12f)
            {
                return;
            }
            const float k = Dot3(dn, d) / len2; // (n_b - n_a)・(p_b - p_a) / |p_b - p_a|^2：凸なら正
            sum[a] += k;
            sum[b] += k;
            weight[a] += 1.0f;
            weight[b] += 1.0f;
        };
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            edge(indices[i], indices[i + 1]);
            edge(indices[i + 1], indices[i + 2]);
            edge(indices[i + 2], indices[i]);
        }
        for (size_t i = 0; i < sum.size(); ++i)
        {
            sum[i] = weight[i] > 0.0f ? sum[i] / weight[i] : 0.0f;
        }
        return sum;
    }
    // 曲率 → マスク。mode 0 = 凸（エッジ）、1 = 凹（キャビティ）、2 = 両方
    inline float CurvatureMask(float curvature, float threshold, float softness, int mode)
    {
        const float c = mode == 0 ? curvature : mode == 1 ? -curvature : std::fabs(curvature);
        const float s = (std::max)(softness, 1e-4f);
        return Smooth(threshold - s, threshold + s, c);
    }

    // ---------------------------------------------------------------- 既定値の設定
    // MaterialGraph::AddNode から呼ばれる（Value / Params / Text / PinNames の既定）
    void InitNode(MaterialGraphNode& node);
    // 表示用：カテゴリ名（エディターのメニュー。UI 色はエディター側）
    const char* CategoryName(Kind kind);
    // 固定出力スロットの数（0 = 通常の出力）
    int FixedOutputCount(Kind kind);
} // namespace MaterialGraphNodes
