#pragma once
#include "rendererstate.h"
#include "rendergraph.h"
#include "graphicsdevice.h"
#include "lightingresources.h"
#include "camera.h"
#include "megalights.h"
#include <d3d12.h>

/// <summary>
/// クラスタード・ライトグリッドの管理・構築を行う統合システム。
/// Deferred と Forward+ の両方から参照される共通リソースを提供する。
/// </summary>
class ClusteredLighting
{
public:
    struct GridConstants
    {
        UINT TilesX;
        UINT TilesY;
        UINT Slices;
        UINT MaxLightsPerTile;
        float SliceFar;
        float NearClip;
        float FarClip;
        float Padding;
        XMFLOAT4X4 View;
        XMFLOAT4X4 Proj;
        XMFLOAT4X4 InvView;
        XMFLOAT4X4 InvProj;
    };

    struct Settings
    {
        bool Enabled = true;
        UINT TileSize = 16;
        UINT DepthSlices = 32;
        UINT MaxLightsPerTile = 256;
        float SliceFar = 100.0f;
        bool BuildForForward = true;  // フォワード用にもグリッドを構築する（視錐台全体）
        bool BuildForVolumetric = false;
    };

    struct Resources
    {
        ComPtr<ID3D12Resource> LightIndexBuffer;     // タイルごとのライトインデックスリスト
        ComPtr<ID3D12Resource> LightGridCounter;     // タイルごとのライトカウント
        ComPtr<ID3D12Resource> LightGridConstants;   // 定数バッファ
        ComPtr<ID3D12Resource> LightDataBuffer;      // GPU側ライトデータ (StructuredBuffer)
        D3D12_GPU_VIRTUAL_ADDRESS LightIndexBufferAddr = 0;
        D3D12_GPU_VIRTUAL_ADDRESS LightGridCounterAddr = 0;
        D3D12_GPU_VIRTUAL_ADDRESS LightGridConstantsAddr = 0;
        D3D12_GPU_VIRTUAL_ADDRESS LightDataBufferAddr = 0;
        UINT TilesX = 0, TilesY = 0, Slices = 0;
        UINT MaxLightsPerTile = 0;
        bool Valid = false;
    };

    static Settings& GetSettings() { return s_Settings; }
    static const Resources& GetResources() { return s_Resources; }
    static Resources& GetMutableResources() { return s_Resources; }

    /// <summary>フレーム開始時の初期化（RenderGraph構築前）</summary>
    static void PrepareFrame(RenderGraph& graph, RenderGraph::ResourceHandle gpuScene, UINT width, UINT height);

    /// <summary>クラスタード・ライトグリッド構築パスをRenderGraphに追加</summary>
    static RenderGraph::ResourceHandle AddLightGridBuildPass(
        RenderGraph& graph,
        RenderGraph::ResourceHandle gpuScene,
        RenderGraph::ResourceHandle depthBuffer,
        UINT width, UINT height);

    /// <summary>ディファード・ライティング用のライトグリッドバインド</summary>
    static void BindForDeferred(ID3D12GraphicsCommandList* cmdList, UINT gridParam, UINT lightsParam, 
                                UINT counterParam, UINT constantsParam);

    /// <summary>フォワード+用のライトグリッドバインド</summary>
    static void BindForForward(ID3D12GraphicsCommandList* cmdList, UINT gridParam, UINT lightsParam,
                               UINT counterParam, UINT constantsParam);

    /// <summary>コンピュートシェーダー用バインド（ボリューメトリック等）</summary>
    static void BindForCompute(ID3D12GraphicsCommandList* cmdList, UINT gridParam, UINT lightsParam,
                               UINT counterParam, UINT constantsParam);

    /// <summary>HLSL共有コード用の定数取得</summary>
    static GridConstants GetGridConstants(UINT width, UINT height);

    /// <summary>リソースリサイズ</summary>
    static void Resize(UINT width, UINT height);

    /// <summary>シャットダウン</summary>
    static void Shutdown();

private:
    static Settings s_Settings;
    static Resources s_Resources;
    static ComPtr<ID3D12RootSignature> s_BuildRootSignature;
    static ComPtr<ID3D12PipelineState> s_BuildCSO;
    static ComPtr<ID3D12PipelineState> s_ClearCountersPSO;
    static UINT s_FrameIndex;
    static UINT s_CurrentWidth, s_CurrentHeight;

    static void EnsureResources(UINT width, UINT height);
    static void CreatePipelineStates(ID3D12Device* device);
    static void BuildLightGridCS(ID3D12GraphicsCommandList* cmdList, RenderGraph& graph,
                                 RenderGraph::ResourceHandle gpuScene,
                                 RenderGraph::ResourceHandle depthBuffer);
};