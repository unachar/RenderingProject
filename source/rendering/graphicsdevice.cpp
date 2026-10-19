#include "pch.h"
#include "proceduralstudiowindow.h"
#include "temporalupscaler.h"
#include "advancedeffects.h"
#include "asynccompute.h"
#include "atmospherelut.h"
#include "pbrluts.h"
#include "transparencyeffects.h"
#include "assetthumbnailmanager.h"
#include "transmittanceshadows.h"
#include "materialmanager.h"
#include "materialpreview.h"
#include "cloud/cloudrenderer.h"
#include "graphicslog.h"
#include "renderprofiler.h"
#include <cstdlib>
#include "shaderpipelines.h"
#include "shaderpaths.h"
#include "world.h"
#include "ecs.h"
#include "texturemanager.h"
#include "componentmanager.h"
#include "imguimanager.h"
#include "psomanager.h"
#include "spatialupscaler.h"
#include "visibilitybuffer.h"
#include "screenspaceeffects.h"
#include "occlusionculling.h"
#include "meshshaderpipeline.h"
#include "gpuscene/gpuscene.h"
#include "lightinstancebuilder.h"
#include "megalights.h"
#include "graphicsdevice.h"
#include "meshshader/meshshaderrenderer.h"
#include "rendertargets.h"
#include "scenepasses.h"
#include "raytracing.h"
#include "vertexdeformation.h"
#include "dlssupscaler.h"

static bool g_DeviceRemovalLogged = false;
static bool g_GpuBasedValidationEnabled = false;
static RenderFeature::Capabilities g_FeatureCapabilities;

const RenderFeature::Capabilities& GraphicsDevice::GetFeatureCapabilities()
{
    return g_FeatureCapabilities;
}

bool GraphicsDevice::GpuBasedValidationEnabled()
{
    return g_GpuBasedValidationEnabled;
}

bool GraphicsDevice::Init(HWND hwnd)
{
    g_DeviceRemovalLogged = false;
    g_FeatureCapabilities = {};
    g_GpuBasedValidationEnabled = false;
    m_FrameIndex = 0;
    m_FenceEvent = nullptr;
    m_FrameLatencyWaitableObject = nullptr;
    for (UINT i = 0; i < g_kFRAME_COUNT; ++i)
    {
        m_FenceValues[i] = 0;
    }
    m_Hwnd = hwnd;

    RECT rc;
    GetClientRect(hwnd, &rc);
    m_Width = rc.right - rc.left;
    m_Height = rc.bottom - rc.top;

    if (m_Width == 0)
    {
        m_Width = g_kSCREEN_WIDTH;
    }
    if (m_Height == 0)
    {
        m_Height = g_kSCREEN_HEIGHT;
    }

    m_SceneWidth = max((UINT)roundf(m_Width * m_ResolutionScale), 1u);
    m_SceneHeight = max((UINT)roundf(m_Height * m_ResolutionScale), 1u);

    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
    {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings1;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings1))))
    {
        dredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }

#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        char gpuValidation[8]{};
        if (GetEnvironmentVariableA("DX12_GPU_BASED_VALIDATION", gpuValidation, sizeof(gpuValidation)) &&
            atoi(gpuValidation))
        {
            ComPtr<ID3D12Debug1> validation;
            if (SUCCEEDED(debugController.As(&validation)))
            {
                validation->SetEnableGPUBasedValidation(TRUE);
                g_GpuBasedValidationEnabled = true;
                WriteGraphicsLog("GPU Based Validation enabled\n");
            }
        }
    }
