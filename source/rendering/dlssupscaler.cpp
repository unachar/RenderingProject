#include "pch.h"
#include "dlssupscaler.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "rendererstate.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "imguimanager.h"
#include "temporalhistorycontext.h"
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
    // 最適化後は Motion の UAV 1 個だけを使う（予約領域は従来どおり kFrameCount * 3 個分）。
    constexpr UINT kDescriptorsPerFrame = 3;
    static_assert(kDescriptorStart + kDescriptorsPerFrame * kFrameCount <= RendererState::g_kENGINE_DESCRIPTOR_END,
                  "DLSS descriptors exceed the reserved engine heap");
    constexpr UINT kMotionUav = kDescriptorStart;

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
    uint64_t g_CameraCutGeneration = 0;
    bool g_JitterSignFlipped = false;
    UINT g_FeatureRecreates = 0;
    UINT64 g_ResourceBytes = 0;
    string g_Status = "NGX SDK is not installed";

#ifdef RENDERINGPROJECT_USE_DLSS
    NVSDK_NGX_Parameter* g_Parameters = nullptr;
    NVSDK_NGX_Handle* g_Feature = nullptr;
    bool g_Initialized = false;
    UINT g_InputWidth = 0;
    UINT g_InputHeight = 0;
    UINT g_OutputWidth = 0;
    UINT g_OutputHeight = 0;
    UINT g_RenderWidth = 0; // 今フレームの実入力（サブ矩形）
    UINT g_RenderHeight = 0;
    bool g_FeatureHdr = false;
    ComPtr<ID3D12RootSignature> g_Root;
    ComPtr<ID3D12PipelineState> g_PreparePipeline;

    // 最適化：
    //  - 色はポストプロセス出力を NGX へ直接渡す（入力コピーのフルスクリーンパスを削除）。
    //  - 出力は EditorScene（UAV 付き・同じフォーマット）へ NGX が直接書く（出力コピーのパスと
    //    表示解像度の RGBA16F 中間テクスチャ ×フレーム数を削除）。
    //  - Motion はコマンドキュー上で直列に使われるのでフレームごとに持つ必要がない（1 枚に統合）。
    ComPtr<ID3D12Resource> g_Motion;

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
        // t0 = エンジンの Velocity, u0 = NGX 用 Motion（RG16F）
        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        CD3DX12_ROOT_PARAMETER parameters[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        parameters[0].InitAsDescriptorTable(1, &ranges[0]);
        parameters[1].InitAsDescriptorTable(1, &ranges[1]);
        const CD3DX12_ROOT_SIGNATURE_DESC description(2, parameters);
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
        return createPipeline(L"shader/hlsl/build/DlssPrepareCS.cso", g_PreparePipeline);
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

    // 実際の内部解像度の比率から NGX の品質モードを選ぶ。DLSS は品質モードごとに
    // その倍率向けに調整されたモデル（プリセット）を使うため、倍率と合っていないと低解像度でぼやける。
    NVSDK_NGX_PerfQuality_Value QualityForScale(float scale)
    {
        if (scale <= 0.38f)
        {
            return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        }
        if (scale <= 0.52f)
        {
            return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        }
        if (scale < 0.64f)
        {
            return NVSDK_NGX_PerfQuality_Value_Balanced;
        }
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }

    NVSDK_NGX_PerfQuality_Value g_FeatureQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    int g_FeaturePreset = 0;

    bool EnsureFeature(ID3D12GraphicsCommandList* commands, UINT width, UINT height, UINT outputWidth,
                       UINT outputHeight)
    {
        const NVSDK_NGX_PerfQuality_Value quality =
            QualityForScale(static_cast<float>(width) / static_cast<float>((std::max)(outputWidth, 1u)));
        const bool hdrInput = ImGuiManager::IsHdrEnabled() && !ImGuiManager::IsToneMapEnabled();
        // 入力が縮んだだけなら作り直さず、サブ矩形（InRenderSubrectDimensions）で評価する。
        // 動的解像度のたびに GPU 待機 + 再作成していたヒッチを無くす。
        if (g_Feature && width <= g_InputWidth && height <= g_InputHeight && g_OutputWidth == outputWidth &&
            g_OutputHeight == outputHeight && g_FeatureHdr == hdrInput && g_FeatureQuality == quality &&
            g_FeaturePreset == DlssUpscaler::GetSettings().RenderPreset)
        {
            // 品質モードの区分（Quality / Balanced / Performance / Ultra Performance）が変わった時だけ作り直す。
            // DLSS は動的解像度（サブ矩形の変化）を履歴を保ったまま扱える。
            // 以前はここで履歴を捨てていたため、解像度が変わるたびに数フレームぼやけていた。
            g_RenderWidth = width;
            g_RenderHeight = height;
            return true;
        }
        ++g_FeatureRecreates;
        // 作成サイズに余裕を持たせる：解像度を下げてから戻した時（自動品質調整など）に
        // 作り直し（GPU 待機）が起きないよう、Ultra Quality 相当（表示の 77%）以上で作る。
        const UINT requestedWidth = width, requestedHeight = height;
        width = (std::min)(outputWidth - 1, (std::max)(width, (outputWidth * 77u + 99u) / 100u));
        height = (std::min)(outputHeight - 1, (std::max)(height, (outputHeight * 77u + 99u) / 100u));
        if (!GraphicsDevice::WaitForGpuIdle())
        {
            return false;
        }
        if (g_Feature)
        {
            NVSDK_NGX_D3D12_ReleaseFeature(g_Feature);
            g_Feature = nullptr;
        }
        g_Motion.Reset();
        g_ResourceBytes = 0;
        if (!CreateTexture(width, height, DXGI_FORMAT_R16G16_FLOAT, L"DLSS Motion Vectors", g_Motion))
        {
            return false;
        }
        g_ResourceBytes = static_cast<UINT64>(width) * height * 4;
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            view.Format = DXGI_FORMAT_R16G16_FLOAT;
            g_Device->CreateUnorderedAccessView(g_Motion.Get(), nullptr, &view, CpuHandle(kMotionUav));
        }
        NVSDK_NGX_DLSS_Create_Params parameters{};
        parameters.Feature.InWidth = width;
        parameters.Feature.InHeight = height;
        parameters.Feature.InTargetWidth = outputWidth;
        parameters.Feature.InTargetHeight = outputHeight;
        // 余裕を持たせた作成サイズではなく、実際の倍率で品質モードを決める
        // （前回の修正では作成サイズ＝77% 以上で判定していたため、50% でも Quality 用のモデルになっていた）。
        parameters.Feature.InPerfQualityValue = quality;
        g_FeatureQuality = quality;
        parameters.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_MVJittered |
            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        if (hdrInput)
        {
            parameters.InFeatureCreateFlags |=
                NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        }
        // レンダープリセット（全品質モードに同じ値を指定）。古い SDK ヘッダーには定義が無いのでガードする。
#ifdef NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality
        {
            const unsigned int preset = static_cast<unsigned int>(std::clamp(DlssUpscaler::GetSettings().RenderPreset, 0, 15));
            NVSDK_NGX_Parameter_SetUI(g_Parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
            NVSDK_NGX_Parameter_SetUI(g_Parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
            NVSDK_NGX_Parameter_SetUI(g_Parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
            NVSDK_NGX_Parameter_SetUI(g_Parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
            NVSDK_NGX_Parameter_SetUI(g_Parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                                      preset);
        }
#endif
        // SDK が未対応でも記録する（記録しないと毎フレーム作り直しになる）。
        g_FeaturePreset = DlssUpscaler::GetSettings().RenderPreset;
        const auto result = NGX_D3D12_CREATE_DLSS_EXT(commands, 1, 1, &g_Feature, g_Parameters, &parameters);
        if (NVSDK_NGX_FAILED(result))
        {
            WriteGraphicsLog("DLSS: feature creation failed 0x%08x\n", static_cast<unsigned>(result));
            g_Status = "DLSS feature creation failed; using Bilateral";
            return false;
        }
        g_InputWidth = width;
        g_InputHeight = height;
        g_RenderWidth = requestedWidth;
        g_RenderHeight = requestedHeight;
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
    g_Motion.Reset();
    g_ResourceBytes = 0;
    g_Root.Reset();
    g_PreparePipeline.Reset();
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

DlssUpscaler::Settings& DlssUpscaler::GetSettings()
{
    static Settings settings;
    return settings;
}

float DlssUpscaler::GetSharpnessStops()
{
    float strength = std::clamp(GetSettings().Sharpness, 0.0f, 1.0f);
    if (strength <= 0.001f)
    {
        return -1.0f;
    }
    // RCAS：0 stops = 最強、2 stops = ごく弱い。strength 1 → 0 stops、strength 0+ → 2 stops。
    return (1.0f - strength) * 2.0f;
}

float DlssUpscaler::GetJitterProjectionSign()
{
    return g_JitterSignFlipped ? -1.0f : 1.0f;
}

bool& DlssUpscaler::JitterSignFlipped()
{
    return g_JitterSignFlipped;
}

UINT DlssUpscaler::GetFeatureRecreateCount()
{
    return g_FeatureRecreates;
}

UINT64 DlssUpscaler::GetResourceBytes()
{
    return g_ResourceBytes;
}

bool DlssUpscaler::IsScaleSupported(UINT width, UINT height, UINT outputWidth, UINT outputHeight)
{
    return g_Available && width > 0 && height > 0 && width < outputWidth && height < outputHeight &&
           static_cast<uint64_t>(width) * 3 >= outputWidth && static_cast<uint64_t>(height) * 3 >= outputHeight;
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
    const uint64_t cutGeneration = TemporalHistory::Context::Capture(camera, 0, 0, 0).CameraCutGeneration;
    if (!requested || !g_Prepared || camera != g_Camera || cutGeneration != g_CameraCutGeneration)
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
    g_CameraCutGeneration = cutGeneration;
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
    (void)colorSrv;
    (void)outputUav;
    // 1) Motion だけ整形する（エンジンは ZW に「現 UV - 前 UV」、NGX は RG に逆向きを要求）。
    //    色のコピーは行わない。
    Transition(commands, g_Motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {g_Heap};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(g_Root.Get());
    commands->SetPipelineState(g_PreparePipeline.Get());
    commands->SetComputeRootDescriptorTable(0, velocitySrv);
    commands->SetComputeRootDescriptorTable(1, GpuHandle(kMotionUav));
    commands->Dispatch((g_RenderWidth + 7) / 8, (g_RenderHeight + 7) / 8, 1);
    Transition(commands, g_Motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 2) NGX：入力 = ポストプロセス出力（呼び出し側で NON_PIXEL_SHADER_RESOURCE）、
    //    出力 = EditorScene（呼び出し側で UNORDERED_ACCESS）。中間テクスチャとコピーは無し。
    NVSDK_NGX_D3D12_DLSS_Eval_Params evaluation{};
    evaluation.Feature.pInColor = color;
    evaluation.Feature.pInOutput = output;
    evaluation.pInDepth = depth;
    evaluation.pInMotionVectors = g_Motion.Get();
    evaluation.InJitterOffsetX = g_Jitter.x;
    evaluation.InJitterOffsetY = g_Jitter.y;
    evaluation.InMVScaleX = static_cast<float>(g_RenderWidth);
    evaluation.InMVScaleY = static_cast<float>(g_RenderHeight);
    evaluation.InRenderSubrectDimensions = {g_RenderWidth, g_RenderHeight};
    evaluation.InReset = g_ResetHistory;
    const auto result = NGX_D3D12_EVALUATE_DLSS_EXT(commands, g_Feature, g_Parameters, &evaluation);
    if (NVSDK_NGX_FAILED(result))
    {
        g_Available = false;
        g_Status = "DLSS evaluation failed; using Bilateral";
        WriteGraphicsLog("DLSS: evaluation failed 0x%08x\n", static_cast<unsigned>(result));
        return false;
    }
    // NGX は記述子ヒープを変更する可能性がある（呼び出し側は RenderFrame::SetDescriptorHeap で復元する）。
    commands->SetDescriptorHeaps(1, heaps);
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

