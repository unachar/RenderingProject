#pragma once
#include <cstdint>
#include <vector>

// Stage 2 H: fixture scenes for the reference path tracer. Builds
// the exact buffer contents ReferencePathTraceCS consumes
// (combined positions/indices, per-instance transforms/materials,
// analytic lights) on the CPU so they can be unit-tested without a
// GPU and uploaded verbatim by the dispatch host.
namespace ReferenceFixture
{
    struct SceneData
    {
        std::vector<float> Positions;          // float3 per vertex
        std::vector<uint32_t> Indices;
        std::vector<uint32_t> GeometryInfo;    // uint4 per geometry: index start, vertex start, index count, unused
        std::vector<float> InstanceTransforms0; // float4 x2 per instance: matrix rows 0, 1
        std::vector<float> InstanceTransforms1; // float4 x2 per instance: matrix row 2, translation (w unused)
        std::vector<uint32_t> InstanceInfo;     // uint4 per instance: geometry, material, unused, unused
        std::vector<float> MaterialData;        // float4 x3 per material
        std::vector<float> Lights;              // float4 x2 per light
        float CameraPosition[3] = {0.0f, 0.0f, 0.0f};
        float CameraTarget[3] = {0.0f, 0.0f, 0.0f};
        float CameraFovYRadians = 0.0f;
    };

    // Classic Cornell box: white room, red left / green right walls,
    // emissive ceiling quad + point light, one tall and one short
    // white box. Deterministic: identical bytes on every call.
    SceneData BuildCornellBox();

    // Material test scene: a white room with three spheres on the
    // floor - rough gold metal, glossy red dielectric, rough white
    // diffuse - under a ceiling light quad + point light. Exercises
    // the GGX lobe across roughness/metallic on curved geometry.
    SceneData BuildMaterialTest();
} // namespace ReferenceFixture
