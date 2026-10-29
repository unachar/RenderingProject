#include "pch.h"
#include "noisecachebindings.h"
#include "envflags.h"
#include "cpuculling.h"
#include "rayreflections.h"
#include "raytracing.h"
#include "scenedepth.h"
#include "rendererstate.h"
#include "rendertargets.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "lightingresources.h"
#include "world.h"
#include "renderersettings.h"
#include "optixdenoiser.h"
#include "atmospherelut.h"
#include "renderquality.h"
#include "raytelemetry.h"
#include "lumensurfacecache.h"
#include "temporalhistory.h"
#include "gpumemorybudget.h"
#include <map>
#include <algorithm>

namespace
{
    constexpr UINT kFrames = RendererState::g_kFRAME_COUNT;
    constexpr UINT kMaximumMeshes = 16384;
    // 0～3は GBuffer とフィルター済み入力、4～7は信号 UAV、8～11は GI 入力、12～13は空
    // LUT、16～19は放射輝度キャッシュ。
    constexpr UINT kSkyLutDescriptors = 12;
    constexpr UINT kCacheDescriptors = 16;
    // 20～21は ReSTIR リザーバー（現在、履歴）の UAV、22は速度 SRV。
    constexpr UINT kReSTIRDescriptors = 20;
    constexpr UINT kLumenDescriptors = 24;
    constexpr UINT kVertexDescriptors = 40;
    // ワールド空間の放射照度キャッシュ（ハッシュグリッド）。2^18 セルで、キーは1 MiB、蓄積値は4 MiB、
    // 解決済み値は4 MiB、経過フレーム数は1 MiB。
    constexpr UINT kCacheCapacity = 1u << 18;
    constexpr UINT kIndexDescriptors = kVertexDescriptors + kMaximumMeshes;
    constexpr UINT kTextureDescriptors = kIndexDescriptors + kMaximumMeshes;
    constexpr UINT kEmissionDescriptors = kTextureDescriptors + kMaximumMeshes;
    // POM の高さマップ（メッシュごと、t0 space12）。
    constexpr UINT kHeightDescriptors = kEmissionDescriptors + kMaximumMeshes;
    constexpr UINT kGiSrv = RendererState::g_kHIZ_MIP_SRV_START_INDEX + 2 * RendererState::g_kHIZ_MAX_MIPS + 44;
    static_assert(kGiSrv + kFrames <= RendererState::g_kCLOUD_DESCRIPTOR_START);
    constexpr UINT kResultSrv = RendererState::g_kHIZ_MIP_SRV_START_INDEX + 2 * RendererState::g_kHIZ_MAX_MIPS + 40;
    static_assert(kResultSrv + kFrames <= RendererState::g_kENGINE_DESCRIPTOR_END);

    struct MeshConstants
    {
        XMFLOAT4X4 World{};
        XMFLOAT4X4 NormalToWorld{};
        XMUINT4 Layout{};
        XMFLOAT4 Material{};
        XMFLOAT4 BaseColor{};
        XMFLOAT4 Emission{};
        XMUINT4 Surface{};
        std::array<ProcStudio::TextureGpuParams, 3> Noise{};
        XMFLOAT4 NoiseSurface{};
    };

    struct FrameConstants
    {
        XMFLOAT4X4 InvViewProjection{};
        XMFLOAT4X4 View{};
        XMFLOAT4X4 ViewProjection{};
        XMFLOAT4 Camera{};
        XMUINT4 Extent{};
        XMFLOAT4 GiParameters{};
        XMFLOAT4 CacheParameters{};  // 有効、基準セルサイズ、容量、最大履歴フレーム数
        XMFLOAT4 ReSTIRParameters{}; // 有効、時間方向の最大 M、空間サンプル数、空間半径
        XMFLOAT4X4 AdaptivePreviousViewProjection{};
        XMFLOAT4 AdaptiveSampling{};
        XMUINT4 ReSTIRState{}; // 履歴有効、リザーバー幅、空間可視性、未使用
        XMFLOAT4 Stability{};
    };

    struct ReflectionLight
    {
        XMFLOAT4 Direction{}, Color{}, PositionType{}, Extras{};
        XMUINT4 Flags{};
    };

    static_assert(sizeof(ReflectionLight) == 80);
    static_assert(sizeof(MeshConstants) == 848 && sizeof(FrameConstants) <= 512);
    ComPtr<ID3D12Resource> g_AdaptiveRadiance, g_AdaptiveGuide;
    ComPtr<ID3D12PipelineState> g_AdaptiveCapture;
    bool g_AdaptiveValid = false, g_BudgetValid = false;
    uint64_t g_AdaptiveSignature = 0, g_BudgetSignature = 0;
    UINT g_AdaptiveSequence = 0, g_BudgetSequence = 0;

    struct RadianceCache
    {
        ComPtr<ID3D12Resource> Keys, Accumulation, Resolved, Ages;
        bool NeedsClear = true;
    };

    struct FrameResources
    {
        ComPtr<ID3D12Resource> DisabledLumen;
        ComPtr<ID3D12DescriptorHeap> Heap;
        ComPtr<ID3D12Resource> Meshes, Lights, Emitters, Constants, Noisy, Result, GiNoisy, GiResult;
        // このフレームスロットのフェンスが完了するまで、参照するリソースを保持する。
        vector<RaytracingGeometry> Geometry;

        // 負荷削減：メッシュごとの SRV（頂点 / インデックス / テクスチャ / 発光）が前回と同じなら作り直さない
        // （参照を保持するので、解放されたアドレスに別のリソースが入って同じポインタになることはない）
        struct DescriptorRecord
        {
            ComPtr<ID3D12Resource> Vertices, Indices, Texture, Emission, Height;
            UINT TextureMip = 0;
            UINT EmissionMip = 0;
            UINT HeightMip = 0;
            bool Valid = false;
        };

        vector<DescriptorRecord> Descriptors;
        UINT Width = 0, Height = 0;
    };

    ComPtr<ID3D12Device> g_Device;

