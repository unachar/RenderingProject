#pragma once
#include "gpuscenebuffer.h"
#include <DirectXMath.h>

class GpuSceneCulling
{
  public:
    void Prepare(ID3D12Device *device, uint32_t slots, uint32_t frame);
    void ExecuteCulling(ID3D12GraphicsCommandList *commands, ID3D12Resource *instances, ID3D12Resource *meshes,
                        ID3D12Resource *materials, uint32_t slots, uint32_t meshCount, uint32_t materialCount);
    GpuSceneBuffer Visibility;

  private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_Root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_Pipeline;
};

