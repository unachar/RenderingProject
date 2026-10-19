#pragma once
#include "nrddenoiser.h"

// D3D12 のパック処理とディスパッチを統合する。CUDA/OptiX リソースは OptixInterop が所有する。
class OptixDenoiser
{
  public:
    OptixDenoiser();
    ~OptixDenoiser();
    static bool Initialize(ID3D12Device* device);
    static void Shutdown();
    static bool IsAvailable();
    static const char* GetStatus();
    bool Execute(ID3D12GraphicsCommandList* commands, ID3D12Resource* noisy, ID3D12Resource* depth,
                 ID3D12Resource* normal, ID3D12Resource* result, const NrdFrame& frame, bool nrdPacked,
                 bool preserveHitDistance, float intensity);

  private:
    struct Impl;
    unique_ptr<Impl> m_Impl;
};