    bool EnsureAdaptiveHistory(UINT width, UINT height)
    {
        if (g_AdaptiveRadiance && g_AdaptiveRadiance->GetDesc().Width == width &&
            g_AdaptiveRadiance->GetDesc().Height == height)
        {
            return true;
        }
        if (!GraphicsDevice::WaitForGpuIdle())
        {
            return false;
        }
        g_AdaptiveValid = false;
        ComPtr<ID3D12Resource> a, b;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto radiance = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1);
        const auto guide = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1, 1, 0,
                                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &radiance,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&a))) ||
            FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &guide,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&b))))
        {
            return false;
        }
        g_AdaptiveRadiance = a;
        g_AdaptiveGuide = b;
        return true;
    }

    ComPtr<ID3D12DescriptorHeap> g_EngineHeap;
    ComPtr<ID3D12RootSignature> g_Root;
    ComPtr<ID3D12PipelineState> g_Trace, g_Compose, g_GiTrace, g_GiCompose, g_GiFilter, g_CacheResolve, g_CacheClear;
    ComPtr<ID3D12PipelineState> g_ReSTIRTemporal, g_ReSTIRSpatial;
    ComPtr<ID3D12PipelineState> g_TraceTelemetry, g_GiTraceTelemetry, g_ReSTIRSpatialTelemetry;
    bool g_TelemetryPipelinesTried = false;
    // ReSTIR GI リザーバー（追跡ピクセルごとに32バイト）。現在と履歴を交互に使う。
    ComPtr<ID3D12Resource> g_Reservoirs[2];
    UINT g_ReservoirWidth = 0, g_ReservoirHeight = 0, g_ReservoirCurrent = 0;
    bool g_ReSTIRHistoryValid = false;
    RadianceCache g_Cache;
    LumenSurfaceCache g_Lumen;
    ComPtr<ID3D12PipelineState> g_LumenBounds, g_LumenCapture, g_LumenClear, g_LumenLighting, g_LumenGather;
    ComPtr<ID3D12PipelineState> g_LumenCaptureTelemetry, g_LumenLightingTelemetry, g_LumenGatherTelemetry;
    uint64_t g_LumenSignature = 0;
    UINT g_LumenSequence = 0;
    UINT g_LumenQuietFrames = 8;
    bool g_LumenWasEnabled = false;
    bool g_CacheWasEnabled = false;
    // 反射とリアルタイム GI が動作しない理由を示す診断情報。
    const char* g_Status = "未初期化";
    string g_LastShaderError;
    array<FrameResources, kFrames> g_Frames;
    unique_ptr<NrdDenoiser> g_Denoiser, g_GiDenoiser;
    unique_ptr<OptixDenoiser> g_OptixGi, g_OptixReflections;
    bool g_OptixGiActive = false, g_OptixReflectionActive = false;
    UINT g_GiWidth = 0, g_GiHeight = 0;
    bool g_GiActive = false, g_GiHistoryValid = false;
    uint64_t g_GiEmissionSignature = 0;
    // 発光のシグネチャが変化しなかったフレーム数。変化が止まった後だけ再開する。
    constexpr UINT kGiEmissionRestartQuietFrames = 8;
    UINT g_GiEmissionStableFrames = kGiEmissionRestartQuietFrames;
    UINT g_Increment = 0, g_Width = 0, g_Height = 0, g_Dispatches = 0;
    bool g_ReflectionCheckerboard = false; // このフレームの反射を半分のピクセルでトレースした
    bool g_DenoiserCheckerboard = false;   // 作成済みの反射デノイザーのモード
    UINT g_LastFrame = 0;
    bool g_HistoryValid = false;
    RayReflections::SamplingLayout g_ReflectionSampling;

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* heap, UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(), index, g_Increment);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* heap, UINT index)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), index, g_Increment);
    }

    bool EnsureUpload(ComPtr<ID3D12Resource>& resource, UINT64 bytes)
    {
        if (resource && resource->GetDesc().Width >= bytes)
        {
            return true;
        }
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        return SUCCEEDED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())));
    }

    bool Upload(ID3D12Resource* resource, const void* data, size_t bytes)
    {
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (FAILED(resource->Map(0, &noRead, &mapped)))
        {
            return false;
        }
        memcpy(mapped, data, bytes);
        resource->Unmap(0, nullptr);
        return true;
    }

    void HashEmission(uint64_t& signature, const void* data, size_t bytes)
    {
        const auto* values = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < bytes; ++index)
        {
            signature ^= values[index];
            signature *= 1099511628211ull;
        }
    }

    void TextureSrv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT minimumResidentMip = 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Format = resource ? resource->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (desc.Format == DXGI_FORMAT_R32_TYPELESS)
        {
            desc.Format = DXGI_FORMAT_R32_FLOAT;
        }
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = resource ? resource->GetDesc().MipLevels : 1;
        desc.Texture2D.ResourceMinLODClamp = static_cast<float>(minimumResidentMip);
        g_Device->CreateShaderResourceView(resource, &desc, handle);
    }

    void BufferSrv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.NumElements = resource ? static_cast<UINT>(resource->GetDesc().Width / 4) : 1;
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        g_Device->CreateShaderResourceView(resource, &desc, handle);
    }

    // 放射輝度信号はダイレクトキューの1つのコマンドリスト内で生成して利用する。
    // GBuffer と同じ前提により、従来のフレームスロットごとの複製を1組にまとめる。
    // 全解像度 RGBA16F テクスチャ4枚を、3スロット分から1組へ削減する。
    // 各スロットは記録時に使った組への参照を保持し、実行中のフレームが読むテクスチャを
    // リサイズで解放しないようにする。
    struct SharedSignals
    {
        ComPtr<ID3D12Resource> Noisy, Result, GiNoisy, GiResult;
        UINT Width = 0, Height = 0;
    };

    SharedSignals g_Shared;

    bool EnsureFrame(FrameResources& frame, UINT slot, UINT width, UINT height)
    {
        if (!frame.Heap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            desc.NumDescriptors = kHeightDescriptors + kMaximumMeshes;
            if (FAILED(g_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&frame.Heap))))
            {
                return false;
            }
            frame.Descriptors.clear(); // 新しいヒープは中身が空
        }
        if (!EnsureUpload(frame.Constants, 512))
        {
            return false;
        }
        if (g_Shared.Width != width || g_Shared.Height != height)
        {
            // 共有参照だけを外す。古いテクスチャを保持するスロットがある間は解放されない。
            g_Shared = {};
            g_Shared.Width = width;
            g_Shared.Height = height;
            g_GiHistoryValid = false;
        }
        frame.Width = width;
        frame.Height = height;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1, 1, 0,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto ensureSignal = [&](ComPtr<ID3D12Resource>& sharedNoisy, ComPtr<ID3D12Resource>& sharedResult,
                                      ComPtr<ID3D12Resource>& noisy, ComPtr<ID3D12Resource>& result, UINT firstUav,
                                      UINT resultSrv, const wchar_t* name) {
            for (auto* resource : {addressof(sharedNoisy), addressof(sharedResult)})
            {
                if (*resource)
                {
                    continue;
                }
                if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                             IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()))))
                {
                    return false;
                }
                (*resource)->SetName(name);
            }
            if (noisy == sharedNoisy && result == sharedResult)
            {
                return true;
            }
            noisy = sharedNoisy;
            result = sharedResult;
            TextureSrv(result.Get(), Cpu(g_EngineHeap.Get(), resultSrv + slot));
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = desc.Format;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_Device->CreateUnorderedAccessView(noisy.Get(), nullptr, &uav, Cpu(frame.Heap.Get(), firstUav));
            g_Device->CreateUnorderedAccessView(result.Get(), nullptr, &uav, Cpu(frame.Heap.Get(), firstUav + 1));
            return true;
        };
        if (Raytracing::GetSettings().Reflections &&
            !ensureSignal(g_Shared.Noisy, g_Shared.Result, frame.Noisy, frame.Result, 4, kResultSrv,
                          L"Ray Reflection Radiance"))
        {
            return false;
        }
        if (RendererSettings::GetRealtimeGiEnabled() &&
            !ensureSignal(g_Shared.GiNoisy, g_Shared.GiResult, frame.GiNoisy, frame.GiResult, 6, kGiSrv,
                          L"Realtime GI Irradiance"))
        {
            return false;
        }
        return true;
    }

    bool EnsureReservoirs(UINT width, UINT height)
    {
        if (g_Reservoirs[0] && g_ReservoirWidth == width && g_ReservoirHeight == height)
        {
            return true;
        }
        // 先行フレームが前のリザーバーを読んでいる可能性がある。
        if (g_Reservoirs[0] && !GraphicsDevice::WaitForGpuIdle())
        {
            return false;
        }
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc =
            CD3DX12_RESOURCE_DESC::Buffer(UINT64(width) * height * 32ull, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        for (auto& buffer : g_Reservoirs)
        {
            if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                         IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf()))))
            {
                g_Reservoirs[0].Reset();
                g_Reservoirs[1].Reset();
                g_ReservoirWidth = g_ReservoirHeight = 0;
                return false;
            }
            buffer->SetName(L"ReSTIR GI Reservoirs");
        }
        g_ReservoirWidth = width;
        g_ReservoirHeight = height;
        g_ReservoirCurrent = 0;
        g_ReSTIRHistoryValid = false;
        return true;
    }

    bool CreateRadianceCache()
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto create = [&](ComPtr<ID3D12Resource>& buffer, UINT64 bytes, const wchar_t* name) {
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                         IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf()))))
            {
                return false;
            }
            buffer->SetName(name);
            return true;
        };
        g_Cache.NeedsClear = true;
        return create(g_Cache.Keys, kCacheCapacity * 4ull, L"GI Radiance Cache Keys") &&
               create(g_Cache.Accumulation, kCacheCapacity * 32ull, L"GI Radiance Cache Accumulation") &&
               create(g_Cache.Resolved, kCacheCapacity * 32ull, L"GI Radiance Cache Resolved") &&
               create(g_Cache.Ages, kCacheCapacity * 4ull, L"GI Radiance Cache Ages");
    }

    void WriteCacheDescriptors(ID3D12DescriptorHeap* heap)
    {
        const auto raw = [&](ID3D12Resource* buffer, UINT index) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.NumElements = buffer ? static_cast<UINT>(buffer->GetDesc().Width / 4) : 1;
            desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            g_Device->CreateUnorderedAccessView(buffer, nullptr, &desc, Cpu(heap, kCacheDescriptors + index));
        };
        raw(g_Cache.Keys.Get(), 0);
        raw(g_Cache.Accumulation.Get(), 1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC structured{};
        structured.Format = DXGI_FORMAT_UNKNOWN;
        structured.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        structured.Buffer.NumElements = kCacheCapacity;
        structured.Buffer.StructureByteStride = 32;
        g_Device->CreateUnorderedAccessView(g_Cache.Resolved.Get(), nullptr, &structured,
                                            Cpu(heap, kCacheDescriptors + 2));
        raw(g_Cache.Ages.Get(), 3);
    }

    void CacheBarrier(ID3D12GraphicsCommandList* commands)
    {
        const D3D12_RESOURCE_BARRIER barriers[] = {CD3DX12_RESOURCE_BARRIER::UAV(g_Cache.Keys.Get()),
                                                   CD3DX12_RESOURCE_BARRIER::UAV(g_Cache.Accumulation.Get()),
                                                   CD3DX12_RESOURCE_BARRIER::UAV(g_Cache.Resolved.Get()),
                                                   CD3DX12_RESOURCE_BARRIER::UAV(g_Cache.Ages.Get())};
        commands->ResourceBarrier(4, barriers);
    }

    bool CompileRadiancePipeline(const char* entry, ComPtr<ID3D12PipelineState>& pipeline, bool telemetry = false)
    {
        const D3D_SHADER_MACRO defines[] = {
            {"USE_NRD", NrdDenoiser::IsCompiled() ? "1" : "0"}, {"ENABLE_RAY_TELEMETRY", telemetry ? "1" : "0"}, {}};
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3DCompileFromFile(L"shader/hlsl/RayReflectionsCS.hlsl", defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                      entry, "cs_6_5", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
        {
            g_LastShaderError = string(entry) + ": " +
                                (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
            WriteGraphicsLog("DXR reflection shader failed (telemetry=%u): %s\n", static_cast<UINT>(telemetry),
                             g_LastShaderError.c_str());
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g_Root.Get();
        desc.CS = CD3DX12_SHADER_BYTECODE(blob.Get());
        if (SUCCEEDED(g_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()))))
        {
            return true;
        }
        g_LastShaderError = string(entry) + ": CreateComputePipelineState failed";
        return false;
    }

    bool EnsureTelemetryPipelines()
    {
        if (!g_TelemetryPipelinesTried)
        {
            g_TelemetryPipelinesTried = true;
            if (!CompileRadiancePipeline("main", g_TraceTelemetry, true) ||
                !CompileRadiancePipeline("TraceDiffuse", g_GiTraceTelemetry, true) ||
                (g_ReSTIRSpatial && !CompileRadiancePipeline("ReSTIRSpatial", g_ReSTIRSpatialTelemetry, true)) ||
                (g_LumenBounds && (!CompileRadiancePipeline("LumenCapture", g_LumenCaptureTelemetry, true) ||
                                   !CompileRadiancePipeline("LumenUpdateLighting", g_LumenLightingTelemetry, true) ||
                                   !CompileRadiancePipeline("LumenTraceProbes", g_LumenGatherTelemetry, true))))
            {
                g_TraceTelemetry.Reset();
                g_GiTraceTelemetry.Reset();
                g_ReSTIRSpatialTelemetry.Reset();
                WriteGraphicsLog("Ray telemetry: radiance counter variant unavailable; keeping normal shaders\n");
            }
        }
        return g_TraceTelemetry && g_GiTraceTelemetry;
    }

    void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
    {
        if (!resource || before == after)
        {
            return;
        }
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        commands->ResourceBarrier(1, &barrier);
    }
} // 名前空間

