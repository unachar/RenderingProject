#pragma once
#include "rendertargets.h"

class PostProcessPass : protected RenderTargets
{
  public:
    static void UpdateCameraConstants();
    static void ApplyPostProcess(const PostProcessComponent& config);
    static void ApplyAntiAliasing();
    static bool RegisterFullscreenPipeline(ID3D12Device* device, ID3D12PipelineState* pipeline,
                                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC& description,
                                           const wchar_t* pixelEntry = L"main");
    static bool DispatchFullscreenPipeline(ID3D12PipelineState* pipeline, ID3D12GraphicsCommandList* commands,
                                           UINT frameIndex);
};
