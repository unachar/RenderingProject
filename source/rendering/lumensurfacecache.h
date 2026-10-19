#pragma once
#include "lumenscheduler.h"
#include "rendererstate.h"
#include "raytracingscene.h"

// 永続的なサーフェスアトラス。フレームごとのアップロードは描画側のスロットフェンスに従う。
class LumenSurfaceCache
{
  public:
    static constexpr UINT Capacity = 256, CardSize = 16, AtlasWidth = CardSize * 6;
    static constexpr UINT AtlasHeight = CardSize * Capacity;
    static constexpr UINT DescriptorCount = 14;

    struct Constants
    {
        XMUINT4 Counts{};    // 有効、取得数、照明更新数、予約領域
        XMUINT4 Grid{};      // プローブの列数、行数、間隔、容量
        XMFLOAT4 Settings{}; // 履歴上限、デバッグ表示、レイ可視性、複数バウンス
    };

    bool Prepare(ID3D12Device* device, UINT frame, const vector<RaytracingGeometry>& geometry, const XMFLOAT3& camera,
                 UINT width, UINT height, UINT spacing, UINT captureBudget, UINT lightingBudget, UINT debugView,
                 bool resetLighting, UINT sequence, bool changingScene);
    void WriteDescriptors(ID3D12Device* device, UINT frame, D3D12_CPU_DESCRIPTOR_HANDLE start, UINT increment);
    D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress(UINT frame) const;
    void BeginLighting(ID3D12GraphicsCommandList* commands);
    void Barrier(ID3D12GraphicsCommandList* commands) const;
    void EndLighting(ID3D12GraphicsCommandList* commands);

    void NotifySceneChanged(UINT sequence)
    {
        m_LastSceneChange = sequence;
    }

    const Constants& GetConstants() const
    {
        return m_Constants;
    }

    bool NeedsLightingClear() const
    {
        return m_Clear;
    }

    UINT ResidentMeshes() const;
    UINT64 MemoryBytes() const;

    ID3D12Resource* LightingResource() const
    {
        return m_Atlases[4 + m_Read].Get();
    }

    void Reset();

  private:
    struct Uploads
    {
        ComPtr<ID3D12Resource> Map, Work, Parameters;
    };

    Lumen::Scheduler m_Scheduler{Capacity};
    Lumen::Schedule m_Schedule;
    array<Uploads, RendererState::g_kFRAME_COUNT> m_Frames;
    // 位置、法線、制限済みアルベド、発光、合計 E A/B、間接 E A/B
    array<ComPtr<ID3D12Resource>, 8> m_Atlases;
    ComPtr<ID3D12Resource> m_Bounds, m_Probes, m_ProbeGuides;
    Constants m_Constants;
    UINT m_ProbeCount = 0, m_Read = 0;
    array<UINT, Capacity> m_LastUpdate{};
    UINT m_LastSceneChange = 0;
    bool m_Clear = true;
    // 前フレームで書き込みアトラスに書いたスロット。交換後に差があるのはこれらの行だけ。
    vector<UINT> m_DirtySlots;
    // 画面プローブの履歴は二重バッファで保持し、グリッド変更または照明リセット時に無効化する。
    UINT m_ProbeParity = 0, m_ProbeColumns = 0, m_ProbeRows = 0, m_ProbeSpacing = 0;
    bool m_ProbeHistory = false;
    bool m_FullCopy = true;
};
