#pragma once
#include "pch.h"
#include <memory>

enum class NrdSignal
{
    Shadow,
    Specular,
    AmbientOcclusion,
    Diffuse,
    MegaLights,
    SpecularCheckerboard
}; // MegaLights: REBLUR_DIFFUSE（チェッカーボードなし）

struct NrdTexture
{
    ID3D12Resource* Resource = nullptr;
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};

struct NrdInputs
{
    NrdTexture Motion;
    NrdTexture NormalRoughness;
    NrdTexture ViewZ;
    NrdTexture NoisySignal;
};

struct NrdFrame
{
    XMFLOAT4X4 View{};
    XMFLOAT4X4 Projection{};
    XMFLOAT4X4 PreviousView{};
    XMFLOAT4X4 PreviousProjection{};
    XMFLOAT2 Jitter{};
    XMFLOAT2 PreviousJitter{};
    XMFLOAT3 LightDirection{};
    UINT FrameIndex = 0; // 安全に再利用できるエンジンのフレームスロット
    UINT Sequence = 0;   // 時間方向に連続するフレーム番号
    float Range = 2000.0f;
    float AmbientRadius = 1.5f;
    bool StableProfile = false; // 所有者ごとに明示的に選択する。実時間時計に依存しない固定60 Hz のデノイズ基準。
    bool ResetHistory = false;
    uint32_t HistoryResetReasons = 0; // 共通契約で定義された TemporalHistory::Reason のビットマスク。
};

// NVIDIA NRD のネイティブ D3D12 統合。信号とライトごとに時間履歴を所有する。
// Execute は入力状態を復元し、結果を PIXEL_SHADER_RESOURCE 状態で返す。
class NrdDenoiser
{
  public:
    NrdDenoiser();
    ~NrdDenoiser();
    NrdDenoiser(const NrdDenoiser&) = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;
    bool Initialize(ID3D12Device* device, NrdSignal signal, UINT width, UINT height);
    bool Execute(ID3D12GraphicsCommandList* commands, const NrdInputs& inputs, const NrdFrame& frame);
    ID3D12Resource* GetOutput() const;
    UINT GetDispatchCount() const;
    bool DidRestartHistory() const;
    static bool IsCompiled();
    static const char* GetVersion();

  private:
    struct Impl;
    unique_ptr<Impl> m_Impl;
};
