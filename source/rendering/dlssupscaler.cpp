#include "pch.h"
#include "dlssupscaler.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "rendererstate.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "imguimanager.h"
#include <filesystem>

#ifdef RENDERINGPROJECT_USE_DLSS
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_d3d.h>
#endif

namespace
{
    constexpr UINT kFrameCount = RendererState::g_kFRAME_COUNT;
    constexpr UINT kDescriptorStart =
        RendererState::g_kHIZ_MIP_SRV_START_INDEX + 2 * RendererState::g_kHIZ_MAX_MIPS + 2 * kFrameCount + 1;
    constexpr UINT kDescriptorsPerFrame = 3;
    static_assert(kDescriptorStart + kDescriptorsPerFrame * kFrameCount <= RendererState::g_kENGINE_DESCRIPTOR_END,
                  "DLSS descriptors exceed the reserved engine heap");

    ID3D12Device* g_Device = nullptr;
    ID3D12DescriptorHeap* g_Heap = nullptr;
    UINT g_Increment = 0;
    bool g_Available = false;
    bool g_Active = false;
    bool g_Prepared = false;
    bool g_ResetHistory = true;
    bool g_LoggedEvaluation = false;
    UINT g_JitterFrame = 0;
    EntityID g_Camera = g_kINVALID_ENTITY;
    XMFLOAT2 g_Jitter{};
    string g_Status = "NGX SDK is not installed";

#ifdef RENDERINGPROJECT_USE_DLSS
    NVSDK_NGX_Parameter* g_Parameters = nullptr;
    NVSDK_NGX_Handle* g_Feature = nullptr;
    bool g_Initialized = false;
    UINT g_InputWidth = 0;
    UINT g_InputHeight = 0;
    UINT g_OutputWidth = 0;
    UINT g_OutputHeight = 0;
    bool g_FeatureHdr = false;
    ComPtr<ID3D12RootSignature> g_Root;
    ComPtr<ID3D12PipelineState> g_PreparePipeline;
    ComPtr<ID3D12PipelineState> g_CopyPipeline;

    struct FrameResources
    {
        ComPtr<ID3D12Resource> Color;
        ComPtr<ID3D12Resource> Motion;
        ComPtr<ID3D12Resource> Output;
    };