#endif

    WriteGraphicsLog("Step: CreateDXGIFactory2\n");
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_Factory));
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateDXGIFactory2 failed\n");
        return false;
    }

    m_AllowTearing = false;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(m_Factory.As(&factory5)))
    {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(
                factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
        {
            m_AllowTearing = allowTearing == TRUE;
        }
    }

    WriteGraphicsLog("Step: D3D12CreateDevice\n");
    ComPtr<IDXGIFactory6> adapterFactory;
    ComPtr<IDXGIAdapter1> selectedAdapter;
    if (SUCCEEDED(m_Factory.As(&adapterFactory)))
    {
        for (UINT adapterIndex = 0;; ++adapterIndex)
        {
            selectedAdapter.Reset();
            const HRESULT adapterResult = adapterFactory->EnumAdapterByGpuPreference(
                adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&selectedAdapter));
            if (FAILED(adapterResult))
            {
                break;
            }
            DXGI_ADAPTER_DESC1 adapterDescription{};
            if (FAILED(selectedAdapter->GetDesc1(&adapterDescription)) ||
                (adapterDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(selectedAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device))))
            {
                char adapterName[256]{};
                WideCharToMultiByte(CP_UTF8, 0, adapterDescription.Description, -1, adapterName, sizeof(adapterName),
                                    nullptr, nullptr);
                WriteGraphicsLog("Adapter: %s\n", adapterName);
                break;
            }
        }
    }
    hr = S_OK;
    if (!m_Device)
    {
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device));
    }
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: D3D12CreateDevice failed\n");
        return false;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
    {
        // 2D テクスチャのミップタイルを選択的にマップするには Tier 2 が必要。
        g_FeatureCapabilities.ReservedResources = options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
    {
        g_FeatureCapabilities.Raytracing = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
    {
        g_FeatureCapabilities.MeshShader = options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        g_FeatureCapabilities.SamplerFeedback =
            options7.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_2};
    if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4))) &&
        SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))))
    {
        g_FeatureCapabilities.Native16BitShaderOps =
            options4.Native16BitShaderOpsSupported && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_2;
    }
    WriteGraphicsLog("Feature capabilities: native16bit=%u\n",
                     static_cast<unsigned>(g_FeatureCapabilities.Native16BitShaderOps));
    WriteGraphicsLog("Feature capabilities: mesh=%u dxr11=%u sampler_feedback=%u reserved_textures=%u\n",
                     static_cast<unsigned>(g_FeatureCapabilities.MeshShader),
                     static_cast<unsigned>(g_FeatureCapabilities.Raytracing),
                     static_cast<unsigned>(g_FeatureCapabilities.SamplerFeedback),
                     static_cast<unsigned>(g_FeatureCapabilities.ReservedResources));

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = m_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CommandQueue));
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateCommandQueue failed\n");
        return false;
    }
    m_CommandQueue->SetName(L"Main Direct Queue");

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = g_kFRAME_COUNT;
    swapChainDesc.Width = m_Width;
    swapChainDesc.Height = m_Height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    UINT tearingFlag;
    if (m_AllowTearing)
    {
        tearingFlag = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    else
    {
        tearingFlag = 0;
    }
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearingFlag);

    ComPtr<IDXGISwapChain1> sc1;
    hr = m_Factory->CreateSwapChainForHwnd(m_CommandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &sc1);
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateSwapChainForHwnd failed\n");
        return false;
    }
    sc1.As(&m_SwapChain);
    if (m_SwapChain)
    {

        m_SwapChain->SetMaximumFrameLatency(2);
        m_FrameLatencyWaitableObject = m_SwapChain->GetFrameLatencyWaitableObject();
    }
    m_FrameIndex = m_SwapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = g_kFRAME_COUNT;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = m_Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_RtvHeap));
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateDescriptorHeap(RTV) failed\n");
        return false;
    }

    UINT rtvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT n = 0; n < g_kFRAME_COUNT; n++)
    {
        m_SwapChain->GetBuffer(n, IID_PPV_ARGS(&m_RenderTargets[n]));
        m_Device->CreateRenderTargetView(m_RenderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, rtvSize);
    }

    for (UINT n = 0; n < g_kFRAME_COUNT; n++)
    {
        hr = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CommandAllocator[n]));
        if (FAILED(hr))
        {
            WriteGraphicsLog("ERROR: CreateCommandAllocator failed\n");
            return false;
        }
    }

    WriteGraphicsLog("Step: CreateCommandList\n");
    hr = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocator[0].Get(), nullptr,
                                     IID_PPV_ARGS(&m_CommandList));
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateCommandList failed\n");
        return false;
    }
    m_CommandList->Close();
    m_CommandList->SetName(L"Main Frame Command List");

    WriteGraphicsLog("Step: CreateFence\n");
    hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateFence failed\n");
        return false;
    }
    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    m_CurrentFenceValue = 0;
    for (UINT i = 0; i < g_kFRAME_COUNT; i++)
    {
        m_FenceValues[i] = 0;
    }

    CD3DX12_DESCRIPTOR_RANGE ranges[1];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE rangesTex[1];
    rangesTex[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
    CD3DX12_DESCRIPTOR_RANGE rangesShadowTex[1];
    rangesShadowTex[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
    CD3DX12_DESCRIPTOR_RANGE rangesNormalTex[1];
    rangesNormalTex[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
    CD3DX12_DESCRIPTOR_RANGE rangesEnvironmentTex[1];
    rangesEnvironmentTex[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
    CD3DX12_DESCRIPTOR_RANGE rangesSceneColorTex[1];
    rangesSceneColorTex[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
    CD3DX12_DESCRIPTOR_RANGE meshHiZRanges[2];
    meshHiZRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 22);
    meshHiZRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 23);

    CD3DX12_DESCRIPTOR_RANGE assetTextures;
    assetTextures.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 512, 0, 1);
    CD3DX12_DESCRIPTOR_RANGE forwardResources[13];
    const UINT forwardRegisters[] = {26, 27, 12, 16, 17, 31, 32, 13, 14, 28, 35, 36, 29};
    for (UINT index = 0; index < _countof(forwardRegisters); ++index)
    {
        forwardResources[index].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, forwardRegisters[index]);
    }
    // 34, 35：MegaLights M7（Forward / 半透明のグリッド：t44〜t46 + b10、ライトのテクスチャ：t0 space5）
    CD3DX12_DESCRIPTOR_RANGE megaLightRanges[2];
    megaLightRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 44);
    megaLightRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 10);
    CD3DX12_DESCRIPTOR_RANGE megaLightTextures;
    megaLightTextures.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 512, 0, 5);
    CD3DX12_DESCRIPTOR_RANGE glassRanges[4];
    glassRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 12);
    glassRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, g_kGLASS_MAX_MESHES, 1, 12);
    glassRanges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, g_kGLASS_MAX_MESHES, 0, 13);
    glassRanges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, g_kGLASS_MAX_MESHES, 0, 14);
    CD3DX12_ROOT_PARAMETER rootParametersAll[37];
    rootParametersAll[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);
    rootParametersAll[1].InitAsDescriptorTable(1, &rangesTex[0], D3D12_SHADER_VISIBILITY_ALL);
    rootParametersAll[2].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[3].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[4].InitAsDescriptorTable(1, &rangesShadowTex[0], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[5].InitAsConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParametersAll[6].InitAsDescriptorTable(1, &rangesNormalTex[0], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[7].InitAsDescriptorTable(1, &rangesEnvironmentTex[0], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[8].InitAsDescriptorTable(1, &rangesSceneColorTex[0], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[9].InitAsShaderResourceView(0, 2, D3D12_SHADER_VISIBILITY_VERTEX);
    rootParametersAll[10].InitAsShaderResourceView(11, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[11].InitAsShaderResourceView(20, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParametersAll[12].InitAsShaderResourceView(21, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParametersAll[13].InitAsConstants(16, 4, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParametersAll[14].InitAsDescriptorTable(1, &meshHiZRanges[0], D3D12_SHADER_VISIBILITY_ALL);
    rootParametersAll[15].InitAsDescriptorTable(1, &meshHiZRanges[1], D3D12_SHADER_VISIBILITY_ALL);
    rootParametersAll[16].InitAsShaderResourceView(25, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[17].InitAsConstantBufferView(5, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[18].InitAsDescriptorTable(1, &assetTextures, D3D12_SHADER_VISIBILITY_PIXEL);
    for (UINT index = 0; index < 7; ++index)
    {
        rootParametersAll[19 + index].InitAsDescriptorTable(1, &forwardResources[index], D3D12_SHADER_VISIBILITY_PIXEL);
    }
    rootParametersAll[26].InitAsConstantBufferView(6, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[27].InitAsConstantBufferView(9, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    for (UINT index = 7; index < _countof(forwardResources); ++index)
    {
        rootParametersAll[21 + index].InitAsDescriptorTable(1, &forwardResources[index], D3D12_SHADER_VISIBILITY_PIXEL);
    }
    rootParametersAll[34].InitAsDescriptorTable(2, megaLightRanges, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[35].InitAsDescriptorTable(1, &megaLightTextures, D3D12_SHADER_VISIBILITY_PIXEL);
    rootParametersAll[36].InitAsDescriptorTable(4, glassRanges, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler{};

    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 16;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[5]{};
    staticSamplers[4] = CD3DX12_STATIC_SAMPLER_DESC(4, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    staticSamplers[3] = CD3DX12_STATIC_SAMPLER_DESC(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    staticSamplers[0] = sampler;
    staticSamplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].MipLODBias = 0.0f;
    staticSamplers[1].MaxAnisotropy = 1;
    staticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    staticSamplers[1].MinLOD = 0.0f;
    staticSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[1].ShaderRegister = 1;
    staticSamplers[1].RegisterSpace = 0;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    staticSamplers[2] = CD3DX12_STATIC_SAMPLER_DESC(
        2, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f, 1, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(_countof(rootParametersAll), rootParametersAll, _countof(staticSamplers), staticSamplers,
                           D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    WriteGraphicsLog("Step: D3D12SerializeRootSignature\n");
    hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: D3D12SerializeRootSignature failed\n");
        if (error)
        {
            Debug::Log("%s\\n", (char*)error->GetBufferPointer());
            FILE* fp = nullptr;
            fopen_s(&fp, "error_log.txt", "w");
            if (fp)
            {
                fprintf(fp, "%s\n", (char*)error->GetBufferPointer());
                fclose(fp);
            }
        }
        return false;
    }
    WriteGraphicsLog("Step: CreateRootSignature\n");
    hr = m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                       IID_PPV_ARGS(&m_RootSignature));
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateRootSignature failed\n");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc{};
    // 末尾に MegaLights（M7：Forward 用のグリッドのディスクリプタ）の領域を追加
    cbvHeapDesc.NumDescriptors = g_kENGINE_DESCRIPTOR_END + 1 + g_kTHUMBNAIL_DESCRIPTOR_COUNT +
                                 ImGuiManager::kSrvDescriptorCount + 16384 + MegaLights::kEngineHeapDescriptors;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    WriteGraphicsLog("Step: CreateDescriptorHeap(CBV)\n");
    hr = m_Device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&m_CbvHeap));
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateDescriptorHeap(CBV) failed\n");
        return false;
    }

    m_PipelineState = nullptr;

    m_Viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)m_SceneWidth, (float)m_SceneHeight);
    m_ScissorRect = CD3DX12_RECT(0, 0, m_SceneWidth, m_SceneHeight);
    m_FullViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)m_Width, (float)m_Height);
    m_FullScissorRect = CD3DX12_RECT(0, 0, m_Width, m_Height);

    const UINT totalCbSize = g_kCB_ALIGNED_SIZE * g_kCBV_COUNT;
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(totalCbSize);
    auto cbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    hr = m_Device->CreateCommittedResource(&cbHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ConstantBuffer));
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateCommittedResource(CB) failed\n");
        return false;
    }

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
    cbvDesc.SizeInBytes = g_kCB_ALIGNED_SIZE;
    CD3DX12_CPU_DESCRIPTOR_HANDLE cbvHandle(m_CbvHeap->GetCPUDescriptorHandleForHeapStart());
    m_CbvIncrementSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT cbvIncrement = m_CbvIncrementSize;
    for (uint32_t i = 0; i < g_kCBV_COUNT; ++i)
    {
        cbvDesc.BufferLocation = m_ConstantBuffer->GetGPUVirtualAddress() + (i * g_kCB_ALIGNED_SIZE);
        m_Device->CreateConstantBufferView(&cbvDesc, cbvHandle);
        cbvHandle.Offset(1, cbvIncrement);
    }

    CD3DX12_RANGE readRange(0, 0);
    m_ConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pCbvDataBegin));

    WriteGraphicsLog("Step: CreateDepthBuffer\n");
    if (!RenderTargets::CreateDepthBuffer())
    {
        return false;
    }

    WriteGraphicsLog("Step: CreateModelPipeline\n");
    if (!ShaderPipelines::CreateModelPipeline())
    {
        return false;
    }

    const UINT dynamicVertexBufferSize = g_kMAX_DYNAMIC_VERTICES * sizeof(Vertex);
    auto dvbDesc = CD3DX12_RESOURCE_DESC::Buffer(dynamicVertexBufferSize);
    auto dvbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    hr = m_Device->CreateCommittedResource(&dvbHeapProps, D3D12_HEAP_FLAG_NONE, &dvbDesc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           IID_PPV_ARGS(&m_DynamicVertexBuffer));
    if (FAILED(hr))
    {
        WriteGraphicsLog("ERROR: CreateCommittedResource(Dynamic VB) failed\n");
        return false;
    }

    m_DynamicVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pDynamicVertexDataBegin));
    m_DynamicVertexBufferView.BufferLocation = m_DynamicVertexBuffer->GetGPUVirtualAddress();
    m_DynamicVertexBufferView.SizeInBytes = dynamicVertexBufferSize;
    m_DynamicVertexBufferView.StrideInBytes = sizeof(Vertex);
    m_DynamicVertexOffset = 0;

    WriteGraphicsLog("Step: TextureManager::Init\n");
    TextureManager::Init();
    MaterialManager::Init();

    WriteGraphicsLog("Step: CreatePostProcessPipeline\n");
    if (!ShaderPipelines::CreatePostProcessPipeline())
    {
        return false;
    }

    WriteGraphicsLog("Step: SpatialUpscaler::Initialize\n");
    SpatialUpscaler::Initialize(m_Device.Get(), m_CbvHeap.Get(), m_CbvIncrementSize);
    WriteGraphicsLog("Step: VisibilityBuffer::Initialize\n");
    VisibilityBuffer::Initialize(m_Device.Get(), m_CbvHeap.Get(), m_CbvIncrementSize);
    WriteGraphicsLog("Step: ScreenSpaceEffects::Initialize\n");
    ScreenSpaceEffects::Initialize(m_Device.Get(), m_CbvHeap.Get(), m_CbvIncrementSize);
    WriteGraphicsLog("Step: CloudRenderer::Initialize\n");
    CloudRenderer::Initialize(m_Device.Get(), m_CbvHeap.Get(), m_CbvIncrementSize);
    WriteGraphicsLog("Step: Raytracing::Initialize\n");
    Raytracing::Initialize(m_Device.Get(), m_CbvHeap.Get(), m_CbvIncrementSize);
    // ShaderPipelines は Raytracing の可否から SM6 とインライン可視性判定を選ぶ。
    // その機能判定の後、最初のフレームより前に遅延ライティングを構築する。
    WriteGraphicsLog("Step: CreateDeferredLightingPipeline\n");
    if (!PsoManager::CreateDeferredLightingPipeline())
    {
        return false;
    }
    WriteGraphicsLog("Step: AsyncCompute::Initialize\n");
    AsyncCompute::Initialize(m_Device.Get());
    PbrLuts::WriteNullDescriptors(m_Device.Get(), m_CbvHeap.Get());
    WriteGraphicsLog("Step: PbrLuts::Initialize\n");
    PbrLuts::Initialize(m_Device.Get(), m_CommandQueue.Get(), m_CbvHeap.Get());
    WriteGraphicsLog("Step: TransparencyEffects::Initialize\n");
    TransparencyEffects::Initialize(m_Device.Get(), m_CbvHeap.Get());
    WriteGraphicsLog("Step: AtmosphereLut::Initialize\n");
    AtmosphereLut::Initialize(m_Device.Get(), m_CbvHeap.Get());
    WriteGraphicsLog("Step: AdvancedEffects::Initialize\n");
    if (!AdvancedEffects::Initialize(m_Device.Get(), m_CbvHeap.Get()))
    {
        WriteGraphicsLog("WARNING: AdvancedEffects unavailable (froxel fog / auto exposure / MB / DOF disabled)\n");
    }
    WriteGraphicsLog("Step: DlssUpscaler::Initialize\n");
    DlssUpscaler::Initialize(m_Device.Get(), m_CbvHeap.Get(), m_CbvIncrementSize);
    WriteGraphicsLog("Step: OcclusionCulling::Initialize\n");
    OcclusionCulling::Initialize(m_Device.Get(), m_CbvHeap.Get(), m_CbvIncrementSize);
    WriteGraphicsLog("Step: MeshShaderPipeline::Initialize\n");
    MeshShaderPipeline::Initialize(m_Device.Get(), ShaderPipelines::GetModelRootSignature());
    MeshShaderRenderer::QuerySupport(m_Device.Get());
    WriteGraphicsLog("Step: LightInstanceBuilder::Initialize\n");
    LightInstanceBuilder::Initialize(m_Device.Get());
    WriteGraphicsLog("Step: MegaLights::Initialize\n");
    if (!MegaLights::Initialize(m_Device.Get()))
    {
        WriteGraphicsLog("WARNING: MegaLights unavailable (clustered light grid disabled)\n");
    }
    // ヒープの末尾 16384 個は VirtualGeometry のページ（PageDescriptors が NumDescriptors - MaximumSlots から使う）。
    // MegaLights の領域はその手前に置く（以前は末尾に置いていたため、最後の 12 ページの記述子と重なっていた）
    MegaLights::SetEngineHeap(m_CbvHeap.Get(), g_kENGINE_DESCRIPTOR_END + 1 + g_kTHUMBNAIL_DESCRIPTOR_COUNT +
                                                   ImGuiManager::kSrvDescriptorCount);

    WriteGraphicsLog("Step: CreateSceneRenderTarget\n");
    if (!RenderTargets::CreateSceneRenderTarget())
    {
        return false;
    }

    WriteGraphicsLog("Step: CreateSkinningPipeline\n");
    if (!ShaderPipelines::CreateSkinningPipeline())
    {
        return false;
    }

    WriteGraphicsLog("Step: CreateShadowDepthBuffer\n");
    if (!RenderTargets::CreateShadowDepthBuffer())
    {
        return false;
    }

    WriteGraphicsLog("Step: ImGuiManager::Init\n");
    if (!ImGuiManager::Init(m_Hwnd, m_Device.Get(), m_CommandQueue.Get(), g_kFRAME_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM,
                            m_CbvHeap.Get(), RenderTargets::GetImGuiCpuHandle(), RenderTargets::GetImGuiGpuHandle()))
    {
        WriteGraphicsLog("ERROR: ImGuiManager::Init failed\n");
        return false;
    }

    return true;
}

void GraphicsDevice::Uninit()
{
    WaitForGpuIdle();
    RayTelemetry::Data().Shutdown();
    ScenePasses::ReleaseStaticShadowCache();

    ProceduralStudioWindow::Shutdown(); // エディターの記述子プールを破棄する前に終了する
    AssetThumbnailManager::Shutdown();
    ImGuiManager::Uninit();

    MaterialPreview::Shutdown();
    TransmittanceShadows::Shutdown();
    MaterialManager::Shutdown();
    TextureManager::Uninit();

    if (m_ConstantBuffer)
    {
        m_ConstantBuffer->Unmap(0, nullptr);
        m_pCbvDataBegin = nullptr;
    }
    if (m_PostProcessConstantBuffer)
    {
        m_PostProcessConstantBuffer->Unmap(0, nullptr);
        m_pPostProcessCbvDataBegin = nullptr;
    }

    m_PsoCache.clear();
    m_PostProcessPsoMap.clear();
    m_BloomExtractPso.Reset();
    m_AtmospherePso.Reset();

    if (m_DynamicVertexBuffer)
    {
        m_DynamicVertexBuffer->Unmap(0, nullptr);
        m_DynamicVertexBuffer.Reset();
    }

    m_ModelPipelineState.Reset();
    m_ModelRootSignature.Reset();
    m_DepthStencilBuffer.Reset();
    m_DsvHeap.Reset();
    m_PipelineState.Reset();
    m_ConstantBuffer.Reset();
    m_PostProcessConstantBuffer.Reset();
    m_PostProcessRootSignature.Reset();
    m_UpscaleRootSignature.Reset();
    SpatialUpscaler::Shutdown();
    VisibilityBuffer::Shutdown();
    ScreenSpaceEffects::Shutdown();
    CloudRenderer::Shutdown();
    AdvancedEffects::Shutdown();
    TemporalUpscaler::Shutdown();
    AtmosphereLut::Shutdown();
    TransparencyEffects::Shutdown();
    PbrLuts::Shutdown();
    AsyncCompute::Shutdown();
    Raytracing::Shutdown();
    VertexDeformation::Shutdown();
    DlssUpscaler::Shutdown();
    OcclusionCulling::Shutdown();
    GpuScene::Shutdown();
    MeshShaderPipeline::Shutdown();
    LightInstanceBuilder::Shutdown();
    MegaLights::Shutdown();
    RenderTargets::ReleaseGBufferResources();
    m_SceneRenderTarget.Reset();
    m_PostProcessRenderTarget.Reset();
    m_PreUpscaleAaRenderTarget.Reset();
    m_PreUpscaleAaHistory.Reset();
    m_EditorSceneRenderTarget.Reset();
    m_TransparentSceneCopy.Reset();
    m_SceneRtvHeap.Reset();
    m_CbvHeap.Reset();
    m_RootSignature.Reset();
    m_CommandList.Reset();
    for (UINT n = 0; n < g_kFRAME_COUNT; n++)
    {
        m_CommandAllocator[n].Reset();
    }
    m_Fence.Reset();
    for (UINT n = 0; n < g_kFRAME_COUNT; n++)
    {
        m_RenderTargets[n].Reset();
    }
    m_RtvHeap.Reset();
    if (m_FrameLatencyWaitableObject)
    {
        CloseHandle(m_FrameLatencyWaitableObject);
        m_FrameLatencyWaitableObject = nullptr;
    }
    m_SwapChain.Reset();
    m_CommandQueue.Reset();
    m_Factory.Reset();
    m_Device.Reset();
    g_FeatureCapabilities = {};

    if (m_FenceEvent)
    {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }
}

bool GraphicsDevice::CheckDeviceHealth(HRESULT operationResult, const char* operation)
{
    if (!m_Device)
    {
        return false;
    }
    const HRESULT removedReason = m_Device->GetDeviceRemovedReason();
    if (SUCCEEDED(operationResult) && SUCCEEDED(removedReason))
    {
        return true;
    }
    if (g_DeviceRemovalLogged)
    {
        return false;
    }
    g_DeviceRemovalLogged = true;

    FILE* log = nullptr;
    fopen_s(&log, "device_removed.log", "a");
    if (!log)
    {
        return false;
    }
    const char* operationName;
    if (operation)
    {
        operationName = operation;
    }
    else
    {
        operationName = "unknown";
    }
    fprintf(log, "\nDEVICE REMOVED: operation=%s operationHr=0x%08X reason=0x%08X\n", operationName, operationResult,
            removedReason);

#ifdef _DEBUG
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(m_Device.As(&infoQueue)))
    {
        const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        UINT64 firstMessage;
        if (messageCount > 256u)
        {
            firstMessage = messageCount - 256u;
        }
        else
        {
            firstMessage = 0u;
        }
        for (UINT64 messageIndex = firstMessage; messageIndex < messageCount; ++messageIndex)
        {
            SIZE_T messageSize = 0;
            if (FAILED(infoQueue->GetMessage(messageIndex, nullptr, &messageSize)) || messageSize == 0)
            {
                continue;
            }
            vector<BYTE> messageStorage(messageSize);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageStorage.data());
            if (SUCCEEDED(infoQueue->GetMessage(messageIndex, message, &messageSize)))
            {
                const char* messageDescription;
                if (message->pDescription)
                {
                    messageDescription = message->pDescription;
                }
                else
                {
                    messageDescription = "<empty>";
                }
                fprintf(log, "DebugLayer[%llu] severity=%u id=%u: %s\n", static_cast<unsigned long long>(messageIndex),
                        static_cast<UINT>(message->Severity), static_cast<UINT>(message->ID), messageDescription);
            }
        }
    }
