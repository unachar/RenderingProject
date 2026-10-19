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

    struct SamplePair
    {
        float x;
        float y;
    };

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

    // 実時間に依存しない、画面上で安定したスクランブルと範囲内の R2/R4 系列。
    // R4 は発光体の選択と三角形座標を1つの積分領域にまとめる。
    inline SamplePair SampleSequence(SampleUint x, SampleUint y, SampleUint index, SampleUint domain)
    {
        SampleUint scramble = SampleHash(x ^ SampleHash(y + domain * 0x9e3779b9u));
        float phase = float((index & 4095u) + 1u);
        float frequencyX = .754877666f, frequencyY = .569840291f;
        // 発光体・三角形の選択と重心座標は、単一の4次元積分問題である。
        // スクランブルだけをずらした R2 ペアを再利用すると、選択した三角形と
        // その表面のサンプル位置の間に相関が生じる。
        if (domain == 1u)
        {
            frequencyX = .856674884f;
            frequencyY = .733891856f;
        }
        if (domain == 2u)
        {
            frequencyX = .628706722f;
            frequencyY = .538597257f;
        }
        SamplePair result;
        result.x = SampleFraction(float(scramble & 65535u) / 65536.0f + phase * frequencyX);
        result.y = SampleFraction(float(scramble >> 16) / 65536.0f + phase * frequencyY);
        return result;
    }

    inline SampleUint PixelSampleIndex(SampleUint frame, bool checkerboard)
    {
        // チェッカーボードのピクセルは1フレームおきに追跡する。frame を直接使うと
        // 低食い違い系列の半分を飛ばしてしまう。
        return checkerboard ? frame >> 1 : frame;
    }
#ifdef __cplusplus
}
#endif
