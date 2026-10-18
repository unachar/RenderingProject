#pragma once
#include "nrddenoiser.h"

// D3D12 packing/dispatch integration. OptixInterop owns CUDA/OptiX resources.
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
