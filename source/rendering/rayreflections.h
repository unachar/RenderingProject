#pragma once
#include "raytracingscene.h"
#include "nrddenoiser.h"

// One-bounce scene reflections. Every in-flight frame owns its bindings and raw signal.
class RayReflections
{
public:
    static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap, UINT increment);
    static void Shutdown();
    static bool Execute(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene,
        const std::vector<RaytracingGeometry>& geometry, const NrdFrame& frame,
        const std::array<ComPtr<ID3D12Resource>, 4>& guides, bool denoise);
    static D3D12_GPU_DESCRIPTOR_HANDLE GetResultSrv(UINT frameIndex);
    static UINT GetDenoiserDispatchCount();
    static bool DidRestartHistory();
    static ID3D12Resource* GetResultResource();
    static ID3D12Resource* GetNoisyResource();
};