bool RayReflections::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap, UINT increment)
{
    g_Device = device;
    g_EngineHeap = engineHeap;
    g_Increment = increment;
    CD3DX12_DESCRIPTOR_RANGE ranges[15];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 1);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);
    for (UINT i = 2; i < 6; ++i)
    {
        ranges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumMeshes, 0, i - 1);
    }
    ranges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 6);    // スカイビュー LUT
    ranges[7].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 7);    // 放射輝度キャッシュ
    ranges[8].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 8, 0); // ReSTIR リザーバー
    ranges[9].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 8, 2); // 速度
    ranges[10].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 10);
    ranges[11].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 10);
    ranges[12].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 11, 0);
    ranges[13].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 11, 0, 11, 3);
    ranges[14].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumMeshes, 0, 12); // POM の高さマップ
    CD3DX12_ROOT_PARAMETER parameters[24];
    parameters[0].InitAsShaderResourceView(25);
    parameters[1].InitAsShaderResourceView(0);
    parameters[2].InitAsConstantBufferView(6);
    parameters[3].InitAsConstantBufferView(1);
    parameters[4].InitAsConstantBufferView(5);
    for (UINT i = 0; i < 5; ++i)
    {
        parameters[5 + i].InitAsDescriptorTable(1, &ranges[i]);
    }
    parameters[10].InitAsShaderResourceView(5);
    parameters[11].InitAsDescriptorTable(1, &ranges[5]);
    parameters[12].InitAsShaderResourceView(6);
    parameters[13].InitAsDescriptorTable(1, &ranges[6]);
    parameters[14].InitAsDescriptorTable(1, &ranges[7]);
    parameters[15].InitAsDescriptorTable(2, &ranges[8]);
    parameters[16].InitAsUnorderedAccessView(0, 9);
    parameters[17].InitAsDescriptorTable(1, &ranges[10]);
    parameters[18].InitAsDescriptorTable(1, &ranges[11]);
    parameters[19].InitAsDescriptorTable(2, &ranges[12]);
    parameters[20].InitAsConstantBufferView(7);
    parameters[21].InitAsDescriptorTable(1, &ranges[14]);
    parameters[22].InitAsShaderResourceView(0, 15);
    parameters[23].InitAsUnorderedAccessView(0, 15);
    const CD3DX12_STATIC_SAMPLER_DESC samplers[2] = {
        CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                    D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
        CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP)};
    const CD3DX12_ROOT_SIGNATURE_DESC root(_countof(parameters), parameters, 2, samplers);
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_Root))))
    {
        g_Status = "ルートシグネチャ作成失敗（反射 / Realtime GI 無効）";
        if (errors)
        {
            g_LastShaderError = static_cast<const char*>(errors->GetBufferPointer());
        }
        WriteGraphicsLog("DXR reflection root signature failed: %s\n", g_LastShaderError.c_str());
        return false;
    }
    auto compile = [&](const char* entry, ComPtr<ID3D12PipelineState>& pipeline) {
        return CompileRadiancePipeline(entry, pipeline);
    };
    // 必須パイプラインを先に解決する。任意の放射輝度キャッシュと ReSTIR は失敗時に自身だけを無効化する。
    // 以前はその失敗で Compose / ComposeDiffuse も省略され、TraceDiffuse が動いているのに
    // NRD 経路に null パイプラインがバインドされたままになっていた。
    compile("CaptureAdaptiveGuide", g_AdaptiveCapture);
    const bool required =
        compile("main", g_Trace) && compile("TraceDiffuse", g_GiTrace) && compile("FilterDiffuse", g_GiFilter) &&
        (!NrdDenoiser::IsCompiled() || (compile("Compose", g_Compose) && compile("ComposeDiffuse", g_GiCompose)));
    if (!required)
    {
        g_Trace.Reset();
        g_GiTrace.Reset();
        g_GiFilter.Reset();
        g_Compose.Reset();
        g_GiCompose.Reset();
        g_Status = "RayReflectionsCS のコンパイル失敗（反射 / Realtime GI 無効）。ログを確認してください";
        return false;
    }
    if (!compile("ResolveRadianceCache", g_CacheResolve) || !compile("ClearRadianceCache", g_CacheClear))
    {
        g_CacheResolve.Reset();
        g_CacheClear.Reset();
        g_Cache = {};
        WriteGraphicsLog("Realtime GI radiance cache unavailable; continuing without it\n");
    }
    if (!compile("ReSTIRTemporal", g_ReSTIRTemporal) || !compile("ReSTIRSpatial", g_ReSTIRSpatial))
    {
        g_ReSTIRTemporal.Reset();
        g_ReSTIRSpatial.Reset();
        WriteGraphicsLog("ReSTIR GI unavailable; continuing without it\n");
    }
    if (!compile("LumenBuildBounds", g_LumenBounds) || !compile("LumenCapture", g_LumenCapture) ||
        !compile("LumenClear", g_LumenClear) || !compile("LumenUpdateLighting", g_LumenLighting) ||
        !compile("LumenTraceProbes", g_LumenGather))
    {
        g_LumenBounds.Reset();
        WriteGraphicsLog("Lumen GI unavailable: %s\n", g_LastShaderError.c_str());
    }
    g_Status = "準備完了";
    return true;
}

const char* RayReflections::GetStatus()
{
    return g_Status;
}

const char* RayReflections::GetLastShaderError()
{
    return g_LastShaderError.c_str();
}

UINT RayReflections::GetLumenResidentMeshes()
{
    return g_Lumen.ResidentMeshes();
}

UINT64 RayReflections::GetLumenMemoryBytes()
{
    return g_Lumen.MemoryBytes();
}

ID3D12Resource* RayReflections::GetLumenLightingResource()
{
    return g_Lumen.LightingResource();
}

void RayReflections::Shutdown()
{
    g_Lumen.Reset();
    g_LumenBounds.Reset();
    g_LumenCapture.Reset();
    g_LumenClear.Reset();
    g_LumenLighting.Reset();
    g_LumenGather.Reset();
    g_LumenCaptureTelemetry.Reset();
    g_LumenLightingTelemetry.Reset();
    g_LumenGatherTelemetry.Reset();
    g_LumenWasEnabled = false;
    g_AdaptiveRadiance.Reset();
    g_AdaptiveGuide.Reset();
    g_AdaptiveCapture.Reset();
    g_AdaptiveValid = g_BudgetValid = false;

    g_TraceTelemetry.Reset();
    g_GiTraceTelemetry.Reset();
    g_ReSTIRSpatialTelemetry.Reset();
    g_TelemetryPipelinesTried = false;
    g_OptixGi.reset();
    g_OptixReflections.reset();
    g_OptixGiActive = g_OptixReflectionActive = false;
    g_GiDenoiser.reset();
    g_GiTrace.Reset();
    g_GiCompose.Reset();
    g_GiFilter.Reset();
    g_CacheResolve.Reset();
    g_Status = "未初期化";
    g_ReSTIRTemporal.Reset();
    g_ReSTIRSpatial.Reset();
    g_Reservoirs[0].Reset();
    g_Reservoirs[1].Reset();
    g_ReservoirWidth = g_ReservoirHeight = 0;
    g_ReSTIRHistoryValid = false;
    g_CacheClear.Reset();
    g_Cache = {};
    g_CacheWasEnabled = false;
    g_GiActive = g_GiHistoryValid = false;
    g_GiEmissionSignature = 0;
    g_GiEmissionStableFrames = kGiEmissionRestartQuietFrames;
    g_GiWidth = g_GiHeight = 0;
    g_Denoiser.reset();
    g_Frames = {};
    g_Shared = {};
    g_Root.Reset();
    g_Trace.Reset();
    g_Compose.Reset();
    g_Device.Reset();
    g_EngineHeap.Reset();
    g_Width = g_Height = g_Dispatches = 0;
    g_HistoryValid = false;
}

