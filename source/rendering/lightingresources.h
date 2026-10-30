#pragma once
#include "rendererstate.h"
struct LightComponent;

class LightingResources : protected RendererState
{
  public:
    enum class ShadowCasterSubset
    {
        All,
        Stationary,
        Moving
    };
    static void SetShadowCasterSubset(ShadowCasterSubset subset);
    static ShadowCasterSubset GetShadowCasterSubset();
    static void SetStaticShadowCacheAvailable(bool available);
    static bool UseStaticShadowCache(UINT shadowIndex);
    static bool ShouldRenderStaticShadowPass(UINT shadowIndex);

    struct LightGridStats
    {
        UINT AuthoredLights = 0;
        UINT ActivePhysicalLights = 0;
        UINT ActiveDecalLights = 0;
        UINT OnScreenLights = 0;
        UINT GpuVisibleLights = 0;
        UINT GpuPhysicalLights = 0;
        UINT GpuDecalLights = 0;
        UINT VolumetricLights = 0;
        UINT ShadowedLights = 0;
        UINT TileCountX = 0;
        UINT TileCountY = 0;
        UINT MaxLightsPerTile = 0;
        UINT OverflowedTileAssignments = 0;
    };

    static void UpdateLightConstantBuffer(float deferredLightStrength);
    static XMFLOAT4 GetIesProfileParameters(const LightComponent& light);
    static void UpdateShadowConstantBuffer();
    static UINT GetShadowLightCount();
    static bool ShouldRenderShadowPass(UINT shadowIndex);
    static bool ShouldDrawEntityInCurrentShadowPass(EntityID entity);
    // 静的 / 動的キャスターの分割だけを判定する（視錐台の判定は GPU：GpuObjectCulling / GpuInstanceCullLodCS）。
    static bool ShouldDrawEntityInCurrentShadowSubset(EntityID entity);
    // 現在のシャドウパスのカリング用行列（転置済み）。パス外なら false（カリングしない）。
    static bool GetCurrentShadowCullMatrix(XMMATRIX& transposedViewProjection);
    // 影を落とす物ならワールド空間の境界球（xyz 中心, w 半径）を返す。GPU へ渡すためのデータ準備。
    static bool GetShadowCasterSphere(EntityID entity, XMFLOAT4& sphere);
    static bool IsCurrentShadowPassVirtualPage();
    static UINT GetCurrentShadowLodBias();
    static bool IsVirtualShadowCacheHit();
    static bool GetShadowPassInfo(UINT shadowIndex, UINT& layer, D3D12_VIEWPORT& viewport, D3D12_RECT& scissor,
                                  bool& clearLayer);
    static void SetCurrentShadowPassIndex(UINT index);
    static UINT GetCurrentShadowPassIndex();
    static XMMATRIX GetCurrentShadowViewProjection();
    // Separate (non-transposed) view/projection for a built shadow light view,
    // for consumers that need the view on its own (e.g. VG shadow traversal).
    static bool GetShadowViewParts(UINT shadowIndex, XMFLOAT4X4& outView, XMFLOAT4X4& outProjection);
    static void GetCurrentShadowLight(XMFLOAT4& positionType, XMFLOAT4& direction);
    static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentShadowConstantBufferAddress();
    static D3D12_GPU_VIRTUAL_ADDRESS GetShadowConstantBufferAddress(UINT shadowIndex);
    static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentLightConstantBufferAddress();
    static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentLightTileIndexBufferAddress();
    static D3D12_GPU_VIRTUAL_ADDRESS GetCurrentVolumetricLightIndexBufferAddress();
    static const LightGridStats& GetLightGridStats();
    static void BeginFrame();

    static ID3D12Resource* GetLightCB()
    {
        return m_LightConstantBuffer.Get();
    }

    static ID3D12Resource* GetShadowCB()
    {
        return m_ShadowConstantBuffer.Get();
    }

    static ID3D12Resource* GetShadowMapResource()
    {
        return m_ShadowDepthBuffer.Get();
    }
};

