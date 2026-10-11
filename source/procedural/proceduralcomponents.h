#pragma once

#include "proceduralmath.h"
#include <DirectXMath.h>

struct ProceduralTerrainComponent
{
    Procedural::TerrainSettings Settings;
};

struct ProceduralMotionComponent
{
    Procedural::MotionTarget Target = Procedural::MotionTarget::Object;
    Procedural::MotionMethod Method = Procedural::MotionMethod::Wave;
    DirectX::XMFLOAT3 Origin{};
    DirectX::XMFLOAT3 Amplitude = { 0.0f, 2.0f, 0.0f };
    float Speed = 0.2f;
    float SpatialFrequency = 0.3f;
    uint32_t Seed = 42;
    double Time = 0.0;
    bool Enabled = true;
    bool PreviewInEditor = false;
};
