#pragma once
#include "gpuscenetypes.h"
#include <d3d12.h>
#include <vector>

class GpuScene
{
  public:
    static void Execute(ID3D12GraphicsCommandList* commands);
    static void Shutdown(); // Caller has already waited for GraphicsDevice's fences.
    static GPUInstanceID GetEntityInstance(uint32_t entity);
    static const vector<GPUInstanceID>& GetEntityInstances(uint32_t entity);
    static const vector<uint32_t>& CpuOpaqueStaticEntities();
    static ID3D12Resource* InstanceBuffer();
    static ID3D12Resource* MeshBuffer();
    static ID3D12Resource* MaterialBuffer();
    static ID3D12Resource* VisibilityBuffer();
    static const GpuSceneStatistics& Statistics();
    static const GPUInstanceData* GetInstance(GPUInstanceID id);
    static uint64_t InstanceHistoryGeneration(GPUInstanceID id);
    static uint32_t ResolveEntity(GPUInstanceID id, uint64_t generation);
    static void DrawDebug();
};
