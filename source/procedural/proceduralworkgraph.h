#pragma once

#include "proceduralstudio.h"
#include "proceduralmath.h"

struct Vertex;

namespace ProceduralWorkGraph
{
    struct MotionInput
    {
        double Time = 0;
        float Speed = 0;
        uint32_t Method = 0;
        Procedural::Vector3 Amplitude;
        uint32_t Seed = 0;
    };

    std::vector<Procedural::Vector3> SampleMotionBatch(const std::vector<MotionInput>& samples);
    void BakeSolidVertexColors(ProcStudio::Mesh& mesh, const ProcStudio::TextureParams& parameters, float scale);
    // Explicit developer diagnostic. Runs GPU authoring jobs and compares their readback with scalar references.
    bool ValidateAuthoringOutputs();
    std::vector<float> BuildHeightMap(const Procedural::TerrainSettings& settings);
    std::vector<Vertex> BuildTerrainVertices(const Procedural::TerrainSettings& settings);
    ProcStudio::Mesh GenerateGridModel(const ProcStudio::ModelParams& parameters);
    ProcStudio::Mesh GenerateAnalyticModel(const ProcStudio::ModelParams& parameters);
    ProcStudio::Mesh GenerateTree(const ProcStudio::ModelParams& parameters);
    ProcStudio::Mesh GenerateLeaf(const ProcStudio::LeafParams& parameters, uint32_t seed, bool orientForModel = false);
    ProcStudio::Image GenerateTexture(const ProcStudio::TextureParams& parameters, int width, int height);
    ProcStudio::Volume GenerateVolume(const ProcStudio::TextureParams& parameters, int extent);
    std::vector<ProcStudio::Float4> GenerateSolidVolume(const ProcStudio::TextureParams& parameters, int extent);
    ProcStudio::Volume VolumeFromImage(const ProcStudio::Image& image, int extent, int octaves, float verticalVariation,
                                       uint32_t seed);
}
