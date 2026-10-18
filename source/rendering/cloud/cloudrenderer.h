#pragma once
#include "cloudsettings.h"
#include "pch.h"

// Uses the renderer's camera/light buffers and descriptor heaps; owns no scene state.
class CloudRenderer
{
  public:
    static constexpr UINT kRtvCount = 2 * 3;
    static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap, UINT increment);
    static void Shutdown();
    // Called after the renderer has waited for all in-flight frames during resize.
    static void Resize(D3D12_CPU_DESCRIPTOR_HANDLE rtvStart, UINT rtvIncrement);
    // Prepare once per frame before deferred lighting; Render reuses the same density/wind state.
    static void PrepareLighting(ID3D12GraphicsCommandList* commands, UINT frameIndex, UINT width, UINT height,
                                float deltaTime);
    static void BindLighting(ID3D12GraphicsCommandList* commands, UINT frameIndex);
    static void BindForwardLighting(ID3D12GraphicsCommandList* commands, UINT frameIndex);
    static void Render(ID3D12GraphicsCommandList* commands, UINT frameIndex, UINT width, UINT height,
                       D3D12_GPU_DESCRIPTOR_HANDLE depth, D3D12_GPU_DESCRIPTOR_HANDLE material,
                       D3D12_GPU_VIRTUAL_ADDRESS cameraConstants, D3D12_GPU_VIRTUAL_ADDRESS lightConstants,
                       D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv, DXGI_FORMAT sceneFormat);
    static CloudSettings& GetSettings();
    static void GenerateNoise(uint32_t seed);
    static void DrawDebugUI();
    static bool IsAvailable();
    static ID3D12Resource* GetCloudResource(UINT frameIndex);
    static ID3D12Resource* GetBaseNoiseResource();
    static UINT GetNoiseGenerationCount();
};
