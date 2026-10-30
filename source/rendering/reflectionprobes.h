#pragma once
#include <cstdint>
#include <vector>

struct ID3D12GraphicsCommandList;

// Stage 2 J: local reflection probes (see
// docs/rendering-technology-stage2j-design.md). The registry holds
// the frame's probes with their resolved environment SRV indices;
// selection picks the probe that overrides the global environment
// texture while the camera is inside its influence box.
namespace ReflectionProbes
{
    struct Probe
    {
        float Center[3] = {0.0f, 0.0f, 0.0f};
        float HalfExtents[3] = {5.0f, 2.5f, 5.0f};
        // Absolute engine SRV index of the prefiltered equirect
        // environment texture (TextureManager::LoadTexture result),
        // or -1 when unresolved.
        int EnvironmentSrvIndex = -1;
        int Priority = 0;
        float Intensity = 1.0f;
    };

    void Clear();
    void Add(const Probe& probe);
    uint32_t Count();
    // Rebuilds the registry from the scene's ReflectionProbeComponents.
    void SyncFromScene(ID3D12GraphicsCommandList* commandList);

    // Per-frame entry (RenderFrame::BeginDraw): syncs the registry,
    // selects the probe containing the camera and writes the result
    // to RendererState's environment override. Clears the override
    // whenever the feature flag is off.
    void UpdateActiveEnvironment(ID3D12GraphicsCommandList* commandList);

    // Runs the GGX prefilter (EnvironmentPrefilterCS) over the source
    // texture and registers the result with TextureManager under
    // `registrationName`. Returns the prefiltered texture's SRV
    // index, or the source index unchanged when the source already
    // has a full mip chain or generation fails.
    int GeneratePrefilteredEnvironment(ID3D12GraphicsCommandList* commandList, int sourceSrvIndex,
                                       const char* registrationName);

    // Live capture step 1: converts a captured cubemap (6+ slices)
    // into an equirect texture registered under registrationName,
    // ready to feed GeneratePrefilteredEnvironment. Returns the
    // engine SRV index or -1 on failure.
    int ConvertCubeToEquirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* cubeResource,
                              uint32_t width, uint32_t height, const char* registrationName);

    // Returns the environment SRV index of the active probe for the
    // given camera position, or -1 when no probe contains it.
    // Selection: containing probes only; highest priority wins; ties
    // go to the nearest center.
    int SelectEnvironmentSrvIndex(const float cameraPosition[3]);
    // The probe selected by the most recent UpdateActiveEnvironment
    // (box-projection data for the lighting shader). Returns false
    // when no probe is active this frame.
    bool GetActiveProbeBox(float center[3], float halfExtents[3], float& intensity);
    // The two best probes containing the point (highest priority,
    // then nearest center), for the per-pixel blending route in
    // docs/rendering-technology-stage2j-blending-design.md. Writes
    // up to two entries into `out` (each with its bindless-relative
    // texture index in `outBindlessIndices`) and returns the count.
    int SelectTopProbes(const float point[3], Probe out[2], int outBindlessIndices[2]);
    // The frame's probe set as chosen by the most recent
    // UpdateActiveEnvironment (same data as SelectTopProbes at the
    // camera position). Returns the count (0, 1 or 2).
    int GetFrameProbes(Probe out[2], int outBindlessIndices[2]);

    const std::vector<Probe>& All();
} // namespace ReflectionProbes