void RayReflections::ReleaseInactiveResources()
{
    if (!g_Device || !GpuMemoryBudget::Options.Enabled || !GpuMemoryBudget::Options.ReleaseInactive)
    {
        return;
    }
    const auto& settings = Raytracing::GetSettings();
    const bool reflection = settings.Enabled && settings.Reflections;
    const bool gi = settings.Enabled && RendererSettings::GetRealtimeGiEnabled();
    const bool lumen = gi && RendererSettings::GetLumenEnabled();
    const bool reservoir = gi && !lumen && RenderQuality::Get().ReSTIRGI;
    const bool cache = gi && (lumen || RendererSettings::GetGiRadianceCacheEnabled());
    const bool optix = settings.Denoise && settings.RadianceDenoiser == Raytracing::DenoiserType::Optix &&
                       OptixDenoiser::IsAvailable();
    const bool nrd = settings.Denoise && !optix;
    const bool releaseReflection = !reflection && (g_Shared.Noisy || g_Shared.Result);
    const bool releaseGi = !gi && (g_Shared.GiNoisy || g_Shared.GiResult);
    const bool releaseReservoir = !reservoir && g_Reservoirs[0];
    const bool releaseCache = !cache && g_Cache.Keys;
    const bool releaseLumen = !lumen && g_Lumen.MemoryBytes() != 0;
    const bool releaseDenoisers = ((!reflection || !nrd) && g_Denoiser) || ((!gi || !nrd) && g_GiDenoiser) ||
                                  ((!reflection || !optix) && g_OptixReflections) || ((!gi || !optix) && g_OptixGi);
    const bool releaseAdaptive = (!reflection || lumen) && g_AdaptiveRadiance;
    if (!(releaseReflection || releaseGi || releaseReservoir || releaseCache || releaseLumen || releaseDenoisers ||
          releaseAdaptive) ||
        !GraphicsDevice::WaitForGpuIdle())
    {
        return;
    }
    if (releaseReflection)
    {
        g_Shared.Noisy.Reset();
        g_Shared.Result.Reset();
        g_HistoryValid = false;
    }
    if (releaseGi)
    {
        g_Shared.GiNoisy.Reset();
        g_Shared.GiResult.Reset();
        g_GiActive = g_GiHistoryValid = false;
    }
    for (auto& frame : g_Frames)
    {
        if (!reflection)
        {
            frame.Noisy.Reset();
            frame.Result.Reset();
        }
        if (!gi)
        {
            frame.GiNoisy.Reset();
            frame.GiResult.Reset();
        }
        if (!reflection && !gi)
        {
            frame = {};
        }
    }
    if (releaseReservoir)
    {
        g_Reservoirs[0].Reset();
        g_Reservoirs[1].Reset();
        g_ReservoirWidth = g_ReservoirHeight = 0;
        g_ReSTIRHistoryValid = false;
    }
    if (releaseCache)
    {
        g_Cache = {};
        g_CacheWasEnabled = false;
    }
    if (releaseLumen)
    {
        g_Lumen.Reset();
        g_LumenWasEnabled = false;
    }
    if (releaseAdaptive)
    {
        g_AdaptiveRadiance.Reset();
        g_AdaptiveGuide.Reset();
        g_AdaptiveValid = false;
    }
    if (!reflection || !nrd)
    {
        g_Denoiser.reset();
        g_Width = g_Height = 0;
        g_HistoryValid = false;
    }
    if (!gi || !nrd)
    {
        g_GiDenoiser.reset();
        g_GiWidth = g_GiHeight = 0;
        g_GiHistoryValid = false;
    }
    if (!reflection || !optix)
    {
        g_OptixReflections.reset();
    }
    if (!gi || !optix)
    {
        g_OptixGi.reset();
    }
}

