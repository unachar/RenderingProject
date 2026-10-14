#pragma once
#include "rendererstate.h"

// Depth-peeled thin dielectric sheets, stored separately from opaque occluders.
class TransmittanceShadows : protected RendererState
{
public:
    static void PrepareFrame();
    static bool HasCasters();
    static bool IsActive();
    static bool BeginPass(UINT shadowIndex, UINT peel);
    static void EndPass();
    static void BindCurrentPass(ID3D12GraphicsCommandList* commands);
    static void BindLighting(ID3D12GraphicsCommandList* commands, UINT colorRoot, UINT depthRoot);
    static ID3D12PipelineState* GetPipelineState();
    static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentConstantBufferAddress();
    static ID3D12Resource* GetColorResource();
    static ID3D12Resource* GetDepthResource();
    static void Shutdown();
private:
    static bool CreateResources();
};