    array<FrameResources, kFrameCount> g_Frames;

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Heap->GetCPUDescriptorHandleForHeapStart(), index, g_Increment);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT index)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(g_Heap->GetGPUDescriptorHandleForHeapStart(), index, g_Increment);
    }

    bool CreateHelperPipelines()
    {
        CD3DX12_DESCRIPTOR_RANGE ranges[4];
        CD3DX12_ROOT_PARAMETER parameters[4];
        for (UINT index = 0; index < 4; ++index)
        {
            const auto type = index < 2 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            ranges[index].Init(type, 1, index % 2);
            parameters[index].InitAsDescriptorTable(1, &ranges[index]);
        }
        const CD3DX12_ROOT_SIGNATURE_DESC description(4, parameters);
        ComPtr<ID3DBlob> serialized;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)) ||
            FAILED(g_Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                 IID_PPV_ARGS(&g_Root))))
        {
            return false;
        }
        auto createPipeline = [](const wchar_t* path, ComPtr<ID3D12PipelineState>& pipeline) {
            ComPtr<ID3DBlob> shader;
            if (FAILED(D3DReadFileToBlob(path, &shader)))
            {
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
            description.pRootSignature = g_Root.Get();
            description.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
            return SUCCEEDED(g_Device->CreateComputePipelineState(&description, IID_PPV_ARGS(&pipeline)));
        };
        return createPipeline(L"shader/hlsl/build/DlssPrepareCS.cso", g_PreparePipeline) &&
               createPipeline(L"shader/hlsl/build/DlssCopyCS.cso", g_CopyPipeline);
    }

    bool CreateTexture(UINT width, UINT height, DXGI_FORMAT format, const wchar_t* name,
                       ComPtr<ID3D12Resource>& texture)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description =
            CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&texture))))
        {
            return false;
        }
        texture->SetName(name);
        return true;
    }

    bool EnsureFeature(ID3D12GraphicsCommandList* commands, UINT width, UINT height, UINT outputWidth,
                       UINT outputHeight)
    {
        const bool hdrInput = ImGuiManager::IsHdrEnabled() && !ImGuiManager::IsToneMapEnabled();
        if (g_Feature && g_InputWidth == width && g_InputHeight == height && g_OutputWidth == outputWidth &&
            g_OutputHeight == outputHeight && g_FeatureHdr == hdrInput)
        {
            return true;
        }
        if (!GraphicsDevice::WaitForGpuIdle())
        {
            return false;
        }
        if (g_Feature)
        {
            NVSDK_NGX_D3D12_ReleaseFeature(g_Feature);
            g_Feature = nullptr;
        }
        g_Frames = {};
        for (UINT index = 0; index < kFrameCount; ++index)
        {
            auto& frame = g_Frames[index];
            if (!CreateTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"DLSS Input Color", frame.Color) ||
                !CreateTexture(width, height, DXGI_FORMAT_R16G16_FLOAT, L"DLSS Motion Vectors", frame.Motion) ||
                !CreateTexture(outputWidth, outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, L"DLSS Output", frame.Output))
            {
                return false;
            }
            const UINT first = kDescriptorStart + index * kDescriptorsPerFrame;
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            view.Format = DXGI_FORMAT_R16G16_FLOAT;
            g_Device->CreateUnorderedAccessView(frame.Motion.Get(), nullptr, &view, CpuHandle(first));
            view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g_Device->CreateUnorderedAccessView(frame.Color.Get(), nullptr, &view, CpuHandle(first + 1));
            D3D12_SHADER_RESOURCE_VIEW_DESC outputView{};
            outputView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            outputView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            outputView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            outputView.Texture2D.MipLevels = 1;
            g_Device->CreateShaderResourceView(frame.Output.Get(), &outputView, CpuHandle(first + 2));
        }
        NVSDK_NGX_DLSS_Create_Params parameters{};
        parameters.Feature.InWidth = width;
        parameters.Feature.InHeight = height;
        parameters.Feature.InTargetWidth = outputWidth;
        parameters.Feature.InTargetHeight = outputHeight;
        const float scale = static_cast<float>(width) / outputWidth;
        parameters.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
        if (scale <= 0.38f)
        {
            parameters.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        }
        else if (scale <= 0.52f)
        {
            parameters.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxPerf;
        }
        else if (scale < 0.64f)
        {
            parameters.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced;
        }
        parameters.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_MVJittered |
            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        if (hdrInput)
        {
            parameters.InFeatureCreateFlags |=
                NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        }
        const auto result = NGX_D3D12_CREATE_DLSS_EXT(commands, 1, 1, &g_Feature, g_Parameters, &parameters);
        if (NVSDK_NGX_FAILED(result))
        {
            WriteGraphicsLog("DLSS: feature creation failed 0x%08x\n", static_cast<unsigned>(result));
            g_Status = "DLSS feature creation failed; using Bilateral";
            return false;
        }
        g_InputWidth = width;
        g_InputHeight = height;
        g_OutputWidth = outputWidth;
        g_OutputHeight = outputHeight;
        g_FeatureHdr = hdrInput;
        g_ResetHistory = true;
        WriteGraphicsLog("DLSS: feature %ux%u -> %ux%u mode=%d HDR=%d\n", width, height, outputWidth, outputHeight,
                         static_cast<int>(parameters.Feature.InPerfQualityValue), static_cast<int>(hdrInput));
        return true;
    }

    void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* texture, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture, before, after);
        commands->ResourceBarrier(1, &barrier);
    }
#endif

    float Halton(UINT index, UINT base)
    {
        float result = 0.0f;
        float weight = 1.0f;
        while (index != 0)
        {
            weight /= base;
            result += (index % base) * weight;
            index /= base;
        }
        return result;
    }
} // 名前空間

