#pragma once
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace ReferenceFixture
{
    struct SceneData;
}

// Stage 2 H: GPU host for the reference path tracer. Step 4b-1
// uploads a fixture scene and builds its BLAS/TLAS; the dispatch
// and readback land in step 4b-2.
namespace ReferenceIntegrator
{
    // Uploads the fixture buffers and records the acceleration
    // structure builds into `commandList` (the caller submits and
    // waits before dispatching). Rebuilding releases the previous
    // scene. Returns false on any failure.
    bool BuildFixtureScene(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                           const ReferenceFixture::SceneData& scene);
    void ReleaseScene();
    bool HasScene();
    uint32_t InstanceCount();
    uint32_t LightCount();
    // GPU addresses for the dispatch step: the TLAS and the scene
    // data buffers in shader register order (t1..t8 indices 0..7).
    uint64_t GetTlasAddress();
    uint64_t GetSceneBufferAddress(int index);
} // namespace ReferenceIntegrator
