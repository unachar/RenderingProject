#pragma once
#include "cloudsettings.h"
#include "pch.h"

// 描画側のカメラ・ライトバッファと記述子ヒープを使い、シーン状態は所有しない。
class CloudRenderer
{
  public:
    static constexpr UINT kRtvCount = 2 * 3;
    static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap, UINT increment);
    static void Shutdown();
    // リサイズ時に、描画側が実行中の全フレームを待ち終わってから呼ぶ。
    static void Resize(D3D12_CPU_DESCRIPTOR_HANDLE rtvStart, UINT rtvIncrement);
    // 遅延ライティングの前に毎フレーム1回準備し、Render は同じ密度・風の状態を再利用する。
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
    // 生成したベース形状ノイズをユーザーボリュームに置き換える（基準サイズへ再サンプリングし、端は反復）。
    // シードが変わっても ClearCustomBaseNoise まで有効なままにする。
    static void SetCustomBaseNoise(const std::vector<float>& density, int extent, const std::string& label);
    static void ClearCustomBaseNoise();
    static bool HasCustomBaseNoise();
    static void DrawDebugUI();
    static bool IsAvailable();
    static ID3D12Resource* GetCloudResource(UINT frameIndex);
    static ID3D12Resource* GetBaseNoiseResource();
    static UINT GetNoiseGenerationCount();
};
