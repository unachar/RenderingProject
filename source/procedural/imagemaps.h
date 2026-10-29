#pragma once
// 画像から高さマップ・法線マップを作る CPU 処理。D3D に依存しない。
// ヘッダーのみで完結させ、プロジェクトへの .cpp 追加を不要にしている。
// エンジンのプリコンパイル済みヘッダーは std / DirectX をグローバルへ導入するため、
// ここでは修飾名だけを使い、短い名前を外へ漏らさない。
#include "proceduralstudio.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace ProcStudio
{
    // 高さの元にするチャンネル。値はファイルに格納された値（8 ビット画像なら sRGB 値）を使う。
    enum class HeightChannel
    {
        Luminance, // Rec.709 輝度
        Average,
        Red,
        Green,
        Blue,
        Alpha,
        Max,
        Count
    };

    enum class NormalFilter
    {
        CentralDifference, // 隣接 2 画素。最も鋭い。
        Sobel,             // 3x3。ノイズに強い。
        Scharr,            // 3x3。回転に対する誤差が小さい。
        Count
    };

    struct ImageMapParams
    {
        // 高さ。
        HeightChannel Channel = HeightChannel::Luminance;
        bool Invert = false;
        float BlurRadius = 0.0f;     // 元画像のピクセル単位（ガウスの σ 相当）。0 で無効。
        float HighPassRadius = 0.0f; // 元画像のピクセル単位。0 で無効。照明ムラなど大きな明暗を除去する。
        bool AutoLevels = false;     // 最小値〜最大値を 0〜1 へ引き伸ばす。
        float Contrast = 1.0f;       // プロシージャルと同じ式: (v - 0.5) * Contrast + 0.5 + Bias
        float Bias = 0.0f;
        float Gamma = 1.0f;          // 最後に v^Gamma。
        // 法線。
        float NormalStrength = 8.0f; // 元画像の隣接画素差に掛ける係数。
        NormalFilter Filter = NormalFilter::Sobel;
        bool FlipY = false;          // false: OpenGL (+Y)。プロシージャルの法線出力と同じ規約。true: DirectX (-Y)。
        bool Tileable = true;        // 端を折り返して参照する。false なら端をクランプ。
    };

    struct ImageMaps
    {
        Image Height; // RGB に同じ高さ、A = 1。
        Image Normal; // 接空間法線を 0〜1 にエンコード。
    };

    namespace ImageMapDetail
    {
        inline void ParallelRows(int count, const std::function<void(int)>& body)
        {
            const int workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 32);
            if (count < 16 || workers == 1)
            {
                for (int i = 0; i < count; ++i)
                {
                    body(i);
                }
                return;
            }
            std::vector<std::thread> threads;
            threads.reserve(workers);
            for (int w = 0; w < workers; ++w)
            {
                threads.emplace_back([&, w] {
                    for (int i = w; i < count; i += workers)
                    {
                        body(i);
                    }
                });
            }
            for (auto& thread : threads)
            {
                thread.join();
            }
        }

        inline int Index(int i, int n, bool wrap)
        {
            if (wrap)
            {
                const int r = i % n;
                return r < 0 ? r + n : r;
            }
            return std::clamp(i, 0, n - 1);
        }

        inline float LinearToSrgb(float v)
        {
            v = std::clamp(v, 0.0f, 1.0f);
            return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        }

        // 1 方向の箱型ぼかし（累積和、O(n)）。stride で行・列を切り替える。
        inline void BoxLine(const float* src, float* dst, int n, int stride, int radius, bool wrap)
        {
            const float inv = 1.0f / static_cast<float>(radius * 2 + 1);
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k)
            {
                sum += src[static_cast<size_t>(Index(k, n, wrap)) * stride];
            }
            for (int i = 0; i < n; ++i)
            {
                dst[static_cast<size_t>(i) * stride] = sum * inv;
                sum += src[static_cast<size_t>(Index(i + radius + 1, n, wrap)) * stride];
                sum -= src[static_cast<size_t>(Index(i - radius, n, wrap)) * stride];
            }
        }

        // 3 回の箱型ぼかしでガウスぼかしを近似する。sigma はピクセル単位。
        inline void GaussianBlur(std::vector<float>& h, int width, int height, float sigma, bool wrap)
        {
            if (sigma < 0.5f || width <= 0 || height <= 0)
            {
                return;
            }
            // n 回の幅 w の箱: 分散 = n (w^2 - 1) / 12。n = 3 → w = sqrt(4σ^2 + 1)。
            const float boxWidth = std::sqrt(4.0f * sigma * sigma + 1.0f);
            const int radius = (std::max)(1, static_cast<int>(std::lround((boxWidth - 1.0f) * 0.5f)));
            std::vector<float> temp(h.size());
            for (int pass = 0; pass < 3; ++pass)
            {
                ParallelRows(height, [&](int y) {
                    const size_t row = static_cast<size_t>(y) * width;
                    BoxLine(h.data() + row, temp.data() + row, width, 1, (std::min)(radius, width), wrap);
                });
                ParallelRows(width, [&](int x) {
                    BoxLine(temp.data() + x, h.data() + x, height, width, (std::min)(radius, height), wrap);
                });
            }
        }

        inline float ChannelValue(const Float4& p, HeightChannel channel, bool srgbEncoded)
        {
            const auto enc = [&](float v) {
                return srgbEncoded ? LinearToSrgb(v) : v;
            };
            switch (channel)
            {
                case HeightChannel::Average:
                    return (enc(p.X) + enc(p.Y) + enc(p.Z)) / 3.0f;
                case HeightChannel::Red:
                    return enc(p.X);
                case HeightChannel::Green:
                    return enc(p.Y);
                case HeightChannel::Blue:
                    return enc(p.Z);
                case HeightChannel::Alpha:
                    return p.W;
                case HeightChannel::Max:
                    return (std::max)({enc(p.X), enc(p.Y), enc(p.Z)});
                case HeightChannel::Luminance:
                default:
                    // 線形空間で輝度を求めてから格納値の空間へ戻す。
                    return enc(0.2126f * p.X + 0.7152f * p.Y + 0.0722f * p.Z);
            }
        }
    } // namespace ImageMapDetail

    inline const char* HeightChannelName(HeightChannel channel)
    {
        static const char* names[] = {"輝度", "RGB 平均", "R", "G", "B", "A (アルファ)", "RGB 最大"};
        return names[std::clamp(static_cast<int>(channel), 0, static_cast<int>(HeightChannel::Count) - 1)];
    }

    // 面積平均で縮小する（長辺を maxSide 以下に）。縮小不要ならそのまま複製。
    inline Image DownsampleImage(const Image& source, int maxSide)
    {
        if (source.Width <= 0 || source.Height <= 0)
        {
            return {};
        }
        const int longest = (std::max)(source.Width, source.Height);
        if (longest <= maxSide)
        {
            return source;
        }
        const double scale = static_cast<double>(maxSide) / longest;
        Image result;
        result.Width = (std::max)(1, static_cast<int>(std::lround(source.Width * scale)));
        result.Height = (std::max)(1, static_cast<int>(std::lround(source.Height * scale)));
        result.Pixels.resize(static_cast<size_t>(result.Width) * result.Height);
        ImageMapDetail::ParallelRows(result.Height, [&](int y) {
            const int y0 = static_cast<int>(static_cast<int64_t>(y) * source.Height / result.Height);
            const int y1 = (std::max)(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * source.Height / result.Height));
            for (int x = 0; x < result.Width; ++x)
            {
                const int x0 = static_cast<int>(static_cast<int64_t>(x) * source.Width / result.Width);
                const int x1 =
                    (std::max)(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * source.Width / result.Width));
                Float4 sum{0, 0, 0, 0};
                for (int sy = y0; sy < y1; ++sy)
                {
                    const Float4* row = source.Pixels.data() + static_cast<size_t>(sy) * source.Width;
                    for (int sx = x0; sx < x1; ++sx)
                    {
                        sum.X += row[sx].X;
                        sum.Y += row[sx].Y;
                        sum.Z += row[sx].Z;
                        sum.W += row[sx].W;
                    }
                }
                const float inv = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
                result.Pixels[static_cast<size_t>(y) * result.Width + x] = {sum.X * inv, sum.Y * inv, sum.Z * inv,
                                                                            sum.W * inv};
            }
        });
        return result;
    }

    // pixelScale = 処理する画像の幅 / 元画像の幅。縮小プレビューでも書き出しと同じ見た目になるよう、
    // ぼかし半径と法線の傾きをこの比率で補正する。
    inline std::vector<float> ComputeHeight(const Image& source, const ImageMapParams& p, bool srgbEncoded,
                                            float pixelScale)
    {
        using namespace ImageMapDetail;
        const int width = source.Width;
        const int height = source.Height;
        std::vector<float> h(static_cast<size_t>(width) * height);
        ParallelRows(height, [&](int y) {
            for (int x = 0; x < width; ++x)
            {
                const size_t i = static_cast<size_t>(y) * width + x;
                const float v = std::clamp(ChannelValue(source.Pixels[i], p.Channel, srgbEncoded), 0.0f, 1.0f);
                h[i] = p.Invert ? 1.0f - v : v;
            }
        });
        GaussianBlur(h, width, height, p.BlurRadius * pixelScale, p.Tileable);
        if (p.HighPassRadius > 0.0f)
        {
            std::vector<float> low = h;
            GaussianBlur(low, width, height, p.HighPassRadius * pixelScale, p.Tileable);
            for (size_t i = 0; i < h.size(); ++i)
            {
                h[i] = h[i] - low[i] + 0.5f;
            }
        }
        float lo = 0.0f;
        float hi = 1.0f;
        if (p.AutoLevels && !h.empty())
        {
            const auto [mn, mx] = std::minmax_element(h.begin(), h.end());
            if (*mx - *mn > 1e-6f)
            {
                lo = *mn;
                hi = *mx;
            }
        }
        const float range = 1.0f / (hi - lo);
        const float gamma = (std::max)(p.Gamma, 0.01f);
        for (float& v : h)
        {
            float t = (v - lo) * range;
            t = std::clamp((t - 0.5f) * p.Contrast + 0.5f + p.Bias, 0.0f, 1.0f);
            v = gamma == 1.0f ? t : std::pow(t, gamma);
        }
        return h;
    }

    inline Image HeightToNormal(const std::vector<float>& h, int width, int height, const ImageMapParams& p,
                                float pixelScale)
    {
        using namespace ImageMapDetail;
        Image out;
        out.Width = width;
        out.Height = height;
        out.Pixels.resize(h.size());
        // 縮小画像では隣接画素の差が 1/pixelScale 倍に大きくなるため、pixelScale を掛けて打ち消す。
        const float strength = p.NormalStrength * pixelScale;
        ParallelRows(height, [&](int y) {
            const auto at = [&](int px, int py) {
                return h[static_cast<size_t>(Index(py, height, p.Tileable)) * width + Index(px, width, p.Tileable)];
            };
            for (int x = 0; x < width; ++x)
            {
                float dx = 0.0f;
                float dy = 0.0f;
                if (p.Filter == NormalFilter::CentralDifference)
                {
                    dx = at(x + 1, y) - at(x - 1, y);
                    dy = at(x, y + 1) - at(x, y - 1);
                }
                else
                {
                    // 中央差分と同じスケールになるよう重みの合計で割る。
                    const float side = p.Filter == NormalFilter::Sobel ? 1.0f : 3.0f;
                    const float center = p.Filter == NormalFilter::Sobel ? 2.0f : 10.0f;
                    const float norm = 1.0f / (side * 2.0f + center);
                    dx = ((at(x + 1, y - 1) - at(x - 1, y - 1)) * side + (at(x + 1, y) - at(x - 1, y)) * center +
                          (at(x + 1, y + 1) - at(x - 1, y + 1)) * side) *
                         norm;
                    dy = ((at(x - 1, y + 1) - at(x - 1, y - 1)) * side + (at(x, y + 1) - at(x, y - 1)) * center +
                          (at(x + 1, y + 1) - at(x + 1, y - 1)) * side) *
                         norm;
                }
                dx *= strength;
                dy *= strength;
                // プロシージャルの法線出力（GenerateTexture）と同じ (-dx, dy, 1)。FlipY で DirectX 規約。
                float nx = -dx;
                float ny = p.FlipY ? -dy : dy;
                float nz = 1.0f;
                const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
                nx *= inv;
                ny *= inv;
                nz *= inv;
                out.Pixels[static_cast<size_t>(y) * width + x] = {nx * 0.5f + 0.5f, ny * 0.5f + 0.5f,
                                                                  nz * 0.5f + 0.5f, 1.0f};
            }
        });
        return out;
    }

    inline ImageMaps BuildImageMaps(const Image& source, const ImageMapParams& p, bool srgbEncoded, float pixelScale)
    {
        ImageMaps maps;
        if (source.Width <= 0 || source.Height <= 0)
        {
            return maps;
        }
        const auto h = ComputeHeight(source, p, srgbEncoded, pixelScale);
        maps.Height.Width = source.Width;
        maps.Height.Height = source.Height;
        maps.Height.Pixels.resize(h.size());
        for (size_t i = 0; i < h.size(); ++i)
        {
            maps.Height.Pixels[i] = {h[i], h[i], h[i], 1.0f};
        }
        maps.Normal = HeightToNormal(h, source.Width, source.Height, p, pixelScale);
        return maps;
    }
} // namespace ProcStudio