bool RayReflections::Execute(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene,
                             const vector<RaytracingGeometry>& geometry, const NrdFrame& nrdFrame,
                             const array<ComPtr<ID3D12Resource>, 4>& guides, bool denoise)
{
    RenderProfiler::ScopedEvent preparationProfile("DXR Radiance Setup", commands);
    g_Dispatches = 0;
    g_OptixGiActive = g_OptixReflectionActive = false;
    g_GiActive = false;
    if (!g_Trace || !g_GiTrace || !g_GiFilter)
    {
        g_Status = "パイプライン未作成（反射 / Realtime GI 無効）";
        return false;
    }
    if (!commands || !scene)
    {
        g_Status = "TLAS がありません";
        return false;
    }
    if (geometry.empty())
    {
        g_Status = "レイトレース対象のメッシュがありません";
        return false;
    }
    const UINT slot = nrdFrame.FrameIndex % kFrames;
    g_LastFrame = slot;
    auto* depth = RenderTargets::GetGBufferResource(GBufferType::DEPTH);
    auto* normal = RenderTargets::GetGBufferResource(GBufferType::NORMAL);
    auto* material = RenderTargets::GetGBufferResource(GBufferType::MATERIAL);
    if (!depth || !normal || !material)
    {
        return false;
    }
    const auto extent = depth->GetDesc();
    auto& frame = g_Frames[slot];
    if (!EnsureFrame(frame, slot, static_cast<UINT>(extent.Width), extent.Height))
    {
        g_Status = "GI / 反射のテクスチャ確保に失敗";
        return false;
    }
    g_Status = RendererSettings::GetRealtimeGiEnabled() ? "Realtime GI 準備中" : "反射のみ動作中";
    const bool instrumented = RayTelemetry::IsCollecting() && EnsureTelemetryPipelines();
    RayTelemetry::Data().RecordProvider(RayTelemetry::Radiance, instrumented);
    frame.Geometry = geometry;
    const UINT count = static_cast<UINT>((min)(geometry.size(), static_cast<size_t>(kMaximumMeshes)));
    vector<MeshConstants> meshes(count);
    vector<UINT> emitters;
    unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> sampledResources;
    sampledResources.reserve(count / 2 + 16);
    if (frame.Descriptors.size() < count)
    {
        frame.Descriptors.resize(count);
    }
    sampledResources.emplace(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    sampledResources.emplace(normal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    sampledResources.emplace(material, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* velocity = RenderTargets::GetGBufferResource(GBufferType::VELOCITY);
    if (velocity)
    {
        sampledResources.emplace(velocity, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    for (UINT i = 0; i < count; ++i)
    {
        const auto& geometry = frame.Geometry[i];
        auto& mesh = meshes[i];
        const XMMATRIX world = XMLoadFloat4x4(&geometry.World);
        XMStoreFloat4x4(&mesh.World, XMMatrixTranspose(world));
        XMStoreFloat4x4(&mesh.NormalToWorld, XMMatrixInverse(nullptr, world));
        UINT indexBitWidth = 0;
        if (geometry.Indices)
        {
            indexBitWidth = 32;
            if (geometry.IndexView.Format == DXGI_FORMAT_R16_UINT)
            {
                indexBitWidth = 16;
            }
        }
        mesh.Layout = {
            geometry.VertexView.StrideInBytes, indexBitWidth,
            static_cast<UINT>(geometry.VertexView.BufferLocation - geometry.Vertices->GetGPUVirtualAddress()),
            geometry.Indices
                ? static_cast<UINT>(geometry.IndexView.BufferLocation - geometry.Indices->GetGPUVirtualAddress())
                : 0u};
        // w = POM の深さ（UV 単位、0 で無効）。
        mesh.Material = {geometry.Roughness, geometry.Metallic, static_cast<float>(geometry.Texture != nullptr),
                         geometry.ParallaxDepth};
        mesh.Noise = geometry.Noise;
        mesh.NoiseSurface.x = geometry.NoiseNormalScale;
        mesh.BaseColor = geometry.BaseColor;
        mesh.Emission = geometry.Emission;
        UINT triangleCount = geometry.IndexCount / 3;
        if (!geometry.Indices)
        {
            triangleCount = geometry.VertexView.SizeInBytes / geometry.VertexView.StrideInBytes / 3;
        }
        mesh.Surface = {triangleCount, static_cast<UINT>(geometry.EmissionTexture != nullptr),
                        static_cast<UINT>(geometry.HeightTexture != nullptr || geometry.Noise[2].Width != 0),
                        static_cast<UINT>(geometry.UsesAssetMaterial)};
        if (geometry.Emission.w > 0 && (geometry.Emission.x > 0 || geometry.Emission.y > 0 || geometry.Emission.z > 0))
        {
            emitters.push_back(i);
        }
        auto& record = frame.Descriptors[i];
        const UINT emissionMip = static_cast<UINT>(geometry.EmissionResidentMip);
        const UINT textureMip = static_cast<UINT>(geometry.TextureResidentMip);
        if (!record.Valid || record.Emission.Get() != geometry.EmissionTexture.Get() ||
            record.EmissionMip != emissionMip)
        {
            TextureSrv(geometry.EmissionTexture.Get(), Cpu(frame.Heap.Get(), kEmissionDescriptors + i),
                       geometry.EmissionResidentMip);
        }
        if (geometry.EmissionTexture)
        {
            sampledResources.emplace(geometry.EmissionTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        const UINT heightMip = static_cast<UINT>(geometry.HeightResidentMip);
        if (!record.Valid || record.Height.Get() != geometry.HeightTexture.Get() || record.HeightMip != heightMip)
        {
            TextureSrv(geometry.HeightTexture.Get(), Cpu(frame.Heap.Get(), kHeightDescriptors + i),
                       geometry.HeightResidentMip);
        }
        if (geometry.HeightTexture)
        {
            sampledResources.emplace(geometry.HeightTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if (!record.Valid || record.Vertices.Get() != geometry.Vertices.Get())
        {
            BufferSrv(geometry.Vertices.Get(), Cpu(frame.Heap.Get(), kVertexDescriptors + i));
        }
        if (!record.Valid || record.Indices.Get() != geometry.Indices.Get())
        {
            BufferSrv(geometry.Indices.Get(), Cpu(frame.Heap.Get(), kIndexDescriptors + i));
        }
        if (!record.Valid || record.Texture.Get() != geometry.Texture.Get() || record.TextureMip != textureMip)
        {
            if (geometry.Texture)
            {
                TextureSrv(geometry.Texture.Get(), Cpu(frame.Heap.Get(), kTextureDescriptors + i),
                           geometry.TextureResidentMip);
            }
            else
            {
                TextureSrv(nullptr, Cpu(frame.Heap.Get(), kTextureDescriptors + i));
            }
        }
        if (geometry.Texture)
        {
            sampledResources.emplace(geometry.Texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        record.Vertices = geometry.Vertices;
        record.Indices = geometry.Indices;
        record.Texture = geometry.Texture;
        record.Emission = geometry.EmissionTexture;
        record.Height = geometry.HeightTexture;
        record.HeightMip = heightMip;
        record.TextureMip = textureMip;
        record.EmissionMip = emissionMip;
        record.Valid = true;
        sampledResources.emplace(geometry.Vertices.Get(), geometry.VertexState);
        if (geometry.Indices)
        {
            sampledResources.emplace(geometry.Indices.Get(), geometry.IndexState);
        }
    }
    if (!EnsureUpload(frame.Meshes, meshes.size() * sizeof(MeshConstants)) ||
        !Upload(frame.Meshes.Get(), meshes.data(), meshes.size() * sizeof(MeshConstants)))
    {
        return false;
    }
    if (!EnsureUpload(frame.Emitters, (max)(size_t(4), emitters.size() * sizeof(UINT))) ||
        (!emitters.empty() && !Upload(frame.Emitters.Get(), emitters.data(), emitters.size() * sizeof(UINT))))
    {
        return false;
    }
    FrameConstants constants;
    vector<ReflectionLight> lights;
    lights.reserve(RendererState::g_kMAX_SHADER_LIGHTS);
    // 負荷削減：反射・GI のヒット点で評価するライトは上限 160 灯。以前は ECS の並び順で先頭から取っていたので、
    // ライトが多いシーンでは遠いライトが枠を使い切っていた。カメラから近い（影響範囲の表面までの距離が短い）順に選ぶ
    static vector<pair<float, EntityID>> reflectionCandidates;
    reflectionCandidates.clear();
    {
        const XMVECTOR cameraPosition = XMMatrixInverse(nullptr, XMLoadFloat4x4(&nrdFrame.View)).r[3];
        for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
        {
            const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
            if (!light.IsActive || !light.AffectsOpaque || light.RenderMode != LightRenderMode::Physical ||
                light.Intensity <= 0)
            {
                continue;
            }
            float key = -1.0f;
            if (light.Type != LightType::Directional)
            {
                const auto& position = ComponentManager::GetComponentUnchecked<TransformComponent>(entity).Position;
                key = (std::max)(XMVectorGetX(
                                     XMVector3Length(XMVectorSubtract(XMLoadFloat3(&position), cameraPosition))) -
                                     LightInfluenceRadius(light),
                                 0.0f);
            }
            reflectionCandidates.emplace_back(key, entity);
        }
        // 負荷削減：ヒット点ではライトをループで全評価するので、灯数がそのまま反射 / GI のコストになる
        const size_t hitLights = static_cast<size_t>(std::clamp(RenderCulling::GetSettings().RayHitLights, 1,
                                                                static_cast<int>(RendererState::g_kMAX_SHADER_LIGHTS)));
        if (reflectionCandidates.size() > hitLights)
        {
            nth_element(reflectionCandidates.begin(), reflectionCandidates.begin() + hitLights,
                        reflectionCandidates.end());
            reflectionCandidates.resize(hitLights);
        }
    }
    for (const auto& [distanceKey, entity] : reflectionCandidates)
    {
        const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        XMFLOAT3 direction;
        XMStoreFloat3(&direction, XMVector3Normalize(XMLoadFloat3(&light.Direction)));
        ReflectionLight entry;
        entry.Direction = {direction.x, direction.y, direction.z, light.Range};
        entry.Color = {light.Color.x, light.Color.y, light.Color.z, light.Intensity * 1.35f};
        entry.PositionType = {transform.Position.x, transform.Position.y, transform.Position.z,
                              static_cast<float>(light.Type)};
        entry.Extras = PackLightExtra(light);
        entry.Flags.x = static_cast<UINT>(light.CastShadow && light.Type != LightType::Capsule);
        lights.push_back(entry);
    }
    if (!EnsureUpload(frame.Lights, (max)(sizeof(ReflectionLight), lights.size() * sizeof(ReflectionLight))) ||
        (!lights.empty() && !Upload(frame.Lights.Get(), lights.data(), lights.size() * sizeof(ReflectionLight))))
    {
        return false;
    }
    const XMMATRIX view = XMLoadFloat4x4(&nrdFrame.View);
    XMMATRIX projection = XMLoadFloat4x4(&nrdFrame.Projection);
    // このフレームの GBuffer が使ったジッターを復元する。
    XMFLOAT4X4 jittered;
    XMStoreFloat4x4(&jittered, projection);
    jittered._31 = -nrdFrame.Jitter.x * 2 / frame.Width;
    jittered._32 = nrdFrame.Jitter.y * 2 / frame.Height;
    projection = XMLoadFloat4x4(&jittered);
    XMStoreFloat4x4(&constants.InvViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, view * projection)));
    XMStoreFloat4x4(&constants.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&constants.ViewProjection, XMMatrixTranspose(view * projection));
    XMStoreFloat4(&constants.Camera, XMMatrixInverse(nullptr, view).r[3]);
    constants.Camera.w = nrdFrame.Range;
    constants.Extent = {frame.Width, frame.Height, nrdFrame.Sequence, static_cast<UINT>(lights.size())};
    const float inputScale = float(frame.Width) / float((std::max)(1u, GraphicsDevice::GetWidth()));
    constants.Stability = {Raytracing::GetSettings().StableSampling ? 1.f : 0.f, inputScale, 0, 0};
    // 動きのガイドでは、静止メッシュの発光輝度の変化を検出できない。
    // 発光マテリアルや発光体の変換が変わったら、この信号だけを再開する。
    uint64_t emissionSignature = 14695981039346656037ull;
    for (UINT emitter : emitters)
    {
        const auto& source = frame.Geometry[emitter];
        HashEmission(emissionSignature, &emitter, sizeof(emitter));
        HashEmission(emissionSignature, &source.Emission, sizeof(source.Emission));
        HashEmission(emissionSignature, &source.World, sizeof(source.World));
        const auto texture = reinterpret_cast<uintptr_t>(source.EmissionTexture.Get());
        HashEmission(emissionSignature, &texture, sizeof(texture));
    }
    const float giDistance = RendererSettings::GetGiDistance();
    HashEmission(emissionSignature, &giDistance, sizeof(giDistance));
    if (emissionSignature != g_GiEmissionSignature)
    {
        // 切り替え、新規マテリアル、瞬間移動など離散的な変更では履歴を1回リセットする。
        // 連続アニメーションでは毎フレームシグネチャが変わる。毎回リセットすると REBLUR が常に
        // 0.5 rpp のままでちらつくため、その場合は NRD のアンチラグに任せる。
        if (g_GiEmissionStableFrames >= kGiEmissionRestartQuietFrames)
        {
            g_GiHistoryValid = false;
        }
        g_GiEmissionSignature = emissionSignature;
        g_GiEmissionStableFrames = 0;
    }
    else if (g_GiEmissionStableFrames < kGiEmissionRestartQuietFrames)
    {
        ++g_GiEmissionStableFrames;
    }
    const bool useOptix = denoise && Raytracing::GetSettings().RadianceDenoiser == Raytracing::DenoiserType::Optix &&
                          OptixDenoiser::IsAvailable();
    const bool useGiDenoiser = denoise && !useOptix && NrdDenoiser::IsCompiled() && guides[0] && guides[1] && guides[2];
    constants.GiParameters = {RendererSettings::GetGiDistance(), RendererSettings::GetRealtimeGiIntensity(),
                              static_cast<float>(useGiDenoiser), static_cast<float>(emitters.size())};
    const bool lumenRequested = RendererSettings::GetLumenEnabled();
    // Lumen のワールド空間放射輝度キャッシュは常にハッシュグリッドを使う（キー空間をソルトで分離）。
    const bool cacheEnabled = RendererSettings::GetRealtimeGiEnabled() &&
                              (lumenRequested || RendererSettings::GetGiRadianceCacheEnabled()) &&
                              (g_Cache.Keys || CreateRadianceCache()) && g_CacheResolve && g_CacheClear;
    if (cacheEnabled && !g_CacheWasEnabled)
    {
        g_Cache.NeedsClear = true;
    }
    g_CacheWasEnabled = cacheEnabled;
    const auto& quality = RenderQuality::Get();
    const UINT reservoirWidth = useGiDenoiser ? (frame.Width + 1) / 2 : frame.Width;
    const bool restir = RendererSettings::GetRealtimeGiEnabled() && !lumenRequested && quality.ReSTIRGI && velocity &&
                        g_ReSTIRTemporal && g_ReSTIRSpatial && EnsureReservoirs(reservoirWidth, frame.Height);
    if (!restir || nrdFrame.ResetHistory)
    {
        g_ReSTIRHistoryValid = restir && g_ReSTIRHistoryValid && !nrdFrame.ResetHistory;
    }
    constants.ReSTIRParameters = {restir ? 1.0f : 0.0f,
                                  static_cast<float>(std::clamp(quality.ReSTIRTemporalMaxM, 1, 64)),
                                  static_cast<float>(std::clamp(quality.ReSTIRSpatialSamples, 0, 16)),
                                  std::clamp(quality.ReSTIRSpatialRadius, 1.0f, 64.0f)};
    // 反射の半解像度（チェッカーボード）：NRD で復元できるときだけ
    const bool reflectionCheckerboard = RenderCulling::GetSettings().ReflectionCheckerboard && !useOptix && denoise &&
                                        NrdDenoiser::IsCompiled() && guides[0] && guides[1] && guides[2];
    g_ReflectionCheckerboard = reflectionCheckerboard;
    g_ReflectionSampling = {frame.Width, frame.Height, nrdFrame.Sequence, reflectionCheckerboard};
    constants.ReSTIRState = {
        g_ReSTIRHistoryValid ? 1u : 0u, reservoirWidth, quality.ReSTIRSpatialVisibility ? 1u : 0u,
        static_cast<UINT>(std::clamp(RenderCulling::GetSettings().ReflectionRoughnessCutoff, 0.0f, 1.0f) * 1000.0f +
                          0.5f) |
            (reflectionCheckerboard ? 0x10000u : 0u)};
    constants.CacheParameters = {cacheEnabled ? 1.0f : 0.0f, RendererSettings::GetGiRadianceCacheCellSize(),
                                 static_cast<float>(kCacheCapacity),
                                 static_cast<float>(RendererSettings::GetGiRadianceCacheHistory())};
    static const bool adaptiveRequested = ReadEnvironmentFlagOnce("DX12_ADAPTIVE_REFLECTIONS");
    static const bool budgetRequested = ReadEnvironmentFlagOnce("DX12_GI_UPDATE_BUDGET");
    const bool adaptive = adaptiveRequested && !lumenRequested && velocity && g_AdaptiveCapture &&
                          Raytracing::GetSettings().Reflections && EnsureAdaptiveHistory(frame.Width, frame.Height);
    uint64_t signature = 1469598103934665603ull;
    for (const auto& entry : geometry)
    {
        HashEmission(signature, &entry.World, sizeof(entry.World));
        HashEmission(signature, &entry.BaseColor, sizeof(entry.BaseColor));
        HashEmission(signature, &entry.Emission, sizeof(entry.Emission));
        HashEmission(signature, &entry.Roughness, sizeof(entry.Roughness));
        HashEmission(signature, &entry.Metallic, sizeof(entry.Metallic));
        HashEmission(signature, &entry.VertexView, sizeof(entry.VertexView));
        HashEmission(signature, &entry.IndexView, sizeof(entry.IndexView));
        const auto texture = entry.Texture.Get();
        const auto emission = entry.EmissionTexture.Get();
        HashEmission(signature, &texture, sizeof(texture));
        HashEmission(signature, &emission, sizeof(emission));
        HashEmission(signature, &entry.TextureResidentMip, sizeof(entry.TextureResidentMip));
        const auto height = entry.HeightTexture.Get();
        HashEmission(signature, &height, sizeof(height));
        HashEmission(signature, &entry.ParallaxDepth, sizeof(entry.ParallaxDepth));
        HashEmission(signature, entry.Noise.data(), sizeof(entry.Noise));
        HashEmission(signature, &entry.NoiseNormalScale, sizeof(entry.NoiseNormalScale));
        if (entry.Deforming)
        {
            HashEmission(signature, &nrdFrame.Sequence, sizeof(nrdFrame.Sequence));
        }
    }
    if (!lights.empty())
    {
        HashEmission(signature, lights.data(), lights.size() * sizeof(ReflectionLight));
    }
    HashEmission(signature, &nrdFrame.LightDirection, sizeof(nrdFrame.LightDirection));
    HashEmission(signature, &giDistance, sizeof(giDistance));
    const auto& atmosphere = Atmosphere::GetParameters();
    HashEmission(signature, &atmosphere, sizeof(atmosphere));
    const auto& skySettings = AtmosphereLut::GetSettings();
    HashEmission(signature, &skySettings, sizeof(skySettings));
    const bool lumenChanged = signature != g_LumenSignature;
    const bool lumenDiscreteChange = Lumen::LightingChangeRequiresReset(lumenChanged, g_LumenQuietFrames);
    g_LumenQuietFrames = lumenChanged ? 0 : (std::min)(g_LumenQuietFrames + 1, 8u);
    const UINT hardResetReasons = TemporalHistory::FirstFrame | TemporalHistory::FrameGap |
                                  TemporalHistory::CameraChanged | TemporalHistory::ResolutionChanged |
                                  TemporalHistory::ConfigurationChanged;
    const bool lumenReset = !g_LumenWasEnabled || lumenDiscreteChange ||
                            (nrdFrame.HistoryResetReasons & hardResetReasons) != 0 ||
                            nrdFrame.Sequence != g_LumenSequence + 1;
    const XMFLOAT3 lumenCamera{constants.Camera.x, constants.Camera.y, constants.Camera.z};
    if (lumenRequested && (lumenChanged || !g_LumenWasEnabled))
    {
        g_Lumen.NotifySceneChanged(nrdFrame.Sequence);
    }
    const bool lumen =
        lumenRequested && g_LumenBounds &&
        g_Lumen.Prepare(g_Device.Get(), slot, frame.Geometry, lumenCamera, frame.Width, frame.Height,
                        RendererSettings::GetLumenProbeSpacing(), RendererSettings::GetLumenCaptureBudget(),
                        RendererSettings::GetLumenLightingBudget(), RendererSettings::GetLumenDebugView(), lumenReset,
                        nrdFrame.Sequence, g_LumenQuietFrames < 8);
    if (lumenRequested && !lumen)
    {
        g_Status = "Lumen GI の初期化失敗。既存 DXR GI を使用中";
    }
    if (g_LumenWasEnabled != lumen || (lumen && lumenReset))
    {
        // キャッシュ済みのヒット放射輝度は、以前の照明とモードに対応する。
        g_Cache.NeedsClear = true;
        g_GiHistoryValid = false;
        g_HistoryValid = false;
    }
    if (lumen)
    {
        // ワールド放射輝度キャッシュ：物体やライトが動く間は短い履歴、静止時は長い履歴を使う。
        const float limit = g_LumenQuietFrames < 8 ? 4.0f : 16.0f;
        constants.CacheParameters.w = (std::min)(constants.CacheParameters.w, limit);
    }
    g_LumenWasEnabled = lumen;
    g_LumenSignature = signature;
    g_LumenSequence = nrdFrame.Sequence;
    if (!frame.DisabledLumen)
    {
        const LumenSurfaceCache::Constants disabled{};
        if (!EnsureUpload(frame.DisabledLumen, 256) || !Upload(frame.DisabledLumen.Get(), &disabled, sizeof(disabled)))
        {
            return false;
        }
    }
    g_Lumen.WriteDescriptors(g_Device.Get(), slot, Cpu(frame.Heap.Get(), kLumenDescriptors), g_Increment);
    const bool reuse = adaptive && g_AdaptiveValid && !nrdFrame.ResetHistory && g_AdaptiveSignature == signature &&
                       nrdFrame.Sequence == g_AdaptiveSequence + 1u;
    const bool budget = budgetRequested && cacheEnabled && !restir && velocity && g_BudgetValid &&
                        !nrdFrame.ResetHistory && g_BudgetSignature == signature &&
                        nrdFrame.Sequence == g_BudgetSequence + 1u;
    if (budgetRequested && cacheEnabled && (!g_BudgetValid || g_BudgetSignature != signature || nrdFrame.ResetHistory))
    {
        g_Cache.NeedsClear = true;
    }
    constants.AdaptiveSampling = {adaptive ? 1.f : 0.f, reuse ? 1.f : 0.f, .2f, budget ? 4.f : 1.f};
    g_BudgetSignature = signature;
    g_BudgetSequence = nrdFrame.Sequence;
    g_BudgetValid = budgetRequested && cacheEnabled && !restir;
    XMFLOAT4X4 previousProjection = nrdFrame.PreviousProjection;
    SceneDepth::SetProjectionJitter(previousProjection,
        {-nrdFrame.PreviousJitter.x * 2 / frame.Width, nrdFrame.PreviousJitter.y * 2 / frame.Height});
    XMStoreFloat4x4(&constants.AdaptivePreviousViewProjection,
                    XMMatrixTranspose(XMLoadFloat4x4(&nrdFrame.PreviousView) * XMLoadFloat4x4(&previousProjection)));
    TextureSrv(adaptive ? g_AdaptiveRadiance.Get() : nullptr, Cpu(frame.Heap.Get(), 14));
    TextureSrv(adaptive ? g_AdaptiveGuide.Get() : nullptr, Cpu(frame.Heap.Get(), 15));
    D3D12_UNORDERED_ACCESS_VIEW_DESC guide{};
    guide.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    guide.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_Device->CreateUnorderedAccessView(adaptive ? g_AdaptiveGuide.Get() : nullptr, nullptr, &guide,
                                        Cpu(frame.Heap.Get(), 23));
    if (!Upload(frame.Constants.Get(), &constants, sizeof(constants)))
    {
        return false;
    }
    for (auto [resource, state] : sampledResources)
    {
        if ((state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0)
        {
            Transition(commands, resource, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    for (UINT i = 0; i < 3; ++i)
    {
        constexpr GBufferType guideTypes[] = {GBufferType::DEPTH, GBufferType::NORMAL, GBufferType::MATERIAL};
        const auto type = guideTypes[i];
        TextureSrv(RenderTargets::GetGBufferResource(type), Cpu(frame.Heap.Get(), i));
        TextureSrv(RenderTargets::GetGBufferResource(type), Cpu(frame.Heap.Get(), 8 + i));
    }
    TextureSrv(nullptr, Cpu(frame.Heap.Get(), 3));
    AtmosphereLut::WriteSrvs(g_Device.Get(), Cpu(frame.Heap.Get(), kSkyLutDescriptors),
                             Cpu(frame.Heap.Get(), kSkyLutDescriptors + 1));
    WriteCacheDescriptors(frame.Heap.Get());
    if (restir)
    {
        const auto rawUav = [&](ID3D12Resource* buffer, UINT index) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.NumElements = static_cast<UINT>(buffer->GetDesc().Width / 4);
            desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            g_Device->CreateUnorderedAccessView(buffer, nullptr, &desc, Cpu(frame.Heap.Get(), index));
        };
        rawUav(g_Reservoirs[g_ReservoirCurrent].Get(), kReSTIRDescriptors);
        rawUav(g_Reservoirs[g_ReservoirCurrent ^ 1u].Get(), kReSTIRDescriptors + 1);
        TextureSrv(velocity, Cpu(frame.Heap.Get(), kReSTIRDescriptors + 2));
    }
    else
    {
        // 毎回バインドするテーブルなので、有効な null 記述子を用意する。
        for (UINT index = 0; index < 2; ++index)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.NumElements = 1;
            desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            g_Device->CreateUnorderedAccessView(nullptr, nullptr, &desc,
                                                Cpu(frame.Heap.Get(), kReSTIRDescriptors + index));
        }
        TextureSrv(nullptr, Cpu(frame.Heap.Get(), kReSTIRDescriptors + 2));
    }
    TextureSrv(velocity, Cpu(frame.Heap.Get(), 22));
    if (adaptive)
    {
        Transition(commands, g_AdaptiveRadiance.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commands, g_AdaptiveGuide.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    auto bind = [&](ID3D12PipelineState* pipeline) {
        ID3D12DescriptorHeap* heaps[] = {frame.Heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(g_Root.Get());
        NoiseCacheBindings::BindCompute(commands, NoiseCacheBindings::ReflectionRoot);
        commands->SetPipelineState(pipeline);
        commands->SetComputeRootShaderResourceView(0, scene->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(1, frame.Meshes->GetGPUVirtualAddress());
        commands->SetComputeRootConstantBufferView(2, frame.Constants->GetGPUVirtualAddress());
        commands->SetComputeRootConstantBufferView(3, LightingResources::GetCurrentLightConstantBufferAddress());
        commands->SetComputeRootConstantBufferView(4, Raytracing::GetLightingConstantsAddress(slot));
        commands->SetComputeRootDescriptorTable(5, Gpu(frame.Heap.Get(), 0));
        commands->SetComputeRootDescriptorTable(6, Gpu(frame.Heap.Get(), 4));
        commands->SetComputeRootDescriptorTable(7, Gpu(frame.Heap.Get(), kVertexDescriptors));
        commands->SetComputeRootDescriptorTable(8, Gpu(frame.Heap.Get(), kIndexDescriptors));
        commands->SetComputeRootDescriptorTable(9, Gpu(frame.Heap.Get(), kTextureDescriptors));
        commands->SetComputeRootShaderResourceView(10, frame.Lights->GetGPUVirtualAddress());
        commands->SetComputeRootDescriptorTable(11, Gpu(frame.Heap.Get(), kEmissionDescriptors));
        commands->SetComputeRootDescriptorTable(21, Gpu(frame.Heap.Get(), kHeightDescriptors));
        commands->SetComputeRootShaderResourceView(12, frame.Emitters->GetGPUVirtualAddress());
        commands->SetComputeRootDescriptorTable(13, Gpu(frame.Heap.Get(), kSkyLutDescriptors));
        commands->SetComputeRootDescriptorTable(14, Gpu(frame.Heap.Get(), kCacheDescriptors));
        commands->SetComputeRootDescriptorTable(15, Gpu(frame.Heap.Get(), kReSTIRDescriptors));
        commands->SetComputeRootDescriptorTable(17, Gpu(frame.Heap.Get(), 14));
        commands->SetComputeRootDescriptorTable(18, Gpu(frame.Heap.Get(), 23));
        commands->SetComputeRootDescriptorTable(19, Gpu(frame.Heap.Get(), kLumenDescriptors));
        commands->SetComputeRootConstantBufferView(20, lumen ? g_Lumen.ConstantsAddress(slot)
                                                             : frame.DisabledLumen->GetGPUVirtualAddress());
        if (instrumented)
        {
            commands->SetComputeRootUnorderedAccessView(16, RayTelemetry::Data().Address());
        }
    };
    preparationProfile.Finish();
    if (lumen)
    {
        RenderProfiler::ScopedEvent surfaceProfile("Lumen Surface Cache", commands);
        if (cacheEnabled && g_Cache.NeedsClear)
        {
            // サーフェスキャッシュやプローブレイがワールド放射輝度キャッシュへ書く前にクリアする。
            bind(g_CacheClear.Get());
            commands->Dispatch(kCacheCapacity / 64, 1, 1);
            CacheBarrier(commands);
            g_Cache.NeedsClear = false;
        }
        if (g_Lumen.NeedsLightingClear())
        {
            bind(g_LumenClear.Get());
            commands->Dispatch(LumenSurfaceCache::AtlasWidth / 8, LumenSurfaceCache::AtlasHeight / 8, 1);
            g_Lumen.Barrier(commands);
        }
        const auto& parameters = g_Lumen.GetConstants();
        if (parameters.Counts.y != 0)
        {
            RenderProfiler::ScopedEvent captureProfile("Lumen Material Capture", commands);
            bind(g_LumenBounds.Get());
            commands->Dispatch(parameters.Counts.y, 1, 1);
            g_Lumen.Barrier(commands);
            bind(instrumented ? g_LumenCaptureTelemetry.Get() : g_LumenCapture.Get());
            commands->Dispatch(2, 2, parameters.Counts.y * 6);
            g_Lumen.Barrier(commands);
        }
        g_Lumen.BeginLighting(commands);
        if (parameters.Counts.z != 0)
        {
            RenderProfiler::ScopedEvent lightingProfile("Lumen Lighting Update", commands);
            bind(instrumented ? g_LumenLightingTelemetry.Get() : g_LumenLighting.Get());
            commands->Dispatch(2, 2, parameters.Counts.z * 6);
        }
        g_Lumen.EndLighting(commands);
        surfaceProfile.Finish();
        RenderProfiler::ScopedEvent probeProfile("Lumen Screen Probes", commands);
        bind(instrumented ? g_LumenGatherTelemetry.Get() : g_LumenGather.Get());
        // 64レーンのグループごとに8プローブ（LumenTraceProbes を参照）。
        commands->Dispatch((parameters.Grid.x * parameters.Grid.y + 7) / 8, 1, 1);
        g_Lumen.Barrier(commands);
    }
    if (Raytracing::GetSettings().Reflections)
    {
        RenderProfiler::ScopedEvent reflectionProfile("DXR Reflections", commands);
        RenderProfiler::ScopedEvent traceProfile("DXR Reflections Trace", commands);
        Transition(commands, frame.Noisy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bind(instrumented ? g_TraceTelemetry.Get() : g_Trace.Get());
        commands->Dispatch(((g_ReflectionCheckerboard ? (frame.Width + 1) / 2 : frame.Width) + 7) / 8,
                           (frame.Height + 7) / 8, 1);
        Transition(commands, frame.Noisy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        traceProfile.Finish();
        // NRD は独自ヒープを所有し、これら4入力の状態を復元する。
        if (useOptix)
        {
            if (!g_OptixReflections)
            {
                g_OptixReflections = make_unique<OptixDenoiser>();
            }
            g_OptixReflectionActive =
                g_OptixReflections->Execute(commands, frame.Noisy.Get(), depth, normal, frame.Result.Get(), nrdFrame,
                                            NrdDenoiser::IsCompiled(), true, 1.0f);
            g_HistoryValid = false;
        }
        else if (denoise && NrdDenoiser::IsCompiled() && guides[0] && guides[1] && guides[2])
        {
            if (!g_Denoiser || g_Width != frame.Width || g_Height != frame.Height ||
                g_DenoiserCheckerboard != g_ReflectionCheckerboard)
            {
                if (GraphicsDevice::WaitForGpuIdle())
                {
                    auto created = make_unique<NrdDenoiser>();
                    if (created->Initialize(g_Device.Get(),
                                            g_ReflectionCheckerboard ? NrdSignal::SpecularCheckerboard
                                                                     : NrdSignal::Specular,
                                            frame.Width, frame.Height))
                    {
                        g_Denoiser = move(created);
                        g_Width = frame.Width;
                        g_Height = frame.Height;
                        g_HistoryValid = false;
                        g_DenoiserCheckerboard = g_ReflectionCheckerboard;
                    }
                }
            }
            NrdFrame temporalFrame = nrdFrame;
            temporalFrame.ResetHistory = temporalFrame.ResetHistory || !g_HistoryValid;
            NrdInputs inputs{{guides[0].Get()}, {guides[1].Get()}, {guides[2].Get()}, {frame.Noisy.Get()}};
            bool filtered = false;
            {
                RenderProfiler::ScopedEvent denoiseProfile("DXR Reflections Denoise", commands);
                filtered = g_Denoiser && g_Width == frame.Width && g_Height == frame.Height &&
                           g_Denoiser->Execute(commands, inputs, temporalFrame);
            }
            if (filtered)
            {
                RenderProfiler::ScopedEvent composeProfile("DXR Reflections Compose", commands);
                TextureSrv(g_Denoiser->GetOutput(), Cpu(frame.Heap.Get(), 3));
                Transition(commands, g_Denoiser->GetOutput(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                bind(g_Compose.Get());
                commands->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
                Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Transition(commands, g_Denoiser->GetOutput(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                g_Dispatches = g_Denoiser->GetDispatchCount();
                g_HistoryValid = true;
            }
            else
            {
                g_HistoryValid = false;
            }
        }
        else
        {
            g_HistoryValid = false;
        }
    }
    if (RendererSettings::GetRealtimeGiEnabled())
    {
        RenderProfiler::ScopedEvent profile("Realtime GI", commands);
        RenderProfiler::ScopedEvent traceProfile("Realtime GI Trace", commands);
        Transition(commands, frame.GiNoisy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(commands, frame.GiResult.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (cacheEnabled && g_Cache.NeedsClear)
        {
            bind(g_CacheClear.Get());
            commands->Dispatch(kCacheCapacity / 64, 1, 1);
            CacheBarrier(commands);
            g_Cache.NeedsClear = false;
        }
        bind(instrumented ? g_GiTraceTelemetry.Get() : g_GiTrace.Get());
        commands->SetComputeRootDescriptorTable(5, Gpu(frame.Heap.Get(), 8));
        commands->SetComputeRootDescriptorTable(6, Gpu(frame.Heap.Get(), 6));
        UINT traceWidth = frame.Width;
        if (useGiDenoiser)
        {
            traceWidth = (traceWidth + 1) / 2;
        }
        commands->Dispatch((traceWidth + 7) / 8, (frame.Height + 7) / 8, 1);
        if (restir)
        {
            // バウンスのサンプルを時間方向、次に空間方向へ再サンプリングする（ReSTIR GI）。
            const D3D12_RESOURCE_BARRIER uav[] = {CD3DX12_RESOURCE_BARRIER::UAV(g_Reservoirs[g_ReservoirCurrent].Get()),
                                                  CD3DX12_RESOURCE_BARRIER::UAV(frame.GiNoisy.Get())};
            commands->ResourceBarrier(2, uav);
            commands->SetPipelineState(g_ReSTIRTemporal.Get());
            commands->Dispatch((reservoirWidth + 7) / 8, (frame.Height + 7) / 8, 1);
            commands->ResourceBarrier(2, uav);
            commands->SetPipelineState(instrumented ? g_ReSTIRSpatialTelemetry.Get() : g_ReSTIRSpatial.Get());
            commands->Dispatch((reservoirWidth + 7) / 8, (frame.Height + 7) / 8, 1);
            commands->ResourceBarrier(2, uav);
            // このフレームの時間リザーバーを次のフレームの履歴にする。
            g_ReservoirCurrent ^= 1u;
            g_ReSTIRHistoryValid = true;
        }
        if (cacheEnabled)
        {
            // このフレームのサンプルをセルへ平均化し、長く未使用のセルを失効させる。
            CacheBarrier(commands);
            commands->SetPipelineState(g_CacheResolve.Get());
            commands->Dispatch(kCacheCapacity / 64, 1, 1);
            CacheBarrier(commands);
        }
        Transition(commands, frame.GiNoisy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commands, frame.GiResult.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        traceProfile.Finish();
        bool filtered = false;
        bool temporalFiltered = false;
        if (useOptix)
        {
            if (!g_OptixGi)
            {
                g_OptixGi = make_unique<OptixDenoiser>();
            }
            g_OptixGiActive =
                g_OptixGi->Execute(commands, frame.GiNoisy.Get(), depth, normal, frame.GiResult.Get(), nrdFrame,
                                   NrdDenoiser::IsCompiled(), false, RendererSettings::GetRealtimeGiIntensity());
            filtered = g_OptixGiActive;
        }
        if (useGiDenoiser)
        {
            if (!g_GiDenoiser || g_GiWidth != frame.Width || g_GiHeight != frame.Height)
            {
                // 先行フレームが参照するリソースは、そのフェンス完了後だけリサイズで破棄する。
                if (!GraphicsDevice::WaitForGpuIdle())
                {
                    return false;
                }
                auto created = make_unique<NrdDenoiser>();
                if (created->Initialize(g_Device.Get(), NrdSignal::Diffuse, frame.Width, frame.Height))
                {
                    g_GiDenoiser = move(created);
                    g_GiWidth = frame.Width;
                    g_GiHeight = frame.Height;
                    g_GiHistoryValid = false;
                }
            }
            NrdFrame temporalFrame = nrdFrame;
            temporalFrame.ResetHistory |= !g_GiHistoryValid;
            NrdInputs inputs{{guides[0].Get()}, {guides[1].Get()}, {guides[2].Get()}, {frame.GiNoisy.Get()}};
            bool nrdFiltered = false;
            {
                RenderProfiler::ScopedEvent denoiseProfile("Realtime GI Denoise", commands);
                nrdFiltered = g_GiDenoiser && g_GiWidth == frame.Width && g_GiHeight == frame.Height &&
                              g_GiDenoiser->Execute(commands, inputs, temporalFrame);
            }
            if (nrdFiltered)
            {
                RenderProfiler::ScopedEvent composeProfile("Realtime GI Compose", commands);
                TextureSrv(g_GiDenoiser->GetOutput(), Cpu(frame.Heap.Get(), 11));
                Transition(commands, g_GiDenoiser->GetOutput(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Transition(commands, frame.GiResult.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                bind(g_GiCompose.Get());
                commands->SetComputeRootDescriptorTable(5, Gpu(frame.Heap.Get(), 8));
                commands->SetComputeRootDescriptorTable(6, Gpu(frame.Heap.Get(), 6));
                commands->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
                Transition(commands, frame.GiResult.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Transition(commands, g_GiDenoiser->GetOutput(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                g_Dispatches += g_GiDenoiser->GetDispatchCount();
                filtered = true;
                temporalFiltered = true;
            }
        }
        if (!filtered)
        {
            // 空間フォールバックはフィルターと合成を1回のディスパッチで行う。
            RenderProfiler::ScopedEvent spatialProfile("Realtime GI Spatial Denoise / Compose", commands);
            TextureSrv(frame.GiNoisy.Get(), Cpu(frame.Heap.Get(), 11));
            Transition(commands, frame.GiNoisy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(commands, frame.GiResult.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            bind(g_GiFilter.Get());
            commands->SetComputeRootDescriptorTable(5, Gpu(frame.Heap.Get(), 8));
            commands->SetComputeRootDescriptorTable(6, Gpu(frame.Heap.Get(), 6));
            commands->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
            Transition(commands, frame.GiResult.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(commands, frame.GiNoisy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        g_GiHistoryValid = temporalFiltered;
        g_GiActive = true;
        g_Status = lumen            ? "Lumen GI 動作中（Surface Cache / Screen Probes）"
                   : lumenRequested ? "Lumen 初期化失敗。既存 DXR GI を使用中"
                   : restir         ? "Realtime GI 動作中（ReSTIR）"
                                    : "Realtime GI 動作中";
    }
    else
    {
        g_GiHistoryValid = false;
    }
    if (adaptive)
    {
        Transition(commands, g_AdaptiveGuide.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bind(g_AdaptiveCapture.Get());
        commands->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
        Transition(commands, g_AdaptiveGuide.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commands, g_AdaptiveRadiance.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyResource(g_AdaptiveRadiance.Get(), frame.Result.Get());
        Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commands, g_AdaptiveRadiance.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_AdaptiveSignature = signature;
        g_AdaptiveSequence = nrdFrame.Sequence;
        g_AdaptiveValid = true;
    }
    else
    {
        g_AdaptiveValid = false;
    }
    for (auto [resource, state] : sampledResources)
    {
        if ((state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0)
        {
            Transition(commands, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state);
        }
    }
    ID3D12DescriptorHeap* engine[] = {g_EngineHeap.Get()};
    commands->SetDescriptorHeaps(1, engine);
    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE RayReflections::GetResultSrv(UINT frameIndex)
{
    return Gpu(g_EngineHeap.Get(), kResultSrv + frameIndex % kFrames);
}

RayReflections::SamplingLayout RayReflections::GetReflectionSamplingLayout()
{
    return g_ReflectionSampling;
}

UINT RayReflections::GetDenoiserDispatchCount()
{
    return g_Dispatches;
}

bool RayReflections::DidRestartHistory()
{
    return g_Denoiser && g_Denoiser->DidRestartHistory();
}

ID3D12Resource* RayReflections::GetResultResource()
{
    return g_Frames[g_LastFrame].Result.Get();
}

ID3D12Resource* RayReflections::GetNoisyResource()
{
    return g_Frames[g_LastFrame].Noisy.Get();
}

bool RayReflections::IsGiActive()
{
    return g_GiActive;
}

bool RayReflections::IsOptixGiActive()
{
    return g_OptixGiActive;
}

bool RayReflections::IsOptixReflectionActive()
{
    return g_OptixReflectionActive;
}

D3D12_GPU_DESCRIPTOR_HANDLE RayReflections::GetGiSrv(UINT frameIndex)
{
    return Gpu(g_EngineHeap.Get(), kGiSrv + frameIndex % kFrames);
}

ID3D12Resource* RayReflections::GetGiResource()
{
    return g_Frames[g_LastFrame].GiResult.Get();
}

ID3D12Resource* RayReflections::GetGiNoisyResource()
{
    return g_Frames[g_LastFrame].GiNoisy.Get();
}
