#include "pch.h"
#include "clusteredlighting.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "persistentpsocache.h"
#include "renderprofiler.h"
#include "graphicsdevice.h"
#include "camera.h"
#include "world.h"
#include "lightingresources.h"
#include "frameconstants.h"
#include "renderconfiguration.h"
#include "megalights.h"
#include "gpuscene/gpuscene.h"

namespace
{
    bool LoadComputeShader(const char* path, ComPtr<ID3DBlob>& shader)
    {
        ShaderDescription description{};
        description.csoPath = path;
        description.ppBlob = shader.GetAddressOf();
        return ShaderPipelines::LoadShaderBlob(description);
    }
}

ClusteredLighting::Settings ClusteredLighting::s_Settings;
ClusteredLighting::Resources ClusteredLighting::s_Resources;
ComPtr<ID3D12RootSignature> ClusteredLighting::s_BuildRootSignature;
ComPtr<ID3D12PipelineState> ClusteredLighting::s_BuildCSO;
ComPtr<ID3D12PipelineState> ClusteredLighting::s_ClearCountersPSO;
UINT ClusteredLighting::s_FrameIndex = 0;
UINT ClusteredLighting::s_CurrentWidth = 0;
UINT ClusteredLighting::s_CurrentHeight = 0;

void ClusteredLighting::PrepareFrame(RenderGraph& graph, RenderGraph::ResourceHandle gpuScene, UINT width, UINT height)
{
    s_FrameIndex = GraphicsDevice::GetFrameIndex();
    if (width != s_CurrentWidth || height != s_CurrentHeight)
    {
        Resize(width, height);
    }
}

void ClusteredLighting::Resize(UINT width, UINT height)
{
    s_CurrentWidth = width;
    s_CurrentHeight = height;
    s_Resources.Valid = false;
    s_Resources.LightIndexBuffer.Reset();
    s_Resources.LightGridCounter.Reset();
    s_Resources.LightGridConstants.Reset();
    s_Resources.LightDataBuffer.Reset();
}

void ClusteredLighting::EnsureResources(UINT width, UINT height)
{
    if (s_Resources.Valid && s_Resources.TilesX == (width + s_Settings.TileSize - 1) / s_Settings.TileSize &&
        s_Resources.TilesY == (height + s_Settings.TileSize - 1) / s_Settings.TileSize &&
        s_Resources.Slices == s_Settings.DepthSlices)
    {
        return;
    }

    auto device = GraphicsDevice::GetDevice();
    if (!device) return;

    const UINT tilesX = (width + s_Settings.TileSize - 1) / s_Settings.TileSize;
    const UINT tilesY = (height + s_Settings.TileSize - 1) / s_Settings.TileSize;
    const UINT slices = s_Settings.DepthSlices;
    const UINT totalTiles = tilesX * tilesY * slices;

    // Light Index Buffer: 各タイルのライトインデックスリスト
    const UINT indexBufferSize = totalTiles * s_Settings.MaxLightsPerTile * sizeof(UINT);
    {
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, 
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&s_Resources.LightIndexBuffer));
        s_Resources.LightIndexBuffer->SetName(L"Clustered Light Index Buffer");
        s_Resources.LightIndexBufferAddr = s_Resources.LightIndexBuffer->GetGPUVirtualAddress();
    }

    // Light Grid Counter: 各タイルのライト数
    {
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(totalTiles * sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&s_Resources.LightGridCounter));
        s_Resources.LightGridCounter->SetName(L"Clustered Light Grid Counter");
        s_Resources.LightGridCounterAddr = s_Resources.LightGridCounter->GetGPUVirtualAddress();
    }

    // Light Grid Constants
    {
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(GridConstants));
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s_Resources.LightGridConstants));
        s_Resources.LightGridConstants->SetName(L"Clustered Light Grid Constants");
        s_Resources.LightGridConstantsAddr = s_Resources.LightGridConstants->GetGPUVirtualAddress();
    }

    // Light Data Buffer - use MegaLights buffer
    // MegaLights uses its own internal buffer

    s_Resources.TilesX = tilesX;
    s_Resources.TilesY = tilesY;
    s_Resources.Slices = slices;
    s_Resources.MaxLightsPerTile = s_Settings.MaxLightsPerTile;
    s_Resources.Valid = true;
}