void DlssUpscaler::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT increment)
{
    Shutdown();
    g_Device = device;
    g_Heap = heap;
    g_Increment = increment;
#ifdef RENDERINGPROJECT_USE_DLSS
    char disabled[8]{};
    if (GetEnvironmentVariableA("DX12_DISABLE_DLSS", disabled, sizeof(disabled)) > 0 && atoi(disabled) != 0)
    {
        g_Status = "DLSS disabled for compatibility validation";
        return;
    }
    if (!device || !heap || increment == 0 || !CreateHelperPipelines())
    {
        g_Status = "DLSS helper pipeline is unavailable";
        return;
    }
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto directory = filesystem::path(executable).parent_path().wstring();
    const wchar_t* paths[] = {directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    const auto logs = filesystem::absolute("bin/ngx").wstring();
    error_code error;
    filesystem::create_directories(logs, error);
    const auto result = NVSDK_NGX_D3D12_Init_with_ProjectID(
        "ddaac575-3a1b-4406-94ce-e13d43222fe2", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", logs.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(result))
    {
        WriteGraphicsLog("DLSS: NGX initialization unavailable 0x%08x\n", static_cast<unsigned>(result));
        g_Status = "NGX unavailable on this GPU/driver; using Bilateral";
        return;
    }
    g_Initialized = true;
    int available = 0;
    int needsDriver = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&g_Parameters)))
    {
        g_Parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        g_Parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    }
    g_Available = available != 0 && needsDriver == 0;
    g_Status =
        g_Available ? "DLSS Super Resolution ready" : "DLSS unsupported or driver update required; using Bilateral";
    WriteGraphicsLog("DLSS: %s\n", g_Status.c_str());
#endif
}

void DlssUpscaler::Shutdown()
{
#ifdef RENDERINGPROJECT_USE_DLSS
    if (g_Feature)
    {
        NVSDK_NGX_D3D12_ReleaseFeature(g_Feature);
        g_Feature = nullptr;
    }
    if (g_Parameters)
    {
        NVSDK_NGX_D3D12_DestroyParameters(g_Parameters);
        g_Parameters = nullptr;
    }
    if (g_Initialized)
    {
        NVSDK_NGX_D3D12_Shutdown1(g_Device);
    }
    g_Initialized = false;
    g_Frames = {};
    g_Root.Reset();
    g_PreparePipeline.Reset();
    g_CopyPipeline.Reset();
#endif
    g_Available = false;
    g_Active = false;
    g_Prepared = false;
    g_ResetHistory = true;
    g_LoggedEvaluation = false;
    g_JitterFrame = 0;
    g_Jitter = {};
    g_Camera = g_kINVALID_ENTITY;
    g_Status = "NGX SDK is not installed";
}

bool DlssUpscaler::IsAvailable()
{
    return g_Available;
}

bool DlssUpscaler::IsActive()
{
    return g_Active;
}

const char* DlssUpscaler::GetStatus()
{
    return g_Status.c_str();
}

XMFLOAT2 DlssUpscaler::GetJitter()
{
    return g_Jitter;
}

bool DlssUpscaler::IsScaleSupported(UINT width, UINT height, UINT outputWidth, UINT outputHeight)
{
    return g_Available && width > 0 && height > 0 && width < outputWidth && height < outputHeight &&
           static_cast<uint64_t>(width) * 4 >= outputWidth && static_cast<uint64_t>(height) * 4 >= outputHeight;
}

bool DlssUpscaler::IsRequested()
{
    return RenderConfiguration::GetRenderMode() == RenderMode::DEFERRED &&
           RendererSettings::GetUpscaleMode() == UpscaleMode::Dlss &&
           IsScaleSupported(GraphicsDevice::GetSceneWidth(), GraphicsDevice::GetSceneHeight(),
                            GraphicsDevice::GetWidth(), GraphicsDevice::GetHeight());
}

void DlssUpscaler::PrepareFrame(EntityID camera)
{
    g_Active = false;
    const bool requested = IsRequested();
    if (!requested || !g_Prepared || camera != g_Camera)
    {
        g_ResetHistory = true;
        g_JitterFrame = 0;
    }
    g_Jitter = {};
    if (requested)
    {
        const UINT phaseCount =
            max(8u, static_cast<UINT>(8.0f * GraphicsDevice::GetWidth() / GraphicsDevice::GetSceneWidth() *
                                      GraphicsDevice::GetHeight() / GraphicsDevice::GetSceneHeight()));
        const UINT index = g_JitterFrame++ % phaseCount + 1;
        g_Jitter = {Halton(index, 2) - 0.5f, Halton(index, 3) - 0.5f};
    }
    g_Prepared = requested;
    g_Camera = camera;
}