#endif

    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (SUCCEEDED(m_Device.As(&dred)))
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            UINT nodeCount = 0;
            for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node && nodeCount < 64;
                 node = node->pNext, ++nodeCount)
            {
                UINT completed;
                if (node->pLastBreadcrumbValue)
                {
                    completed = *node->pLastBreadcrumbValue;
                }
                else
                {
                    completed = 0;
                }
                UINT operationIndex;
                if (node->BreadcrumbCount > 0)
                {
                    operationIndex = min(completed, node->BreadcrumbCount - 1u);
                }
                else
                {
                    operationIndex = 0u;
                }
                UINT lastOperation;
                if (node->pCommandHistory && node->BreadcrumbCount > 0)
                {
                    lastOperation = static_cast<UINT>(node->pCommandHistory[operationIndex]);
                }
                else
                {
                    lastOperation = UINT_MAX;
                }
                const char* queueName;
                if (node->pCommandQueueDebugNameA)
                {
                    queueName = node->pCommandQueueDebugNameA;
                }
                else
                {
                    queueName = "<unnamed>";
                }
                const char* commandListName;
                if (node->pCommandListDebugNameA)
                {
                    commandListName = node->pCommandListDebugNameA;
                }
                else
                {
                    commandListName = "<unnamed>";
                }
                fprintf(log, "Breadcrumb[%u]: queue=%s list=%s completed=%u/%u lastOp=%u\n", nodeCount, queueName,
                        commandListName, completed, node->BreadcrumbCount, lastOperation);
                if (node->pCommandHistory && node->BreadcrumbCount > 0)
                {
                    UINT begin;
                    if (completed > 12u)
                    {
                        begin = completed - 12u;
                    }
                    else
                    {
                        begin = 0u;
                    }
                    const UINT end = min(completed + 4u, node->BreadcrumbCount);
                    fprintf(log, "  Operations:");
                    for (UINT index = begin; index < end; ++index)
                    {
                        const char* completionMarker;
                        if (index == completed)
                        {
                            completionMarker = "*";
                        }
                        else
                        {
                            completionMarker = "";
                        }
                        fprintf(log, " %u:%u%s", index, static_cast<UINT>(node->pCommandHistory[index]),
                                completionMarker);
                    }
                    fprintf(log, "\n");
                }
                for (UINT contextIndex = 0; contextIndex < node->BreadcrumbContextsCount; ++contextIndex)
                {
                    const auto& context = node->pBreadcrumbContexts[contextIndex];
                    if (context.BreadcrumbIndex + 32u >= completed && context.BreadcrumbIndex <= completed + 4u)
                    {
                        const wchar_t* contextName;
                        if (context.pContextString)
                        {
                            contextName = context.pContextString;
                        }
                        else
                        {
                            contextName = L"<unnamed>";
                        }
                        fprintf(log, "  Context[%u] @%u: %ls\n", contextIndex, context.BreadcrumbIndex, contextName);
                    }
                }
            }
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault)))
        {
            fprintf(log, "PageFaultVA=0x%016llX\n", static_cast<unsigned long long>(pageFault.PageFaultVA));
            UINT allocationCount = 0;
            for (auto* node = pageFault.pHeadExistingAllocationNode; node && allocationCount < 32;
                 node = node->pNext, ++allocationCount)
            {
                const char* objectName;
                if (node->ObjectNameA)
                {
                    objectName = node->ObjectNameA;
                }
                else
                {
                    objectName = "<unnamed>";
                }
                fprintf(log, "ExistingAllocation[%u]: %s type=%u\n", allocationCount, objectName,
                        static_cast<UINT>(node->AllocationType));
            }
            allocationCount = 0;
            for (auto* node = pageFault.pHeadRecentFreedAllocationNode; node && allocationCount < 32;
                 node = node->pNext, ++allocationCount)
            {
                const char* objectNameValue;
                if (node->ObjectNameA)
                {
                    objectNameValue = node->ObjectNameA;
                }
                else
                {
                    objectNameValue = "<unnamed>";
                }
                fprintf(log, "RecentFreedAllocation[%u]: %s type=%u\n", allocationCount, objectNameValue,
                        static_cast<UINT>(node->AllocationType));
            }
        }
    }
    fclose(log);
    return false;
}