void ClusteredLighting::CreatePipelineStates(ID3D12Device* device)
{
    if (s_BuildRootSignature || !device) return;

    // Root Signature for Light Grid Build CS
    CD3DX12_ROOT_PARAMETER params[8];
    params[0].InitAsConstantBufferView(0);  // GridConstants (b0)
    params[1].InitAsShaderResourceView(0);  // Light Data Buffer (t0)
    params[2].InitAsShaderResourceView(1);  // Depth Buffer (t1)
    params[3].InitAsUnorderedAccessView(0); // Light Index Buffer (u0)
    params[4].InitAsUnorderedAccessView(1); // Light Grid Counter (u1)
    params[5].InitAsShaderResourceView(2);  // Instance Buffer (t2)
    params[6].InitAsShaderResourceView(3);  // Light Buffer from LightingResources (t3)
    params[7].InitAsConstants(4, 1);        // Tile config (b1)

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc(_countof(params), params, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || !blob)
    {
        return;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&s_BuildRootSignature))))
    {
        return;
    }

    // Clear Counters PSO
    {
        ComPtr<ID3DBlob> cs;
        if (!LoadComputeShader("shader/hlsl/ClusteredLightGridClearCS.hlsl", cs)) return;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = s_BuildRootSignature.Get();
        desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        PersistentPsoCache::Compute(device, desc, s_ClearCountersPSO);
    }

    // Build Light Grid PSO
    {
        ComPtr<ID3DBlob> cs;
        if (!LoadComputeShader("shader/hlsl/ClusteredLightGridBuildCS.hlsl", cs)) return;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = s_BuildRootSignature.Get();
        desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        PersistentPsoCache::Compute(device, desc, s_BuildCSO);
    }
}

ClusteredLighting::GridConstants ClusteredLighting::GetGridConstants(UINT width, UINT height)
{
    GridConstants c{};
    c.TilesX = s_Resources.TilesX;
    c.TilesY = s_Resources.TilesY;
    c.Slices = s_Resources.Slices;
    c.MaxLightsPerTile = s_Resources.MaxLightsPerTile;
    c.SliceFar = s_Settings.SliceFar;
    
    XMMATRIX view, proj;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, proj);
    XMStoreFloat4x4(&c.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&c.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&c.InvView, XMMatrixTranspose(XMMatrixInverse(nullptr, view)));
    XMStoreFloat4x4(&c.InvProj, XMMatrixTranspose(XMMatrixInverse(nullptr, proj)));
    
    c.NearClip = RendererState::g_kNEAR_CLIP;
    c.FarClip = RendererState::g_kFAR_CLIP;
    
    return c;
}

