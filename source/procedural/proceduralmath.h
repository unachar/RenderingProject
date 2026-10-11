#pragma once

#include <cstdint>
#include <vector>

namespace Procedural
{
enum class NoiseMethod
{
    Perlin,
    Value,
    Worley,
    Fractal,
    Ridged,
    DomainWarp
};

struct NoiseSettings
{
    NoiseMethod Method = NoiseMethod::Fractal;
    uint32_t Seed = 42;
    float Frequency = 0.08f;
    int Octaves = 5;
    float Persistence = 0.5f;
    float Lacunarity = 2.0f;
    float WarpStrength = 1.5f;
};

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

float SampleNoise(float x, float y, const NoiseSettings& settings);
std::vector<float> BuildHeightMap(const TerrainSettings& settings);
Vector3 SampleMotion(MotionMethod method, double time, float speed,
    const Vector3& amplitude, uint32_t seed);
bool TerrainSettingsEqual(const TerrainSettings& first, const TerrainSettings& second);
}
