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

    // Returns the environment SRV index of the active probe for the
    // given camera position, or -1 when no probe contains it.
    // Selection: containing probes only; highest priority wins; ties
    // go to the nearest center.
    int SelectEnvironmentSrvIndex(const float cameraPosition[3]);

    const std::vector<Probe>& All();
} // namespace ReflectionProbes