bool DlssUpscaler::Execute(ID3D12GraphicsCommandList* commands, UINT frameIndex, ID3D12Resource* color,
                           D3D12_GPU_DESCRIPTOR_HANDLE colorSrv, ID3D12Resource* depth, ID3D12Resource* velocity,
                           D3D12_GPU_DESCRIPTOR_HANDLE velocitySrv, ID3D12Resource* output,
                           D3D12_GPU_DESCRIPTOR_HANDLE outputUav)
{
    g_Active = false;
#ifdef RENDERINGPROJECT_USE_DLSS
    if (!IsRequested() || !g_Prepared || !commands || !color || !depth || !velocity || !output ||
        frameIndex >= kFrameCount)
    {
        return false;
    }
    const auto input = color->GetDesc();
    const auto target = output->GetDesc();
    if (!EnsureFeature(commands, static_cast<UINT>(input.Width), input.Height, static_cast<UINT>(target.Width),
                       target.Height))
    {
        g_Available = false;
        return false;
    }
    auto& frame = g_Frames[frameIndex];
    const UINT first = kDescriptorStart + frameIndex * kDescriptorsPerFrame;
    Transition(commands, frame.Motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commands, frame.Color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {g_Heap};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(g_Root.Get());
    commands->SetPipelineState(g_PreparePipeline.Get());
    commands->SetComputeRootDescriptorTable(0, velocitySrv);
    commands->SetComputeRootDescriptorTable(1, colorSrv);
    commands->SetComputeRootDescriptorTable(2, GpuHandle(first));
    commands->SetComputeRootDescriptorTable(3, GpuHandle(first + 1));
    commands->Dispatch((g_InputWidth + 7) / 8, (g_InputHeight + 7) / 8, 1);
    Transition(commands, frame.Motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commands, frame.Color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commands, frame.Output.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    NVSDK_NGX_D3D12_DLSS_Eval_Params evaluation{};
    evaluation.Feature.pInColor = frame.Color.Get();
    evaluation.Feature.pInOutput = frame.Output.Get();
    evaluation.pInDepth = depth;
    evaluation.pInMotionVectors = frame.Motion.Get();
    evaluation.InJitterOffsetX = g_Jitter.x;
    evaluation.InJitterOffsetY = g_Jitter.y;
    evaluation.InMVScaleX = static_cast<float>(g_InputWidth);
    evaluation.InMVScaleY = static_cast<float>(g_InputHeight);
    evaluation.InRenderSubrectDimensions = {g_InputWidth, g_InputHeight};
    evaluation.InReset = g_ResetHistory;
    const auto result = NGX_D3D12_EVALUATE_DLSS_EXT(commands, g_Feature, g_Parameters, &evaluation);
    Transition(commands, frame.Output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (NVSDK_NGX_FAILED(result))
    {
        g_Available = false;
        g_Status = "DLSS evaluation failed; using Bilateral";
        WriteGraphicsLog("DLSS: evaluation failed 0x%08x\n", static_cast<unsigned>(result));
        return false;
    }
    // NGX は記述子ヒープとコンピュートのバインドを変更する可能性がある。
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(g_Root.Get());
    commands->SetPipelineState(g_CopyPipeline.Get());
    commands->SetComputeRootDescriptorTable(0, GpuHandle(first + 2));
    commands->SetComputeRootDescriptorTable(2, outputUav);
    commands->Dispatch((g_OutputWidth + 7) / 8, (g_OutputHeight + 7) / 8, 1);
    g_ResetHistory = false;
    g_Active = true;
    if (!g_LoggedEvaluation)
    {
        WriteGraphicsLog("DLSS: first evaluation %ux%u -> %ux%u\n", g_InputWidth, g_InputHeight, g_OutputWidth,
                         g_OutputHeight);
        g_LoggedEvaluation = true;
    }
    return true;
#else
    return false;
#endif
}
