#pragma once

#include "proceduralstudio.h"
#include "proceduralmath.h"

namespace ProceduralWorkGraph
{
    std::vector<float> BuildHeightMap(const Procedural::TerrainSettings& settings);
    ProcStudio::Mesh GenerateGridModel(const ProcStudio::ModelParams& parameters);
    ProcStudio::Mesh GenerateAnalyticModel(const ProcStudio::ModelParams& parameters);
    ProcStudio::Image GenerateTexture(const ProcStudio::TextureParams& parameters, int width, int height);
    ProcStudio::Volume GenerateVolume(const ProcStudio::TextureParams& parameters, int extent);
    std::vector<ProcStudio::Float4> GenerateSolidVolume(const ProcStudio::TextureParams& parameters, int extent);
    ProcStudio::Volume VolumeFromImage(const ProcStudio::Image& image, int extent, int octaves, float verticalVariation,
                                       uint32_t seed);
}