bool GraphicsDevice::WaitForGpuIdle()
{
    if (!m_CommandQueue || !m_Fence || !m_FenceEvent)
    {
        return false;
    }
    const UINT64 fenceValue = ++m_CurrentFenceValue;
    const HRESULT signalHr = m_CommandQueue->Signal(m_Fence.Get(), fenceValue);
    if (!CheckDeviceHealth(signalHr, "GPU idle fence signal"))
    {
        return false;
    }
    if (m_Fence->GetCompletedValue() < fenceValue)
    {
        const HRESULT eventHr = m_Fence->SetEventOnCompletion(fenceValue, m_FenceEvent);
        if (FAILED(eventHr))
        {
            return false;
        }
        WaitForSingleObject(m_FenceEvent, INFINITE);
    }
    return CheckDeviceHealth(S_OK, "GPU idle wait");
}

void GraphicsDevice::SubmitForExternalCompute()
{
    // 外部 CUDA ストリームが待てるように、パック処理のディスパッチを送信する。
    // 前のコマンドが実行中の可能性があるため、アロケーターはリセットしない。
    RenderProfiler::PrepareCommandListSplit(m_CommandList.Get());
    if (!CheckDeviceHealth(m_CommandList->Close(), "OptiX command list close"))
    {
        // 送信失敗は致命的なエラー。未送信の無効なコマンドリストからは、安全な記録の再開も
        // リソース状態の復元もできない。
        WriteGraphicsLog("Fatal OptiX command-list close failure.\n");
        std::abort();
    }
    ID3D12CommandList* lists[] = {m_CommandList.Get()};
    m_CommandQueue->ExecuteCommandLists(1, lists);
    if (!CheckDeviceHealth(m_CommandList->Reset(m_CommandAllocator[m_FrameIndex].Get(), nullptr),
                           "OptiX command list reset"))
    {
        WriteGraphicsLog("Fatal OptiX command-list reset failure.\n");
        std::abort();
    }
    // リセットでグラフィックス状態が失われるため、シーン共通のラスタライズ状態を戻す。
    // 後続パスは自身のルートシグネチャ、PSO、ターゲット、テーブルをバインドする。
    m_CommandList->RSSetViewports(1, &m_Viewport);
    m_CommandList->RSSetScissorRects(1, &m_ScissorRect);
    ID3D12DescriptorHeap* heaps[] = {m_CbvHeap.Get()};
    m_CommandList->SetDescriptorHeaps(1, heaps);
}
