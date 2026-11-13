#pragma once

#include "pch.h"
#include "ecs.h"

class DlssUpscaler
{
  public:
    static void Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT descriptorIncrement);
    static void Shutdown();
    static bool IsAvailable();
    static bool IsActive();
    static const char* GetStatus();
    static bool IsScaleSupported(UINT inputWidth, UINT inputHeight, UINT outputWidth, UINT outputHeight);
    static void PrepareFrame(EntityID camera);
    static bool IsRequested();
    static XMFLOAT2 GetJitter();

    struct Settings
    {
        // DLSS 後段の RCAS シャープン（0 = 無効、1 = 最大）。
        // DLSS 2.5.1 以降は内蔵シャープンが廃止されたため、くっきりさせるにはアプリ側で掛ける。
        float Sharpness = 0.35f;
        // NGX のレンダープリセット（0 = ドライバー既定、1 = A、2 = B … 11 = K）。
        // 新しいプリセット（Transformer 系）ほど低解像度入力でもくっきりする。DLL が対応しない値は既定扱い。
        int RenderPreset = 0;
    };
    static Settings& GetSettings();
    // RCAS に渡す値（stops。0 = 最強）。Sharpness が 0 なら負（掛けない）。
    static float GetSharpnessStops();
    // 投影へ適用するジッターの符号（通常 +1）。検証用に UI から反転できる。
    static float GetJitterProjectionSign();
    static bool& JitterSignFlipped();
    // NGX フィーチャーを作り直した回数（作り直し = GPU 待機のヒッチ）。
    static UINT GetFeatureRecreateCount();
    // DLSS 用に確保している VRAM（バイト）。
    static UINT64 GetResourceBytes();
    static bool Execute(ID3D12GraphicsCommandList* commands, UINT frameIndex, ID3D12Resource* color,
                        D3D12_GPU_DESCRIPTOR_HANDLE colorSrv, ID3D12Resource* depth, ID3D12Resource* velocity,
                        D3D12_GPU_DESCRIPTOR_HANDLE velocitySrv, ID3D12Resource* output,
                        D3D12_GPU_DESCRIPTOR_HANDLE outputUav);
};

