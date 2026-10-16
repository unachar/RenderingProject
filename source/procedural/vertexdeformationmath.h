#pragma once

#include "proceduralmath.h"

namespace Procedural
{
struct VertexMotionSettings
{
    MotionMethod Method = MotionMethod::Wave;
    Vector3 Amplitude = { 0.0f, 2.0f, 0.0f };
    float Speed = 0.2f;
    float SpatialFrequency = 0.3f;
    uint32_t Seed = 42;
};

Vector3 DeformPosition(const Vector3& basePosition, const VertexMotionSettings& settings, double time);
Vector3 DeformNormal(const Vector3& basePosition, const Vector3& baseNormal,
    const VertexMotionSettings& settings, double time);
Vector3 GetDeformationBoundsPadding(const VertexMotionSettings& settings);
bool VertexMotionSettingsEqual(const VertexMotionSettings& first, const VertexMotionSettings& second);
}