RenderGraph::ResourceHandle ClusteredLighting::AddLightGridBuildPass(
    RenderGraph& graph,
    RenderGraph::ResourceHandle gpuScene,
    RenderGraph::ResourceHandle depthBuffer,
    UINT width, UINT height)
{
    if (!s_Settings.Enabled) return {};

    EnsureResources(width, height);
    CreatePipelineStates(GraphicsDevice::GetDevice());

    auto device = GraphicsDevice::GetDevice();
    if (!device || !s_Resources.Valid || !s_ClearCountersPSO || !s_BuildCSO) return {};

    // ライトグリッド定数バッファの更新
    GridConstants constants = GetGridConstants(width, height);
    {
        void* mapped = nullptr;
        if (FAILED(s_Resources.LightGridConstants->Map(0, nullptr, &mapped))) return {};
        memcpy(mapped, &constants, sizeof(constants));
        s_Resources.LightGridConstants->Unmap(0, nullptr);
    }

    // リソースハンドルのインポート
    auto importBuffer = [&](ID3D12Resource* res, const char* name, D3D12_RESOURCE_STATES initial, D3D12_RESOURCE_STATES final) {
        RenderGraph::ImportedResource desc{};
        desc.Name = name;
        desc.Resource = res;
        desc.InitialState = initial;
        desc.FinalState = final;
        desc.HasFinalState = true;
        return graph.ImportResource(desc);
    };

    auto lightIndexHandle = importBuffer(s_Resources.LightIndexBuffer.Get(), "Light Index Buffer",
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto lightCounterHandle = importBuffer(s_Resources.LightGridCounter.Get(), "Light Grid Counter",
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto lightConstantsHandle = importBuffer(s_Resources.LightGridConstants.Get(), "Light Grid Constants",
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_GENERIC_READ);

    // Light Data BufferはMegaLightsから取得
    ID3D12Resource* lightDataRes = MegaLights::GetLightUploadBuffer();
    if (!lightDataRes) return {};
    auto lightDataHandle = importBuffer(lightDataRes, "Light Data Buffer",
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    graph.AddPass(
        "Clustered Light Grid - Clear Counters",
        [lightCounterHandle](RenderGraph::PassBuilder& builder) {
            builder.Write(lightCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        [](ID3D12GraphicsCommandList* cmdList) {
            RenderProfiler::ScopedEvent profile("Clear Light Grid Counters", cmdList);
            cmdList->SetComputeRootSignature(s_BuildRootSignature.Get());
            cmdList->SetPipelineState(s_ClearCountersPSO.Get());
            cmdList->SetComputeRootUnorderedAccessView(4, s_Resources.LightGridCounterAddr);
            cmdList->Dispatch((s_Resources.TilesX * s_Resources.TilesY * s_Resources.Slices + 255) / 256, 1, 1);
        });

    graph.AddPass(
        "Clustered Light Grid - Build",
        [gpuScene, depthBuffer, lightIndexHandle, lightCounterHandle, lightConstantsHandle, lightDataHandle]
        (RenderGraph::PassBuilder& builder) {
            builder.Read(gpuScene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                   .Read(depthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                   .Write(lightIndexHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                   .ReadWrite(lightCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                   .Read(lightConstantsHandle, D3D12_RESOURCE_STATE_GENERIC_READ)
                   .Read(lightDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        },
        [&graph, gpuScene, depthBuffer](ID3D12GraphicsCommandList* cmdList) {
            RenderProfiler::ScopedEvent profile("Build Light Grid", cmdList);
             BuildLightGridCS(cmdList, graph, gpuScene, depthBuffer);
        });

    // 後続パスで使うための論理リソースを返す（LightIndexBuffer）
    return lightIndexHandle;
}

void ClusteredLighting::BuildLightGridCS(ID3D12GraphicsCommandList* cmdList, RenderGraph& graph,
                                         RenderGraph::ResourceHandle gpuScene,
                                         RenderGraph::ResourceHandle depthBuffer)
{
    cmdList->SetComputeRootSignature(s_BuildRootSignature.Get());
    cmdList->SetPipelineState(s_BuildCSO.Get());

    cmdList->SetComputeRootConstantBufferView(0, s_Resources.LightGridConstantsAddr);
    cmdList->SetComputeRootShaderResourceView(1, MegaLights::GetLightUploadBuffer()->GetGPUVirtualAddress() + GraphicsDevice::GetFrameIndex() * MegaLights::GetLightStride());
    
    // Depth buffer SRV
    auto depthRes = graph.GetResource(depthBuffer);
    if (!depthRes) return;
    cmdList->SetComputeRootShaderResourceView(2, depthRes->GetGPUVirtualAddress());

    cmdList->SetComputeRootUnorderedAccessView(3, s_Resources.LightIndexBufferAddr);
    cmdList->SetComputeRootUnorderedAccessView(4, s_Resources.LightGridCounterAddr);
    
    // Instance buffer from GPU Scene
    cmdList->SetComputeRootShaderResourceView(5, GpuScene::InstanceBuffer()->GetGPUVirtualAddress());
    
    // Light buffer (MegaLights style)
    cmdList->SetComputeRootShaderResourceView(6, MegaLights::GetLightUploadBuffer()->GetGPUVirtualAddress() + GraphicsDevice::GetFrameIndex() * MegaLights::GetLightStride());

    // Tile config constants
    UINT tileConfig[4] = { s_Resources.TilesX, s_Resources.TilesY, s_Resources.Slices, s_Resources.MaxLightsPerTile };
    cmdList->SetComputeRoot32BitConstants(7, 4, tileConfig, 0);

    const UINT totalTiles = s_Resources.TilesX * s_Resources.TilesY * s_Resources.Slices;
    const UINT groupSize = 64;
    cmdList->Dispatch((totalTiles + groupSize - 1) / groupSize, 1, 1);
}

void ClusteredLighting::BindForDeferred(ID3D12GraphicsCommandList* cmdList, UINT gridParam, UINT lightsParam,
                                        UINT counterParam, UINT constantsParam)
{
    if (!s_Resources.Valid) return;
    cmdList->SetGraphicsRootShaderResourceView(gridParam, s_Resources.LightIndexBufferAddr);
    cmdList->SetGraphicsRootShaderResourceView(lightsParam, MegaLights::GetLightUploadBuffer()->GetGPUVirtualAddress() + GraphicsDevice::GetFrameIndex() * MegaLights::GetLightStride());
    cmdList->SetGraphicsRootShaderResourceView(counterParam, s_Resources.LightGridCounterAddr);
    cmdList->SetGraphicsRootConstantBufferView(constantsParam, s_Resources.LightGridConstantsAddr);
}

void ClusteredLighting::BindForForward(ID3D12GraphicsCommandList* cmdList, UINT gridParam, UINT lightsParam,
                                       UINT counterParam, UINT constantsParam)
{
    // Forward+用も同じバッファを参照
    BindForDeferred(cmdList, gridParam, lightsParam, counterParam, constantsParam);
}

void ClusteredLighting::BindForCompute(ID3D12GraphicsCommandList* cmdList, UINT gridParam, UINT lightsParam,
                                       UINT counterParam, UINT constantsParam)
{
    if (!s_Resources.Valid) return;
    cmdList->SetComputeRootShaderResourceView(gridParam, s_Resources.LightIndexBufferAddr);
    cmdList->SetComputeRootShaderResourceView(lightsParam, MegaLights::GetLightUploadBuffer()->GetGPUVirtualAddress() + GraphicsDevice::GetFrameIndex() * MegaLights::GetLightStride());
    cmdList->SetComputeRootShaderResourceView(counterParam, s_Resources.LightGridCounterAddr);
    cmdList->SetComputeRootConstantBufferView(constantsParam, s_Resources.LightGridConstantsAddr);
}

void ClusteredLighting::Shutdown()
{
    s_Resources = Resources{};
    s_BuildRootSignature.Reset();
    s_BuildCSO.Reset();
    s_ClearCountersPSO.Reset();
}