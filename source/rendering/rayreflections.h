#pragma once
#include "raytracingscene.h"
#include "nrddenoiser.h"

// 1バウンス反射と拡散 GI で共有するシーン・マテリアルのバインド。
// 実行中の各フレームは生の信号を所有し、NRD の履歴は信号ごとに分ける。
class RayReflections
{
  public:
    struct SamplingLayout
    {
        UINT Width = 0;
        UINT Height = 0;
        UINT Sequence = 0;
        bool Checkerboard = false;
    };

    static SamplingLayout GetReflectionSamplingLayout();
    static bool IsGiActive();
    static bool IsOptixGiActive();
    static bool IsOptixReflectionActive();
    static D3D12_GPU_DESCRIPTOR_HANDLE GetGiSrv(UINT frameIndex);
    static ID3D12Resource* GetGiResource();
    static ID3D12Resource* GetGiNoisyResource();
    static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap, UINT increment);
    static void Shutdown();
    static bool Execute(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene,
                        const vector<RaytracingGeometry>& geometry, const NrdFrame& frame,
                        const array<ComPtr<ID3D12Resource>, 4>& guides, bool denoise);
    static D3D12_GPU_DESCRIPTOR_HANDLE GetResultSrv(UINT frameIndex);
    static UINT GetDenoiserDispatchCount();
    static bool DidRestartHistory();
    static ID3D12Resource* GetResultResource();
    static ID3D12Resource* GetNoisyResource();
    // 反射とリアルタイム GI パイプラインの状態を、人が読める形で表す（エディター診断）。
    static const char* GetStatus();
    static const char* GetLastShaderError();
    static UINT GetLumenResidentMeshes();
    static UINT64 GetLumenMemoryBytes();
    static ID3D12Resource* GetLumenLightingResource();
};
