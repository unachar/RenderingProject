#pragma once
// ============================================================================
// 音声 → マテリアルグラフ（Audio Spectrum / Beat Phase ノード）の橋渡し。レンダラー / XAudio2 / ECS に依存しない。
//  ・SpectrumAnalyzer：PCM の窓から 32 バンド（20Hz〜20kHz の対数）の大きさ 0〜1 を作る（Hann 窓 + 基数 2 の FFT）
//  ・Bridge：音声スレッド / タイムラインが「いまの音声クロックとスペクトル」を置き、レンダリングが毎フレーム読む。
//    音声クロックは XAudio2 の SamplesPlayed（再生位置）なので、壁時計と違ってシーク / 一時停止 / ループでもずれない。
//    フレーム間（音声クロックの更新が粗いとき）は壁時計で補間する（再生中のみ。上限 100ms）
// ============================================================================
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace AudioMaterial
{
    inline constexpr int kBands = 32;
    inline constexpr double kMinHz = 20.0;
    inline constexpr double kMaxHz = 20000.0;

    // バンド k の下端の周波数（Hz）。k = kBands で上端
    inline double BandEdgeHz(int k)
    {
        return kMinHz * std::pow(kMaxHz / kMinHz, static_cast<double>(k) / static_cast<double>(kBands));
    }

    class SpectrumAnalyzer
    {
      public:
        explicit SpectrumAnalyzer(size_t fftSize = 2048) : m_Size(fftSize)
        {
            // 2 のべき乗に丸める
            size_t n = 1;
            while (n < fftSize)
            {
                n <<= 1;
            }
            m_Size = (std::max<size_t>)(n, 64);
            m_Window.resize(m_Size);
            for (size_t i = 0; i < m_Size; ++i)
            {
                m_Window[i] = 0.5f - 0.5f * std::cos(6.28318530718f * static_cast<float>(i) / static_cast<float>(m_Size - 1));
            }
            m_Buffer.resize(m_Size);
        }

        size_t Size() const
        {
            return m_Size;
        }

        // mono = Size() 個のサンプル（-1〜1）。bands = kBands 個（0〜1）。dB（-60〜0）を 0〜1 に写す
        void Analyze(const float* mono, size_t count, double sampleRate, float* bands)
        {
            for (int b = 0; b < kBands; ++b)
            {
                bands[b] = 0.0f;
            }
            if (!mono || count == 0 || !(sampleRate > 0.0))
            {
                return;
            }
            double window = 0.0;
            for (size_t i = 0; i < m_Size; ++i)
            {
                const float s = i < count && std::isfinite(mono[i]) ? mono[i] : 0.0f;
                m_Buffer[i] = {s * m_Window[i], 0.0f};
                window += m_Window[i];
            }
            Fft(m_Buffer);
            const double binHz = sampleRate / static_cast<double>(m_Size);
            const double nyquist = sampleRate * 0.5;
            const double scale = 2.0 / (std::max)(window, 1e-9); // 振幅 1 の正弦波 → 大きさ 1
            for (int b = 0; b < kBands; ++b)
            {
                const double lo = BandEdgeHz(b);
                const double hi = BandEdgeHz(b + 1);
                if (lo >= nyquist)
                {
                    break;
                }
                const double first = std::floor(lo / binHz);
                const double last = std::ceil((std::min)(hi, nyquist) / binHz);
                double peak = 0.0;
                for (double k = (std::max)(first, 1.0); k <= last && k < static_cast<double>(m_Size / 2); k += 1.0)
                {
                    const double hz = k * binHz;
                    if (hz + binHz * 0.5 < lo || hz - binHz * 0.5 > hi)
                    {
                        continue;
                    }
                    peak = (std::max)(peak, static_cast<double>(std::abs(m_Buffer[static_cast<size_t>(k)])) * scale);
                }
                if (peak <= 0.0)
                {
                    // 帯域に FFT のビンが無い（低域で帯が狭い）：中心に最も近いビンを使う
                    const size_t k = static_cast<size_t>(std::clamp(std::round(std::sqrt(lo * hi) / binHz), 1.0, static_cast<double>(m_Size / 2 - 1)));
                    peak = static_cast<double>(std::abs(m_Buffer[k])) * scale;
                }
                const double db = 20.0 * std::log10((std::max)(peak, 1e-6));
                bands[b] = static_cast<float>(std::clamp((db + 60.0) / 60.0, 0.0, 1.0));
            }
        }

      private:
        static void Fft(std::vector<std::complex<float>>& a)
        {
            const size_t n = a.size();
            for (size_t i = 1, j = 0; i < n; ++i)
            {
                size_t bit = n >> 1;
                for (; j & bit; bit >>= 1)
                {
                    j ^= bit;
                }
                j ^= bit;
                if (i < j)
                {
                    std::swap(a[i], a[j]);
                }
            }
            for (size_t len = 2; len <= n; len <<= 1)
            {
                const float angle = -6.28318530718f / static_cast<float>(len);
                const std::complex<float> wlen(std::cos(angle), std::sin(angle));
                for (size_t i = 0; i < n; i += len)
                {
                    std::complex<float> w(1.0f, 0.0f);
                    for (size_t k = 0; k < len / 2; ++k)
                    {
                        const auto u = a[i + k];
                        const auto v = a[i + k + len / 2] * w;
                        a[i + k] = u + v;
                        a[i + k + len / 2] = u - v;
                        w *= wlen;
                    }
                }
            }
        }

        size_t m_Size;
        std::vector<float> m_Window;
        std::vector<std::complex<float>> m_Buffer;
    };

    struct Frame
    {
        bool Valid = false; // 音声が読み込まれている
        bool Playing = false;
        double Time = 0.0;  // 音声クロック（秒）。再生中は壁時計で補間済み
        std::array<float, kBands> Bands{};
    };

    // 音声クロックの補間（テスト可能な純粋関数）。published = 最後に置かれた音声クロック、elapsed = その後の壁時計の経過
    inline double EstimateClock(double published, bool playing, double elapsedWall, double maxExtrapolation = 0.1)
    {
        if (!playing || !(elapsedWall > 0.0))
        {
            return published;
        }
        return published + (std::min)(elapsedWall, maxExtrapolation);
    }

    class Bridge
    {
      public:
        // 音声側（タイムライン）が毎フレーム呼ぶ。wallNow = 単調増加の壁時計（秒）
        static void Publish(double audioTime, bool playing, const float* bands, double wallNow)
        {
            std::lock_guard lock(Mutex());
            State& s = Get();
            s.Valid = true;
            s.Playing = playing;
            s.AudioTime = audioTime;
            s.PublishWall = wallNow;
            for (int b = 0; b < kBands; ++b)
            {
                s.Bands[static_cast<size_t>(b)] = bands ? std::clamp(bands[b], 0.0f, 1.0f) : 0.0f;
            }
        }
        static void Clear()
        {
            std::lock_guard lock(Mutex());
            Get() = State{};
        }
        // レンダリング側が毎フレーム呼ぶ
        static Frame Query(double wallNow)
        {
            std::lock_guard lock(Mutex());
            const State& s = Get();
            Frame frame;
            frame.Valid = s.Valid;
            frame.Playing = s.Playing;
            frame.Time = EstimateClock(s.AudioTime, s.Playing, wallNow - s.PublishWall);
            frame.Bands = s.Bands;
            return frame;
        }

      private:
        struct State
        {
            bool Valid = false;
            bool Playing = false;
            double AudioTime = 0.0;
            double PublishWall = 0.0;
            std::array<float, kBands> Bands{};
        };
        static State& Get()
        {
            static State state;
            return state;
        }
        static std::mutex& Mutex()
        {
            static std::mutex mutex;
            return mutex;
        }
    };
} // namespace AudioMaterial
