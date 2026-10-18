#pragma once
#include "raytracingscene.h"
#include "nrddenoiser.h"

// Shared scene/material bindings for one-bounce reflections and diffuse GI.
// Each in-flight frame owns its raw signals; NRD histories are separate per signal.
class RayReflections
{
  public:
    struct SamplingLayout
    {
        UINT Width = 0;
        UINT Height = 0;
        UINT Sequence = 0;
        bool Checkerboard = false;
    };

    static SamplingLayout GetReflectionSamplingLayout();
    static bool IsGiActive();
    static bool IsOptixGiActive();
    static bool IsOptixReflectionActive();
    static D3D12_GPU_DESCRIPTOR_HANDLE GetGiSrv(UINT frameIndex);
    static ID3D12Resource* GetGiResource();
    static ID3D12Resource* GetGiNoisyResource();
    static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap, UINT increment);
    static void Shutdown();
    static bool Execute(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene,
                        const vector<RaytracingGeometry>& geometry, const NrdFrame& frame,
                        const array<ComPtr<ID3D12Resource>, 4>& guides, bool denoise);
    static D3D12_GPU_DESCRIPTOR_HANDLE GetResultSrv(UINT frameIndex);
    static UINT GetDenoiserDispatchCount();
    static bool DidRestartHistory();
    static ID3D12Resource* GetResultResource();
    static ID3D12Resource* GetNoisyResource();
    // Human readable state of the reflection / realtime GI pipeline (editor diagnostics).
    static const char* GetStatus();
    static const char* GetLastShaderError();
};
