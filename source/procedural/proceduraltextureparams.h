#pragma once
#include "proceduralstudio.h"
#include <cstddef>

namespace ProcStudio
{
    struct TextureGpuParams
    {
        uint32_t Width, Height, Kind, Output;
        uint32_t BasisType, Fractal, Seed, Tileable;
        float Frequency;
        int Octaves;
        float Lacunarity, Gain;
        float WarpStrength, RidgeOffset, Contrast, Bias;
        Float4 ColorA, ColorB, ColorC;
        float Turbulence, RingCount, Mortar, Time;
        int BricksX, BricksY, Pattern, RepeatX;
        int RepeatY;
        float PatternSize, PatternSoftness, NormalStrength;
        float Choppiness, Coverage, CloudSoftness, Padding = 0;
        uint32_t PreviewSize = 0, Tiles = 1, Padding1 = 0, Padding2 = 0;
        float Direction = 0, Anisotropy = 4, KernelFrequency = 2, Phase = 0;
    };

    static_assert(sizeof(TextureGpuParams) == 208);
    static_assert(offsetof(TextureGpuParams, Direction) == 192);

    inline TextureGpuParams MakeTextureGpuParams(const TextureParams& params)
    {
        TextureGpuParams result{};
        const auto& n = params.Noise;
        result = {0,
                  0,
                  static_cast<uint32_t>(params.Kind),
                  static_cast<uint32_t>(params.Output),
                  static_cast<uint32_t>(n.BasisType),
                  static_cast<uint32_t>(n.Fractal),
                  n.Seed,
                  params.Tileable ? 1u : 0u,
                  n.Frequency,
                  n.Octaves,
                  n.Lacunarity,
                  n.Gain,
                  n.WarpStrength,
                  n.RidgeOffset,
                  params.Contrast,
                  params.Bias,
                  {params.ColorA[0], params.ColorA[1], params.ColorA[2], 1},
                  {params.ColorB[0], params.ColorB[1], params.ColorB[2], 1},
                  {params.ColorC[0], params.ColorC[1], params.ColorC[2], 1},
                  params.Turbulence,
                  params.RingCount,
                  params.Mortar,
                  params.Time,
                  params.BricksX,
                  params.BricksY,
                  static_cast<int>(params.Pattern),
                  params.RepeatX,
                  params.RepeatY,
                  params.PatternSize,
                  params.PatternSoftness,
                  params.NormalStrength,
                  params.Choppiness,
                  params.Coverage,
                  params.Softness};
        result.Direction = n.Direction;
        result.Anisotropy = n.Anisotropy;
        result.KernelFrequency = n.KernelFrequency;
        result.Phase = n.Phase;
        return result;
    }
}
