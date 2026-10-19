#pragma once
#include "rendererstate.h"

// 深度ピーリングによる薄い誘電体の層。不透明な遮蔽物とは別に保存する。
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
    static void Shutdown();

  private:
    static bool CreateResources();
};
