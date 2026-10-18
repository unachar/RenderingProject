#pragma once
#include <d3d12.h>
#include <memory>

// Each instance owns one signal's shared images and denoiser workspace.
class OptixInterop
{
  public:
    OptixInterop();
    ~OptixInterop();
    OptixInterop(const OptixInterop&) = delete;
    OptixInterop& operator=(const OptixInterop&) = delete;
    static bool Initialize(ID3D12Device* device);
    static void Shutdown();
    static bool IsAvailable();
    static const char* GetStatus();
    bool Ensure(UINT width, UINT height, UINT frameIndex);
    ID3D12Resource* GetSharedBuffer(UINT frameIndex) const;
    bool Enqueue(ID3D12CommandQueue* queue, UINT frameIndex);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
