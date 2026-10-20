#pragma once
#include "gpuscenetypes.h"
#include <d3d12.h>
#include <vector>

class GpuScene
{
  public:
    static void Execute(ID3D12GraphicsCommandList* commands);
    static void Shutdown(); // 呼び出し側は GraphicsDevice のフェンス完了を待ち終わっている。
    static GPUInstanceID GetEntityInstance(uint32_t entity);
    static const vector<GPUInstanceID>& GetEntityInstances(uint32_t entity);
    static const vector<uint32_t>& CpuOpaqueStaticEntities();
    static ID3D12Resource* InstanceBuffer();
    static ID3D12Resource* MeshBuffer();
    static ID3D12Resource* MaterialBuffer();
    static ID3D12Resource* VisibilityBuffer();
    static const GpuSceneStatistics& Statistics();
    static const GPUInstanceData* GetInstance(GPUInstanceID id);
    static uint64_t InstanceHistoryGeneration(GPUInstanceID id);
    static uint32_t ResolveEntity(GPUInstanceID id, uint64_t generation);
    static void DrawDebug();
    // エンティティの全インスタンスが GPU 不透明描画（ビット2）の対象か。
    static bool EntityUsesGpuOpaque(uint32_t entity);
    // インポスター置き換えの準備状態を全インスタンスへ書く（ビット5・切替距離・モデル中心）。
    // 距離の判定は GPU（GPUOcclusionCS）が行う。値が変わった場合のみ転送対象になる。
    static void SetEntityImpostor(uint32_t entity, bool ready, const DirectX::XMFLOAT3& localCenter, float distance);
};

