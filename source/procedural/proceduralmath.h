#pragma once
#include "noise.h"

#include <cstdint>
#include <array>
#include <iosfwd>
#include <vector>

namespace Procedural
{
    // 各レイヤーの方式とブレンド重みを保持しつつ、共通値をコピーする。
    void ApplyCommonNoiseParameters(NoiseSettings& settings);

    struct TerrainSettings
    {
        NoiseSettings Noise;
        int Resolution = 64;
        float Size = 40.0f;
        float Height = 5.0f;
        float BaseHeight = -2.0f;
    };

    enum class MotionMethod
    {
        Wave,
        Orbit,
        Noise
    };

    enum class MotionTarget
    {
        Object,
        MeshVertices
    };

    struct Vector3
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    std::vector<float> BuildHeightMap(const TerrainSettings& settings);
    Vector3 SampleMotion(MotionMethod method, double time, float speed, const Vector3& amplitude, uint32_t seed);
    bool TerrainSettingsEqual(const TerrainSettings& first, const TerrainSettings& second);
    bool WriteTerrainPreset(std::ostream& stream, const TerrainSettings& settings);
    bool ReadTerrainPreset(std::istream& stream, TerrainSettings& settings);
} // 名前空間 Procedural の終わり。
