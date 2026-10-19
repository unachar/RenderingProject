#pragma once
#include "lumenscheduler.h"
#include "rendererstate.h"
#include "raytracingscene.h"

// Persistent surface atlases; per-frame uploads follow the renderer's frame-slot fences.
class LumenSurfaceCache
{
  public:
    static constexpr UINT Capacity = 256, CardSize = 16, AtlasWidth = CardSize * 6;
    static constexpr UINT AtlasHeight = CardSize * Capacity;
    static constexpr UINT DescriptorCount = 14;
    struct Constants
    {
        XMUINT4 Counts{}; // enabled, captures, lighting updates, reserved
        XMUINT4 Grid{};   // probe columns, rows, spacing, capacity
        XMFLOAT4 Settings{}; // history limit, debug view, ray visibility, multi-bounce
    };
    bool Prepare(ID3D12Device* device, UINT frame, const vector<RaytracingGeometry>& geometry,
                 const XMFLOAT3& camera, UINT width, UINT height, UINT spacing, UINT captureBudget,
                 UINT lightingBudget, UINT debugView, bool resetLighting, UINT sequence, bool changingScene);
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
    // position, normal, bounded albedo, emission, total E A/B, indirect E A/B
    array<ComPtr<ID3D12Resource>, 8> m_Atlases;
    ComPtr<ID3D12Resource> m_Bounds, m_Probes, m_ProbeGuides;
    Constants m_Constants;
    UINT m_ProbeCount = 0, m_Read = 0;
    array<UINT, Capacity> m_LastUpdate{};
    UINT m_LastSceneChange = 0;
    bool m_Clear = true;
    // Slots written to the write atlas last frame; only those rows differ after the ping-pong swap.
    vector<UINT> m_DirtySlots;
    // Screen probe history: double-buffered records, invalidated on grid change or lighting reset.
    UINT m_ProbeParity = 0, m_ProbeColumns = 0, m_ProbeRows = 0, m_ProbeSpacing = 0;
    bool m_ProbeHistory = false;
    bool m_FullCopy = true;
};

