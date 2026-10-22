#include "pch.h"
#include "gpumemorybudget.h"
#include "raytracing.h"
#include "raytracingscene.h"
#include "rendererstate.h"
#include "graphicslog.h"
#include "camera.h"
#include "world.h"
#include "nrddenoiser.h"
#include "rayreflections.h"
#include "optixdenoiser.h"
#include "renderersettings.h"
#include "rendertargets.h"
#include "graphicsdevice.h"
#include "materialmanager.h"
#include "materialsystem.h"
#include "temporalhistorycontext.h"
#include "raytelemetry.h"
#include "renderquality.h"
#include <map>
#include <tuple>
#include <unordered_set>

namespace
{
    constexpr UINT kFrameCount = RendererState::g_kFRAME_COUNT;
    constexpr UINT kSrvStart = RendererState::g_kHIZ_MIP_SRV_START_INDEX + 2 * RendererState::g_kHIZ_MAX_MIPS;
    constexpr UINT kUavStart = kSrvStart + kFrameCount;
    constexpr UINT kFallbackSrv = kUavStart + kFrameCount;
    constexpr UINT kGuideUavStart = kSrvStart + 16; // 既存の DXR と DLSS の記述子領域の後に配置する
    constexpr UINT kNrdShadowSrvStart = kGuideUavStart + 5 * kFrameCount;
    constexpr UINT kNrdAmbientSrvStart = kNrdShadowSrvStart + kFrameCount;
    static_assert(kNrdAmbientSrvStart + kFrameCount <= RendererState::g_kENGINE_DESCRIPTOR_END);
    static_assert(kFallbackSrv < RendererState::g_kENGINE_DESCRIPTOR_END, "DXR descriptor range exceeds engine heap");
    constexpr UINT kConstantStride = 256;
    constexpr UINT kMaximumInstances = RendererState::g_kGLASS_MAX_MESHES;

    using GeometryKey = tuple<UINT64, UINT64, UINT, UINT, UINT, UINT>;

    struct BottomLevel
    {
        ComPtr<ID3D12Resource> Result;
        ComPtr<ID3D12Resource> Scratch;
        ComPtr<ID3D12Resource> Vertices;
        ComPtr<ID3D12Resource> Indices;
        UINT64 LastSeen = 0;
        UINT64 LastBuilt = 0;
        // コンパクション前の結果サイズ（統計用）。
        UINT64 UncompactedBytes = 0;
        // 最後のフルビルドからのリフィット回数（変形 BLAS の定期再構築判定）。
        UINT UpdatesSinceBuild = 0;
        // CompactionSizes 内の照会スロット。UINT_MAX は照会なし。
        UINT CompactionQuery = UINT_MAX;
        bool AllowsCompaction = false;
        bool Compacted = false;
        // R16G16B16A16_SNORM + Transform3x4 で構築したか。リフィットは同じ形式でのみ可能。
        bool Quantized = false;
    };

    // 量子化頂点は頂点ソース単位で共有する（PMX の材質別サブメッシュが同じ頂点バッファを参照するため）。
    using VertexSourceKey = tuple<UINT64, UINT, UINT>;

    struct PackedVertices
    {
        ComPtr<ID3D12Resource> Buffer;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        UINT64 LastUsed = 0;
        UINT64 LastWritten = 0;
    };

    constexpr UINT64 kPackedPositionOffset = 64;
    constexpr UINT kPackedStride = 8;

    struct GlassMesh
    {
        XMFLOAT4X4 World{}, NormalToWorld{};
        XMUINT4 Layout{};
        XMFLOAT4 BaseColor{}, Emission{}, Optical{}, Absorption{}, OpticalExtra{};
        XMUINT4 Flags{}; // 媒質 ID、テクスチャ有無、三角形数、予約領域
    };

    static_assert(sizeof(GlassMesh) == 240);

    struct GlassDescriptorRecord
    {
        ComPtr<ID3D12Resource> Vertices, Indices, Texture;
        UINT ResidentMip = UINT_MAX;
        bool Valid = false;
    };

    struct FrameResources
    {
        array<ComPtr<ID3D12Resource>, 4> Guides;
        ComPtr<ID3D12Resource> AmbientGuide;
        map<GeometryKey, BottomLevel> Geometry;
        map<VertexSourceKey, PackedVertices> PackedSources;
        // BLAS コンパクションのサイズ照会（GPU 書き込み先とリードバック）。
        ComPtr<ID3D12Resource> CompactionSizes;
        ComPtr<ID3D12Resource> CompactionReadback;
        vector<GeometryKey> CompactionPending;
        bool CompactionReadbackValid = false;
        ComPtr<ID3D12Resource> TopLevel;
        ComPtr<ID3D12Resource> Scratch;
        ComPtr<ID3D12Resource> Instances;
        ComPtr<ID3D12Resource> Result;
        UINT Width = 0;
        vector<RaytracingGeometry> SceneGeometry;
        ComPtr<ID3D12Resource> GlassMetadata;
        vector<GlassDescriptorRecord> GlassDescriptors;
        bool GlassReady = false;
        bool GlassReading = false;
        vector<D3D12_RESOURCE_BARRIER> GlassRestore;
        unordered_set<ID3D12Resource*> GlassSkinnedVertices;
        // 同じコマンドリスト内ですでにシーン SRV が以前の TLAS を参照している可能性がある。
        // 置き換えたリソースは、このフレームのフェンスが再利用されるまで保持する。
        vector<ComPtr<ID3D12Resource>> Retired;
        UINT Height = 0;
    };

    struct Constants
    {
        XMFLOAT4X4 InvViewProjection;
        XMFLOAT4 SunDirection;
        XMUINT4 ExtentSamples;
        XMFLOAT4 Distances;
        XMUINT4 Features;
        XMFLOAT4X4 WorldToView;
        XMFLOAT4 Jitter;
        XMFLOAT4 Stability;
    };

    struct LightingConstants
    {
        XMFLOAT4 Flags{};
        XMFLOAT4 Parameters{};
        array<XMUINT4, 160> Lights{};
    };

    constexpr UINT kLightingStride = (sizeof(LightingConstants) + 255u) & ~255u;
    array<XMUINT4, 160> g_LightShadowFlags{};
    ComPtr<ID3D12Resource> g_LightingConstants;
    static_assert(sizeof(Constants) <= kConstantStride);

    ComPtr<ID3D12Device5> g_Device;
    ComPtr<ID3D12DescriptorHeap> g_Heap;
    ComPtr<ID3D12RootSignature> g_RootSignature;
    ComPtr<ID3D12PipelineState> g_Pipeline;
    ComPtr<ID3D12PipelineState> g_TelemetryPipeline;
    bool g_TelemetryPipelineTried = false;
    ComPtr<ID3D12PipelineState> g_ComposeShadow;
    ComPtr<ID3D12PipelineState> g_ComposeAmbient;
    unique_ptr<NrdDenoiser> g_NrdSun;
    unique_ptr<NrdDenoiser> g_NrdAmbient;
    UINT g_NrdWidth = 0, g_NrdHeight = 0, g_NrdDispatchCount = 0;
    UINT g_AmbientDispatchCount = 0;
    NrdFrame g_PreviousNrdFrame;
    TemporalHistory::State g_PreviousTemporalState;
    uint64_t g_GuideSerial = 0;
    bool g_NrdHistoryValid = false;
    bool g_ShadowHistoryValid = false;
    bool g_AmbientHistoryValid = false;
    EntityID g_NrdCamera = g_kINVALID_ENTITY;
    ComPtr<ID3D12Resource> g_Constants;
    array<FrameResources, kFrameCount> g_Frames;
    Raytracing::Settings g_Settings;
    UINT g_Increment = 0;
    UINT g_FrameIndex = 0;
    UINT g_InstanceCount = 0;
    UINT64 g_Serial = 0;
    bool g_Active = false;
    bool g_ReflectionsActive = false;
    bool g_GiActive = false;
    ComPtr<ID3D12RootSignature> g_QuantizeRoot;
    ComPtr<ID3D12PipelineState> g_QuantizePipeline;
    bool g_QuantizeTried = false;
    UINT g_RebuildStagger = 0;
    Raytracing::BlasStatistics g_BlasStats;
    XMFLOAT4 g_SunDirection{};
    const char* g_Status = "未初期化";

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Heap->GetCPUDescriptorHandleForHeapStart(), index, g_Increment);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(g_Heap->GetGPUDescriptorHandleForHeapStart(), index, g_Increment);
    }

    bool EnsureBuffer(ComPtr<ID3D12Resource>& resource, UINT64 bytes, D3D12_HEAP_TYPE heapType,
                      D3D12_RESOURCE_STATES state, const wchar_t* name)
    {
        bytes = (bytes + 255ull) & ~255ull;
        if (resource && resource->GetDesc().Width >= bytes)
        {
            return true;
        }
        const auto heap = CD3DX12_HEAP_PROPERTIES(heapType);
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        if (heapType == D3D12_HEAP_TYPE_DEFAULT)
        {
            flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
        ComPtr<ID3D12Resource> created;
        const HRESULT result = g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state,
                                                                 nullptr, IID_PPV_ARGS(&created));
        if (FAILED(result))
        {
            Debug::Log("DXR: buffer allocation failed (0x%08X)\n", static_cast<unsigned int>(result));
            return false;
        }
        created->SetName(name);
        if (resource)
        {
            g_Frames[g_FrameIndex].Retired.push_back(resource);
        }
        resource = move(created);
        return true;
    }

    void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
    {
        if (resource && before != after && before != D3D12_RESOURCE_STATE_GENERIC_READ &&
            after != D3D12_RESOURCE_STATE_GENERIC_READ)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
            commandList->ResourceBarrier(1, &barrier);
        }
    }

    bool EnsureOutput(FrameResources& frame, UINT width, UINT height)
    {
        if (frame.Result && frame.Width == width && frame.Height == height)
        {
            return true;
        }
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16_FLOAT, width, height, 1, 1, 1, 0,
                                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> created;
        if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&created))))
        {
            return false;
        }
        created->SetName(L"Raytraced Sun Shadow and AO");
        frame.Result = move(created);
        frame.Width = width;
        frame.Height = height;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = description.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_Device->CreateShaderResourceView(frame.Result.Get(), &srv, Cpu(kSrvStart + g_FrameIndex));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = description.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_Device->CreateUnorderedAccessView(frame.Result.Get(), nullptr, &uav, Cpu(kUavStart + g_FrameIndex));
        return true;
    }

    // NRD ガイドは同じコマンドリスト内で生成・利用し、以前の法線と viewZ は NRD が保持する。
    // そのため、従来のフレームスロット別の複製を共有の1組へ置き換える。
    // 全解像度テクスチャ5枚を3スロット分から1組へ削減し、各スロットは記録時の組を参照する。
    // これによりリサイズ時も、実行中のフレームが使うテクスチャを解放しない。
    array<ComPtr<ID3D12Resource>, 5> g_SharedGuides;

    bool EnsureSharedGuide(UINT index, DXGI_FORMAT format, UINT width, UINT height)
    {
        auto& guide = g_SharedGuides[index];
        if (guide && guide->GetDesc().Width == width && guide->GetDesc().Height == height)
        {
            return true;
        }
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description =
            CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> created;
        if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&created))))
        {
            return false;
        }
        created->SetName(index < 4 ? L"NRD Primary Surface Guide (shared)" : L"NRD Ambient Guide (shared)");
        guide = move(created);
        return true;
    }

    bool EnsureDenoiser(FrameResources& frame)
    {
        if (!NrdDenoiser::IsCompiled() || !g_ComposeShadow)
        {
            return false;
        }
        constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                                           DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT};
        for (UINT index = 0; index < 5; ++index)
        {
            if (!EnsureSharedGuide(index, formats[index], frame.Width, frame.Height))
            {
                return false;
            }
            auto& slotGuide = index < 4 ? frame.Guides[index] : frame.AmbientGuide;
            if (slotGuide == g_SharedGuides[index])
            {
                continue;
            }
            slotGuide = g_SharedGuides[index];
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = formats[index];
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_Device->CreateUnorderedAccessView(slotGuide.Get(), nullptr, &uav,
                                                Cpu(kGuideUavStart + g_FrameIndex * 5 + index));
        }
        const bool resize = g_NrdWidth != frame.Width || g_NrdHeight != frame.Height;
        const bool needSun = g_Settings.Denoise && Raytracing::UsesRaytracedShadows();
        const bool needAmbient = g_Settings.Denoise && Raytracing::UsesRaytracedAmbientOcclusion();
        if (resize)
        {
            if ((g_NrdSun || g_NrdAmbient) && !GraphicsDevice::WaitForGpuIdle())
            {
                return false;
            }
            g_NrdSun.reset();
            g_NrdAmbient.reset();
            g_NrdWidth = frame.Width;
            g_NrdHeight = frame.Height;
            g_NrdHistoryValid = g_ShadowHistoryValid = g_AmbientHistoryValid = false;
        }
        if (needSun && !g_NrdSun)
        {
            auto created = make_unique<NrdDenoiser>();
            if (!created->Initialize(g_Device.Get(), NrdSignal::Shadow, frame.Width, frame.Height))
            {
                return false;
            }
            g_NrdSun = move(created);
            g_ShadowHistoryValid = false;
        }
        if (needAmbient && !g_NrdAmbient)
        {
            auto created = make_unique<NrdDenoiser>();
            if (!created->Initialize(g_Device.Get(), NrdSignal::AmbientOcclusion, frame.Width, frame.Height))
            {
                return false;
            }
            g_NrdAmbient = move(created);
            g_AmbientHistoryValid = false;
        }
        return true;
    }

    // 各 BLAS がスクラッチ領域を所有し、独立した構築処理を重ねられるようにする。
    // 後続 BLAS の確保に失敗した場合も、ジオメトリの状態を復元する。
    class GeometryReadBatch
    {
      public:
        explicit GeometryReadBatch(ID3D12GraphicsCommandList* commandList) : m_CommandList(commandList) {}

        void Add(ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
        {
            if (!resource || state == D3D12_RESOURCE_STATE_GENERIC_READ ||
                state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE || !m_Resources.emplace(resource, state).second)
            {
                return;
            }
            m_ReadBarriers.push_back(
                CD3DX12_RESOURCE_BARRIER::Transition(resource, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            m_FinishBarriers.push_back(
                CD3DX12_RESOURCE_BARRIER::Transition(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state));
        }

        void Begin()
        {
            if (!m_ReadBarriers.empty())
            {
                m_CommandList->ResourceBarrier(static_cast<UINT>(m_ReadBarriers.size()), m_ReadBarriers.data());
            }
        }

        void PrepareUpdate(ID3D12Resource* result)
        {
            // ラスタライティングが Execute による BLAS 更新前に、このスロットの前の TLAS を読む場合がある。
            // 再フィットの前にその読み取りを完了させる。
            if (result)
            {
                m_ReadBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(result));
            }
        }

        void CompleteBuild(ID3D12Resource* result)
        {
            m_FinishBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(result));
        }

        void Finish()
        {
            if (!m_FinishBarriers.empty())
            {
                m_CommandList->ResourceBarrier(static_cast<UINT>(m_FinishBarriers.size()), m_FinishBarriers.data());
                m_FinishBarriers.clear();
            }
        }

        ~GeometryReadBatch()
        {
            Finish();
        }

      private:
        ID3D12GraphicsCommandList* m_CommandList;
        map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_Resources;
        vector<D3D12_RESOURCE_BARRIER> m_ReadBarriers;
        vector<D3D12_RESOURCE_BARRIER> m_FinishBarriers;
    };

    bool EnsureQuantizePipeline()
    {
        if (g_QuantizeTried)
        {
            return g_QuantizePipeline != nullptr;
        }
        g_QuantizeTried = true;
        CD3DX12_ROOT_PARAMETER parameters[3];
        parameters[0].InitAsConstants(4, 0);
        parameters[1].InitAsShaderResourceView(0);
        parameters[2].InitAsUnorderedAccessView(0);
        CD3DX12_ROOT_SIGNATURE_DESC rootDescription(3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> serialized, errors;
        if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
            FAILED(g_Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                 IID_PPV_ARGS(&g_QuantizeRoot))))
        {
            WriteGraphicsLog("DXR: BLAS quantize root signature failed; 32bit BLAS vertices are used\n");
            return false;
        }
        ComPtr<ID3DBlob> shader;
        errors.Reset();
        if (FAILED(D3DCompileFromFile(L"shader/hlsl/BlasQuantizeCS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                      "main", "cs_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors)))
        {
            WriteGraphicsLog("DXR: BlasQuantizeCS compile failed: %s\n",
                             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            g_QuantizeRoot.Reset();
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = g_QuantizeRoot.Get();
        description.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
        if (FAILED(g_Device->CreateComputePipelineState(&description, IID_PPV_ARGS(&g_QuantizePipeline))))
        {
            WriteGraphicsLog("DXR: BlasQuantizeCS pipeline creation failed\n");
            g_QuantizeRoot.Reset();
            return false;
        }
        g_QuantizePipeline->SetName(L"BLAS Vertex Quantize");
        return true;
    }

    VertexSourceKey MakeVertexSourceKey(const RaytracingGeometry& geometry)
    {
        return VertexSourceKey(geometry.VertexView.BufferLocation, geometry.VertexView.SizeInBytes,
                               geometry.VertexView.StrideInBytes);
    }

    // 16bit SNORM の量子化誤差（AABB 半幅 / 32767 の半分）がワールド空間の許容値以内なら量子化する。
    bool WantsQuantizedVertices(const RaytracingGeometry& geometry)
    {
        const UINT stride = geometry.VertexView.StrideInBytes;
        if (!g_Settings.QuantizeBlasVertices || stride < sizeof(XMFLOAT3) || (stride & 3u) != 0 ||
            (geometry.VertexView.BufferLocation & 3ull) != 0 || geometry.VertexView.SizeInBytes < stride * 3)
        {
            return false;
        }
        if (geometry.Skinned)
        {
            return EnsureQuantizePipeline();
        }
        if (!(geometry.ObjectRadius > 0.0f))
        {
            return false;
        }
        const XMMATRIX world = XMLoadFloat4x4(&geometry.World);
        const float scale =
            (std::max)({XMVectorGetX(XMVector3Length(world.r[0])), XMVectorGetX(XMVector3Length(world.r[1])),
                        XMVectorGetX(XMVector3Length(world.r[2]))});
        const float worldError = geometry.ObjectRadius * scale / 32767.0f * 0.5f;
        return worldError <= (std::max)(g_Settings.BlasQuantizationMaxError, 0.0f) && EnsureQuantizePipeline();
    }

    bool EnsureCompactionBuffers(FrameResources& frame)
    {
        const UINT64 bytes = static_cast<UINT64>(kMaximumInstances) * sizeof(UINT64);
        if (!frame.CompactionSizes)
        {
            if (!EnsureBuffer(frame.CompactionSizes, bytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"BLAS Compaction Sizes"))
            {
                return false;
            }
        }
        if (!frame.CompactionReadback)
        {
            if (!EnsureBuffer(frame.CompactionReadback, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                              L"BLAS Compaction Readback"))
            {
                return false;
            }
        }
        return true;
    }

    // 同じフレームスロットの前回コマンドは完了済み（Retired と同じ前提）なので、前回照会したサイズを読める。
    void ApplyPendingCompaction(ID3D12GraphicsCommandList4* commandList, FrameResources& frame)
    {
        if (frame.CompactionPending.empty())
        {
            frame.CompactionReadbackValid = false;
            return;
        }
        const UINT count = static_cast<UINT>(frame.CompactionPending.size());
        const UINT64* sizes = nullptr;
        void* mapped = nullptr;
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(count) * sizeof(UINT64)};
        if (frame.CompactionReadbackValid && frame.CompactionReadback &&
            SUCCEEDED(frame.CompactionReadback->Map(0, &readRange, &mapped)))
        {
            sizes = static_cast<const UINT64*>(mapped);
        }
        vector<D3D12_RESOURCE_BARRIER> barriers;
        for (UINT index = 0; index < count; ++index)
        {
            const auto found = frame.Geometry.find(frame.CompactionPending[index]);
            if (found == frame.Geometry.end() || found->second.CompactionQuery != index)
            {
                continue;
            }
            BottomLevel& level = found->second;
            level.CompactionQuery = UINT_MAX;
            if (!sizes || level.Compacted || !level.Result || !g_Settings.CompactStaticBlas)
            {
                continue;
            }
            const UINT64 current = level.Result->GetDesc().Width;
            const UINT64 compactedSize = sizes[index];
            // 圧縮の効果がない（または無効値）なら再照会しない。
            if (compactedSize == 0 || compactedSize > current || current - compactedSize < 4096)
            {
                level.Compacted = true;
                continue;
            }
            ComPtr<ID3D12Resource> compacted;
            if (!EnsureBuffer(compacted, compactedSize, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"Scene BLAS (compacted)"))
            {
                continue;
            }
            commandList->CopyRaytracingAccelerationStructure(compacted->GetGPUVirtualAddress(),
                                                             level.Result->GetGPUVirtualAddress(),
                                                             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
            // 旧 BLAS は、このコマンドリストの前段（前回 TLAS を読むラスタライティング）から参照され得る。
            // スロットのフェンス完了まで保持する。
            frame.Retired.push_back(level.Result);
            if (level.Scratch)
            {
                frame.Retired.push_back(level.Scratch);
                level.Scratch.Reset();
            }
            level.UncompactedBytes = current;
            level.Result = move(compacted);
            level.Compacted = true;
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(level.Result.Get()));
        }
        if (mapped)
        {
            const D3D12_RANGE noWrite{0, 0};
            frame.CompactionReadback->Unmap(0, &noWrite);
        }
        frame.CompactionPending.clear();
        frame.CompactionReadbackValid = false;
        if (!barriers.empty())
        {
            // TLAS 構築とレイ判定の前にコピー結果を可視化する。
            commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
    }

    // ビルド済み静的 BLAS のコンパクションサイズを照会する（ビルドと同時に照会できなかったもの）。
    void QueryCompaction(ID3D12GraphicsCommandList4* commandList, FrameResources& frame, const GeometryKey& key,
                         BottomLevel& level)
    {
        if (!g_Settings.CompactStaticBlas || !level.AllowsCompaction || level.Compacted ||
            level.CompactionQuery != UINT_MAX || !level.Result || level.LastBuilt == g_Serial ||
            frame.CompactionPending.size() >= kMaximumInstances || !EnsureCompactionBuffers(frame))
        {
            return;
        }
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC query{};
        query.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
        query.DestBuffer = frame.CompactionSizes->GetGPUVirtualAddress() +
                           static_cast<UINT64>(frame.CompactionPending.size()) * sizeof(UINT64);
        const D3D12_GPU_VIRTUAL_ADDRESS source = level.Result->GetGPUVirtualAddress();
        commandList->EmitRaytracingAccelerationStructurePostbuildInfo(&query, 1, &source);
        level.CompactionQuery = static_cast<UINT>(frame.CompactionPending.size());
        frame.CompactionPending.push_back(key);
    }

    // 量子化が必要な頂点ソースを 1 ソース 1 グループでまとめてディスパッチする。
    // 戻り値: 頂点ソース → BLAS 入力に使える量子化バッファ（NON_PIXEL_SHADER_RESOURCE 状態）。
    map<VertexSourceKey, ID3D12Resource*> QuantizeVertexSources(
        ID3D12GraphicsCommandList4* commandList, FrameResources& frame,
        const map<VertexSourceKey, const RaytracingGeometry*>& sources)
    {
        map<VertexSourceKey, ID3D12Resource*> ready;
        if (sources.empty() || !g_QuantizePipeline)
        {
            return ready;
        }
        vector<pair<PackedVertices*, const RaytracingGeometry*>> dispatches;
        vector<D3D12_RESOURCE_BARRIER> toWrite;
        for (const auto& [key, geometry] : sources)
        {
            PackedVertices& packed = frame.PackedSources[key];
            packed.LastUsed = g_Serial;
            if (packed.LastWritten == g_Serial && packed.Buffer)
            {
                ready[key] = packed.Buffer.Get();
                continue;
            }
            const UINT vertexCount = geometry->VertexView.SizeInBytes / geometry->VertexView.StrideInBytes;
            const UINT64 bytes = kPackedPositionOffset + static_cast<UINT64>(vertexCount) * kPackedStride;
            ID3D12Resource* previous = packed.Buffer.Get();
            if (!EnsureBuffer(packed.Buffer, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              L"BLAS Quantized Vertices"))
            {
                frame.PackedSources.erase(key);
                continue;
            }
            if (packed.Buffer.Get() != previous)
            {
                packed.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            if (packed.State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                toWrite.push_back(CD3DX12_RESOURCE_BARRIER::Transition(packed.Buffer.Get(), packed.State,
                                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                packed.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            dispatches.emplace_back(&packed, geometry);
            ready[key] = packed.Buffer.Get();
        }
        if (!toWrite.empty())
        {
            commandList->ResourceBarrier(static_cast<UINT>(toWrite.size()), toWrite.data());
        }
        if (dispatches.empty())
        {
            return ready;
        }
        RenderProfiler::ScopedEvent profile("BLAS Vertex Quantize", commandList);
        commandList->SetComputeRootSignature(g_QuantizeRoot.Get());
        commandList->SetPipelineState(g_QuantizePipeline.Get());
        vector<D3D12_RESOURCE_BARRIER> toRead;
        toRead.reserve(dispatches.size());
        for (auto& [packed, geometry] : dispatches)
        {
            const UINT constants[4] = {geometry->VertexView.SizeInBytes / geometry->VertexView.StrideInBytes,
                                       geometry->VertexView.StrideInBytes, 0u, 0u};
            commandList->SetComputeRoot32BitConstants(0, 4, constants, 0);
            commandList->SetComputeRootShaderResourceView(1, geometry->VertexView.BufferLocation);
            commandList->SetComputeRootUnorderedAccessView(2, packed->Buffer->GetGPUVirtualAddress());
            commandList->Dispatch(1, 1, 1);
            packed->LastWritten = g_Serial;
            packed->State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            toRead.push_back(CD3DX12_RESOURCE_BARRIER::Transition(packed->Buffer.Get(),
                                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        }
        // 量子化結果（頂点と Transform3x4）を BLAS 構築入力として読める状態にする。
        commandList->ResourceBarrier(static_cast<UINT>(toRead.size()), toRead.data());
        return ready;
    }

    bool BuildBottomLevel(ID3D12GraphicsCommandList4* commandList, const RaytracingGeometry& geometry,
                          BottomLevel& level, GeometryReadBatch& geometryReads, FrameResources& frame,
                          const GeometryKey& key, ID3D12Resource* packed, UINT& rebuildBudget)
    {
        level.LastSeen = g_Serial;
        if (level.Result && (!geometry.Deforming || level.LastBuilt == g_Serial))
        {
            return true;
        }
        const bool firstBuild = !level.Result;
        const bool quantized = packed != nullptr;
        const UINT vertexCount = geometry.VertexView.SizeInBytes / geometry.VertexView.StrideInBytes;
        D3D12_RAYTRACING_GEOMETRY_DESC description{};
        description.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        description.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        description.Triangles.VertexBuffer.StartAddress = geometry.VertexView.BufferLocation;
        description.Triangles.VertexBuffer.StrideInBytes = geometry.VertexView.StrideInBytes;
        description.Triangles.VertexCount = vertexCount;
        description.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        if (quantized)
        {
            // 16bit SNORM 頂点 + 復元行列。BLAS 空間は元のオブジェクト空間のまま。
            description.Triangles.Transform3x4 = packed->GetGPUVirtualAddress();
            description.Triangles.VertexBuffer.StartAddress = packed->GetGPUVirtualAddress() + kPackedPositionOffset;
            description.Triangles.VertexBuffer.StrideInBytes = kPackedStride;
            description.Triangles.VertexFormat = DXGI_FORMAT_R16G16B16A16_SNORM;
        }
        description.Triangles.IndexBuffer = geometry.IndexView.BufferLocation;
        description.Triangles.IndexCount = geometry.IndexCount;
        description.Triangles.IndexFormat = geometry.IndexView.Format;
        if (!geometry.Indices)
        {
            description.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
        }
        const bool compact = !geometry.Deforming && g_Settings.CompactStaticBlas && EnsureCompactionBuffers(frame) &&
                             frame.CompactionPending.size() < kMaximumInstances;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = &description;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (compact)
        {
            inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        }
        if (geometry.Deforming)
        {
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                           D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        }
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
        g_Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
        // リフィットは同じ頂点形式でのみ可能。リフィットを重ねると BVH が劣化するため、
        // 一定回数ごとにフルビルドへ切り替える（1 フレームの再構築数は予算で分散）。
        bool update = geometry.Deforming && level.Result && level.Quantized == quantized;
        const UINT interval = static_cast<UINT>((std::max)(g_Settings.DeformingBlasRebuildInterval, 0));
        if (update && interval > 0 && level.UpdatesSinceBuild >= interval && rebuildBudget > 0)
        {
            update = false;
            --rebuildBudget;
        }
        if (sizes.ResultDataMaxSizeInBytes == 0 ||
            !EnsureBuffer(level.Result, sizes.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"Scene BLAS") ||
            !EnsureBuffer(level.Scratch, (max)(sizes.ScratchDataSizeInBytes, sizes.UpdateScratchDataSizeInBytes),
                          D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"BLAS Scratch"))
        {
            return false;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs = inputs;
        build.DestAccelerationStructureData = level.Result->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = level.Scratch->GetGPUVirtualAddress();
        if (update)
        {
            build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            build.SourceAccelerationStructureData = build.DestAccelerationStructureData;
        }
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC query{};
        UINT queryCount = 0;
        if (compact)
        {
            query.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
            query.DestBuffer = frame.CompactionSizes->GetGPUVirtualAddress() +
                               static_cast<UINT64>(frame.CompactionPending.size()) * sizeof(UINT64);
            queryCount = 1;
            level.CompactionQuery = static_cast<UINT>(frame.CompactionPending.size());
            frame.CompactionPending.push_back(key);
        }
        // アップロードヒープは GENERIC_READ のままにする。NON_PIXEL_SHADER_RESOURCE も含まれている。
        commandList->BuildRaytracingAccelerationStructure(&build, queryCount, queryCount ? &query : nullptr);
        geometryReads.CompleteBuild(level.Result.Get());
        level.Vertices = geometry.Vertices;
        level.Indices = geometry.Indices;
        level.LastBuilt = g_Serial;
        level.Quantized = quantized;
        level.AllowsCompaction = compact;
        level.Compacted = false;
        level.UncompactedBytes = level.Result->GetDesc().Width;
        if (update)
        {
            ++level.UpdatesSinceBuild;
            ++g_BlasStats.RefitsThisFrame;
        }
        else
        {
            // 初回ビルドは開始位置をずらし、多数のキャラクターの再構築が同じフレームに集中しないようにする。
            level.UpdatesSinceBuild = 0;
            if (geometry.Deforming && firstBuild && interval > 0)
            {
                level.UpdatesSinceBuild = (g_RebuildStagger++) % interval;
            }
            ++g_BlasStats.RebuildsThisFrame;
        }
        return true;
    }

    bool EnsureTelemetryPipeline()
    {
        if (!g_TelemetryPipelineTried)
        {
            g_TelemetryPipelineTried = true;
            const D3D_SHADER_MACRO defines[] = {
                {"USE_NRD", NrdDenoiser::IsCompiled() ? "1" : "0"}, {"ENABLE_RAY_TELEMETRY", "1"}, {}};
            ComPtr<ID3DBlob> shader, errors;
            const HRESULT result =
                D3DCompileFromFile(L"shader/hlsl/RaytracingCS.hlsl", defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
                                   "cs_6_5", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
            if (SUCCEEDED(result))
            {
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
                desc.pRootSignature = g_RootSignature.Get();
                desc.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
                g_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&g_TelemetryPipeline));
            }
            if (!g_TelemetryPipeline)
            {
                WriteGraphicsLog("Ray telemetry: shadow/AO counter variant unavailable: %s\n",
                                 errors ? static_cast<const char*>(errors->GetBufferPointer())
                                        : "pipeline creation failed");
            }
        }
        return g_TelemetryPipeline != nullptr;
    }

    UINT GlassTableIndex(UINT slot)
    {
        return RendererState::g_kGLASS_DESCRIPTOR_START + slot * RendererState::g_kGLASS_FRAME_DESCRIPTORS;
    }

    bool BuildGlassMetadata(FrameResources& frame)
    {
        frame.GlassReady = false;
        const UINT count = static_cast<UINT>(frame.SceneGeometry.size());
        if (!count || !EnsureBuffer(frame.GlassMetadata, count * sizeof(GlassMesh), D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, L"Glass mesh metadata"))
        {
            return false;
        }
        const UINT start = GlassTableIndex(g_FrameIndex);
        void* data = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (FAILED(frame.GlassMetadata->Map(0, &noRead, &data)))
        {
            return false;
        }
        auto* meshes = static_cast<GlassMesh*>(data);
        frame.GlassDescriptors.resize(count);
        for (UINT i = 0; i < count; ++i)
        {
            const auto& g = frame.SceneGeometry[i];
            GlassMesh m{};
            XMMATRIX world = XMLoadFloat4x4(&g.World);
            XMStoreFloat4x4(&m.World, XMMatrixTranspose(world));
            XMStoreFloat4x4(&m.NormalToWorld, XMMatrixInverse(nullptr, world));
            UINT indexBitWidth = 0;
            if (g.Indices)
            {
                indexBitWidth = 32;
                if (g.IndexView.Format == DXGI_FORMAT_R16_UINT)
                {
                    indexBitWidth = 16;
                }
            }
            m.Layout = {g.VertexView.StrideInBytes, indexBitWidth,
                        static_cast<UINT>(g.VertexView.BufferLocation - g.Vertices->GetGPUVirtualAddress()),
                        g.Indices ? static_cast<UINT>(g.IndexView.BufferLocation - g.Indices->GetGPUVirtualAddress())
                                  : 0u};
            m.BaseColor = g.BaseColor;
            m.Emission = g.Emission;
            m.Optical = {g.IOR, g.Transmission, g.Roughness, g.Metallic};
            m.Absorption = {g.Absorption.x, g.Absorption.y, g.Absorption.z, g.Thickness};
            m.OpticalExtra = {g.TransmissionRoughness, g.Fresnel, 0, 0};
            m.Flags = {g.MediumID, static_cast<UINT>(g.Texture != nullptr),
                       g.Indices ? g.IndexCount / 3 : g.VertexView.SizeInBytes / g.VertexView.StrideInBytes / 3,
                       static_cast<UINT>(g.UsesAssetMaterial)};
            meshes[i] = m;
            D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
            raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            raw.Format = DXGI_FORMAT_R32_TYPELESS;
            raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            raw.Buffer.NumElements = static_cast<UINT>(g.Vertices->GetDesc().Width / 4);
            auto& cached = frame.GlassDescriptors[i];
            if (!cached.Valid || cached.Vertices.Get() != g.Vertices.Get())
            {
                g_Device->CreateShaderResourceView(g.Vertices.Get(), &raw, Cpu(start + 1 + i));
            }
            raw.Buffer.NumElements = g.Indices ? static_cast<UINT>(g.Indices->GetDesc().Width / 4) : 1;
            if (!cached.Valid || cached.Indices.Get() != g.Indices.Get())
            {
                g_Device->CreateShaderResourceView(g.Indices.Get(), &raw, Cpu(start + 1 + kMaximumInstances + i));
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC tex{};
            tex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            tex.Format = g.Texture ? g.Texture->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
            tex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            switch (tex.Format)
            {
                case DXGI_FORMAT_R32_TYPELESS:
                    tex.Format = DXGI_FORMAT_R32_FLOAT;
                    break;
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    break;
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                    tex.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    break;
                case DXGI_FORMAT_BC1_TYPELESS:
                    tex.Format = DXGI_FORMAT_BC1_UNORM;
                    break;
                case DXGI_FORMAT_BC2_TYPELESS:
                    tex.Format = DXGI_FORMAT_BC2_UNORM;
                    break;
                case DXGI_FORMAT_BC3_TYPELESS:
                    tex.Format = DXGI_FORMAT_BC3_UNORM;
                    break;
                case DXGI_FORMAT_BC7_TYPELESS:
                    tex.Format = DXGI_FORMAT_BC7_UNORM;
                    break;
                default:
                    break;
            }
            tex.Texture2D.MipLevels = g.Texture ? g.Texture->GetDesc().MipLevels : 1;
            tex.Texture2D.ResourceMinLODClamp = static_cast<float>(g.TextureResidentMip);
            if (!cached.Valid || cached.Texture.Get() != g.Texture.Get() || cached.ResidentMip != g.TextureResidentMip)
            {
                g_Device->CreateShaderResourceView(g.Texture.Get(), &tex, Cpu(start + 1 + 2 * kMaximumInstances + i));
            }
            cached.Vertices = g.Vertices;
            cached.Indices = g.Indices;
            cached.Texture = g.Texture;
            cached.ResidentMip = g.TextureResidentMip;
            cached.Valid = true;
        }
        frame.GlassMetadata->Unmap(0, nullptr);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = count;
        srv.Buffer.StructureByteStride = sizeof(GlassMesh);
        g_Device->CreateShaderResourceView(frame.GlassMetadata.Get(), &srv, Cpu(start));
        frame.GlassReady = true;
        return true;
    }

    bool BuildScene(ID3D12GraphicsCommandList4* commandList, FrameResources& frame)
    {
        RenderProfiler::ScopedEvent profile("DXR Acceleration Structures", commandList);
        const auto geometry = GatherRaytracingGeometry();
        frame.GlassReady = false;
        frame.SceneGeometry.clear();
        vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
        instances.reserve((min)(geometry.size(), static_cast<size_t>(kMaximumInstances)));
        g_BlasStats = {};
        // 前回このスロットで照会したコンパクションサイズを反映する（BLAS のアドレスが変わるので TLAS 構築前）。
        ApplyPendingCompaction(commandList, frame);
        GeometryReadBatch geometryReads(commandList);
        // 下記のインスタンス上限と有効性検証に合わせる。キャッシュ済みの静的 BLAS は入力遷移不要。
        // 共有する頂点・インデックスリソースは1回だけ状態遷移する。
        size_t accepted = 0;
        vector<char> wantsQuantized(geometry.size(), 0);
        map<VertexSourceKey, const RaytracingGeometry*> quantizeSources;
        for (size_t index = 0; index < geometry.size(); ++index)
        {
            const auto& entry = geometry[index];
            if (accepted >= kMaximumInstances || entry.VertexView.StrideInBytes < sizeof(XMFLOAT3))
            {
                continue;
            }
            ++accepted;
            const GeometryKey key(entry.VertexView.BufferLocation, entry.IndexView.BufferLocation,
                                  entry.VertexView.SizeInBytes, entry.IndexCount, entry.VertexView.StrideInBytes,
                                  static_cast<UINT>(entry.IndexView.Format));
            const auto bottom = frame.Geometry.find(key);
            if (bottom == frame.Geometry.end() || !bottom->second.Result || entry.Deforming)
            {
                geometryReads.Add(entry.Vertices.Get(), entry.VertexState);
                geometryReads.Add(entry.Indices.Get(), entry.IndexState);
                if (bottom != frame.Geometry.end())
                {
                    geometryReads.PrepareUpdate(bottom->second.Result.Get());
                }
                if (WantsQuantizedVertices(entry))
                {
                    wantsQuantized[index] = 1;
                    quantizeSources.emplace(MakeVertexSourceKey(entry), &entry);
                }
            }
        }
        geometryReads.Begin();
        // 頂点ソースの読み取り遷移後に 16bit 量子化をまとめて実行する。
        const auto quantizedSources = QuantizeVertexSources(commandList, frame, quantizeSources);
        UINT rebuildBudget = static_cast<UINT>((std::max)(g_Settings.DeformingBlasRebuildsPerFrame, 0));
        for (size_t index = 0; index < geometry.size(); ++index)
        {
            const auto& entry = geometry[index];
            if (instances.size() >= kMaximumInstances || entry.VertexView.StrideInBytes < sizeof(XMFLOAT3))
            {
                continue;
            }
            const GeometryKey key(entry.VertexView.BufferLocation, entry.IndexView.BufferLocation,
                                  entry.VertexView.SizeInBytes, entry.IndexCount, entry.VertexView.StrideInBytes,
                                  static_cast<UINT>(entry.IndexView.Format));
            BottomLevel& bottom = frame.Geometry[key];
            ID3D12Resource* packed = nullptr;
            if (wantsQuantized[index])
            {
                const auto found = quantizedSources.find(MakeVertexSourceKey(entry));
                if (found != quantizedSources.end())
                {
                    packed = found->second;
                }
            }
            if (!BuildBottomLevel(commandList, entry, bottom, geometryReads, frame, key, packed, rebuildBudget))
            {
                return false;
            }
            if (!entry.Deforming && bottom.LastBuilt != g_Serial)
            {
                // 過去フレームでビルド済みの静的 BLAS: スクラッチは再利用しないので解放し、
                // 未照会ならコンパクションサイズを照会する。
                if (bottom.Scratch)
                {
                    frame.Retired.push_back(bottom.Scratch);
                    bottom.Scratch.Reset();
                }
                QueryCompaction(commandList, frame, key, bottom);
            }
            D3D12_RAYTRACING_INSTANCE_DESC instance{};
            XMFLOAT4X4 transform;
            XMStoreFloat4x4(&transform, XMMatrixTranspose(XMLoadFloat4x4(&entry.World)));
            memcpy(instance.Transform, &transform, sizeof(instance.Transform));
            instance.InstanceID = static_cast<UINT>(instances.size());
            // ビット0は影の可視性。ビット1は AO と反射用に残す。
            // ガラスは専用の可視性ビットを使い、既存の AO/GI 判定は不透明物体としての意味を維持する。
            if (entry.Transmission > 0.001f)
            {
                instance.InstanceMask = 4;
            }
            else if (entry.CastShadow)
            {
                instance.InstanceMask = 3;
            }
            else
            {
                instance.InstanceMask = 2;
            }
            instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            instance.AccelerationStructure = bottom.Result->GetGPUVirtualAddress();
            instances.push_back(instance);
            frame.SceneGeometry.push_back(entry);
        }
        BuildGlassMetadata(frame);
        // TLAS 構築とレイ判定の前に、すべての BLAS 書き込みを可視化する。
        geometryReads.Finish();
        if (!frame.CompactionPending.empty() && frame.CompactionSizes && frame.CompactionReadback)
        {
            // ビルド時に書かれたコンパクションサイズをリードバックへ写す。次にこのスロットを使うときに読む。
            const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.CompactionSizes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandList->ResourceBarrier(1, &toCopy);
            commandList->CopyBufferRegion(frame.CompactionReadback.Get(), 0, frame.CompactionSizes.Get(), 0,
                                          static_cast<UINT64>(frame.CompactionPending.size()) * sizeof(UINT64));
            const auto toWrite = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.CompactionSizes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1, &toWrite);
            frame.CompactionReadbackValid = true;
        }
        // 今フレームで使わなかった量子化頂点（ビルド済みの静的メッシュ分など）は解放する。
        for (auto iterator = frame.PackedSources.begin(); iterator != frame.PackedSources.end();)
        {
            if (iterator->second.LastUsed != g_Serial)
            {
                if (iterator->second.Buffer)
                {
                    frame.Retired.push_back(iterator->second.Buffer);
                }
                iterator = frame.PackedSources.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        for (auto iterator = frame.Geometry.begin(); iterator != frame.Geometry.end();)
        {
            if (iterator->second.LastSeen != g_Serial)
            {
                const auto& bottom = iterator->second;
                if (bottom.Result)
                {
                    frame.Retired.push_back(bottom.Result);
                }
                if (bottom.Scratch)
                {
                    frame.Retired.push_back(bottom.Scratch);
                }
                if (bottom.Vertices)
                {
                    frame.Retired.push_back(bottom.Vertices);
                }
                if (bottom.Indices)
                {
                    frame.Retired.push_back(bottom.Indices);
                }
                iterator = frame.Geometry.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        g_BlasStats.BottomLevels = static_cast<UINT>(frame.Geometry.size());
        g_BlasStats.ResultBytes = 0;
        for (const auto& [unused, bottom] : frame.Geometry)
        {
            if (!bottom.Result)
            {
                continue;
            }
            const UINT64 bytes = bottom.Result->GetDesc().Width;
            g_BlasStats.ResultBytes += bytes;
            if (bottom.Quantized)
            {
                ++g_BlasStats.Quantized;
            }
            if (bottom.Compacted && bottom.UncompactedBytes > bytes)
            {
                ++g_BlasStats.Compacted;
                g_BlasStats.CompactionSavedBytes += bottom.UncompactedBytes - bytes;
            }
        }
        g_InstanceCount = static_cast<UINT>(instances.size());
        if (instances.empty())
        {
            return false;
        }
        if (!EnsureBuffer(frame.Instances, instances.size() * sizeof(instances.front()), D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ, L"TLAS Instance Descriptions"))
        {
            return false;
        }
        void* mapped = nullptr;
        const CD3DX12_RANGE noRead(0, 0);
        if (FAILED(frame.Instances->Map(0, &noRead, &mapped)))
        {
            return false;
        }
        memcpy(mapped, instances.data(), instances.size() * sizeof(instances.front()));
        frame.Instances->Unmap(0, nullptr);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = g_InstanceCount;
        inputs.InstanceDescs = frame.Instances->GetGPUVirtualAddress();
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
        g_Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
        if (!EnsureBuffer(frame.TopLevel, sizes.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"Scene TLAS") ||
            !EnsureBuffer(frame.Scratch, sizes.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"TLAS Scratch"))
        {
            return false;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs = inputs;
        build.DestAccelerationStructureData = frame.TopLevel->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = frame.Scratch->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(frame.TopLevel.Get());
        commandList->ResourceBarrier(1, &barrier);
        return true;
    }

    void FindSunDirection()
    {
        g_SunDirection = {};
        for (EntityID entity : World::GetView<SunComponent, LightComponent>())
        {
            const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
            if (light.Type == LightType::Directional && light.RenderMode == LightRenderMode::Physical &&
                light.IsActive && light.CastShadow && light.AffectsOpaque && light.Intensity > 0.0f)
            {
                XMFLOAT3 direction;
                XMStoreFloat3(&direction, XMVector3Normalize(XMLoadFloat3(&light.Direction)));
                g_SunDirection = {direction.x, direction.y, direction.z, 1.0f};
                return;
            }
        }
    }
} // 名前空間

bool Raytracing::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT increment)
{
    Shutdown();
    g_Heap = heap;
    g_Increment = increment;
    if (!device || !heap || increment == 0)
    {
        return false;
    }
    for (UINT slot = 0; slot < kFrameCount; ++slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC empty{};
        empty.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        empty.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        empty.Buffer.NumElements = 1;
        empty.Buffer.StructureByteStride = sizeof(GlassMesh);
        device->CreateShaderResourceView(nullptr, &empty, Cpu(GlassTableIndex(slot)));
    }
    if (!device || !heap || increment == 0)
    {
        return false;
    }
    // マテリアルのピクセルシェーダーはラスタ専用デバイスでも b5 のプレビュー識別値を読む。
    // DXR 機能の検査前に、有効なゼロ埋めの定数バッファを用意する。
    const auto lightingHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto lightingDescription = CD3DX12_RESOURCE_DESC::Buffer(kLightingStride * kFrameCount);
    if (FAILED(device->CreateCommittedResource(&lightingHeap, D3D12_HEAP_FLAG_NONE, &lightingDescription,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&g_LightingConstants))))
    {
        return false;
    }
    void* rasterLightingData = nullptr;
    D3D12_RANGE lightingNoRead{0, 0};
    if (FAILED(g_LightingConstants->Map(0, &lightingNoRead, &rasterLightingData)))
    {
        return false;
    }
    memset(rasterLightingData, 0, kLightingStride * kFrameCount);
    g_LightingConstants->Unmap(0, nullptr);
    D3D12_SHADER_RESOURCE_VIEW_DESC fallback{};
    fallback.Format = DXGI_FORMAT_R16G16_FLOAT;
    fallback.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    fallback.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    fallback.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &fallback, Cpu(kFallbackSrv));
    char disabled[8]{};
    if (GetEnvironmentVariableA("DX12_DISABLE_DXR", disabled, sizeof(disabled)) > 0 && atoi(disabled) != 0)
    {
        g_Status = "検証設定で DXR を無効にしています";
        WriteGraphicsLog("DXR: disabled by validation override; raster fallback\n");
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_5};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) ||
        options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) ||
        shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_5 ||
        FAILED(device->QueryInterface(IID_PPV_ARGS(&g_Device))))
    {
        g_Status = "DXR Tier 1.1 / Shader Model 6.5 非対応";
        WriteGraphicsLog("DXR: unavailable; keeping raster rendering\n");
        return false;
    }
    CD3DX12_DESCRIPTOR_RANGE ranges[8];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 1);
    ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
    ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);
    ranges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);
    ranges[7].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);
    CD3DX12_ROOT_PARAMETER parameters[11];
    parameters[0].InitAsShaderResourceView(0);
    for (UINT index = 0; index < 3; ++index)
    {
        parameters[index + 1].InitAsDescriptorTable(1, &ranges[index]);
    }
    parameters[4].InitAsConstantBufferView(0);
    for (UINT index = 3; index < 8; ++index)
    {
        parameters[index + 2].InitAsDescriptorTable(1, &ranges[index]);
    }
    parameters[10].InitAsUnorderedAccessView(0, 9);
    const CD3DX12_ROOT_SIGNATURE_DESC rootDescription(11, parameters);
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&g_RootSignature))))
    {
        g_Status = "DXR root signature 作成失敗";
        return false;
    }
    ComPtr<ID3DBlob> shader;
    if (FAILED(D3DReadFileToBlob(NrdDenoiser::IsCompiled() ? L"shader/hlsl/build/RaytracingNrdCS.cso"
                                                           : L"shader/hlsl/build/RaytracingCS.cso",
                                 &shader)))
    {
        g_Status = "RaytracingCS.cso がありません";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = g_RootSignature.Get();
    pipeline.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g_Pipeline))) ||
        !EnsureBuffer(g_Constants, kConstantStride * kFrameCount, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, L"Raytracing Frame Constants") ||
        !EnsureBuffer(g_LightingConstants, kLightingStride * kFrameCount, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, L"Scene Ray Lighting Constants"))
    {
        g_Pipeline.Reset();
        g_Status = "DXR pipeline 作成失敗";
        return false;
    }
    if (NrdDenoiser::IsCompiled())
    {
        shader.Reset();
        if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/NrdComposeShadowCS.cso", &shader)))
        {
            return false;
        }
        pipeline.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
        if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g_ComposeShadow))))
        {
            return false;
        }
        shader.Reset();
        if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/NrdComposeAmbientCS.cso", &shader)))
        {
            return false;
        }
        pipeline.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
        if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g_ComposeAmbient))))
        {
            return false;
        }
    }
    g_Status = "DXR Tier 1.1 対応";
    void* lightingData = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (SUCCEEDED(g_LightingConstants->Map(0, &noRead, &lightingData)))
    {
        memset(lightingData, 0, kLightingStride * kFrameCount);
        g_LightingConstants->Unmap(0, nullptr);
    }
    if (!RayReflections::Initialize(device, heap, increment))
    {
        WriteGraphicsLog("DXR: reflection pipeline initialization failed\n");
    }
    if (!OptixDenoiser::Initialize(device))
    {
        WriteGraphicsLog("OptiX unavailable: %s\n", OptixDenoiser::GetStatus());
    }
    WriteGraphicsLog("DXR: Tier 1.1 inline raytracing ready\n");
    return true;
}

void Raytracing::Shutdown()
{
    g_TelemetryPipeline.Reset();
    g_TelemetryPipelineTried = false;
    RayReflections::Shutdown();
    OptixDenoiser::Shutdown();
    g_NrdSun.reset();
    g_NrdWidth = g_NrdHeight = g_NrdDispatchCount = 0;
    g_NrdHistoryValid = false;
    g_NrdCamera = g_kINVALID_ENTITY;
    g_ComposeShadow.Reset();
    g_ComposeAmbient.Reset();
    g_NrdAmbient.reset();
    g_Frames = {};
    g_SharedGuides = {};
    g_Constants.Reset();
    g_LightingConstants.Reset();
    g_LightShadowFlags = {};
    g_Pipeline.Reset();
    g_RootSignature.Reset();
    g_Device.Reset();
    g_Heap.Reset();
    g_Active = false;
    g_ReflectionsActive = false;
    g_GiActive = false;
    g_ShadowHistoryValid = false;
    g_AmbientHistoryValid = false;
    g_InstanceCount = 0;
    g_Serial = 0;
    g_QuantizePipeline.Reset();
    g_QuantizeRoot.Reset();
    g_QuantizeTried = false;
    g_RebuildStagger = 0;
    g_BlasStats = {};
}

void Raytracing::ReleaseInactiveResources()
{
    if (!g_Device || !GpuMemoryBudget::Options.Enabled || !GpuMemoryBudget::Options.ReleaseInactive)
    {
        return;
    }
    const bool needed =
        g_Settings.Enabled && (UsesRaytracedShadows() || UsesRaytracedAmbientOcclusion() || g_Settings.Reflections ||
                               RendererSettings::GetRealtimeGiEnabled() || RenderQuality::Get().RaytracedRefraction);
    const bool releaseSun = g_NrdSun && (!needed || !g_Settings.Denoise || !UsesRaytracedShadows());
    const bool releaseAmbient = g_NrdAmbient && (!needed || !g_Settings.Denoise || !UsesRaytracedAmbientOcclusion());
    bool releaseScene = false;
    for (const auto& frame : g_Frames)
    {
        releaseScene |= !needed && (frame.TopLevel || frame.Result || !frame.Geometry.empty());
    }
    if (!(releaseScene || releaseSun || releaseAmbient) || !GraphicsDevice::WaitForGpuIdle())
    {
        return;
    }
    if (releaseSun)
    {
        g_NrdSun.reset();
        g_ShadowHistoryValid = false;
    }
    if (releaseAmbient)
    {
        g_NrdAmbient.reset();
        g_AmbientHistoryValid = false;
    }
    if (releaseScene)
    {
        g_Frames = {};
        g_SharedGuides = {};
        g_InstanceCount = 0;
        g_Active = g_ReflectionsActive = g_GiActive = false;
        g_NrdHistoryValid = false;
        g_GuideSerial = 0;
        g_BlasStats = {};
    }
}

void Raytracing::Execute(ID3D12GraphicsCommandList* commandList, ID3D12Resource* depth, ID3D12Resource* normal,
                         D3D12_GPU_DESCRIPTOR_HANDLE depthSrv, D3D12_GPU_DESCRIPTOR_HANDLE normalSrv, UINT frameIndex)
{
    g_Active = false;
    g_ReflectionsActive = false;
    g_GiActive = false;
    const bool shadowHistoryValid = g_ShadowHistoryValid;
    const bool ambientHistoryValid = g_AmbientHistoryValid;
    g_ShadowHistoryValid = false;
    g_AmbientHistoryValid = false;
    g_NrdDispatchCount = 0;
    g_AmbientDispatchCount = 0;
    g_InstanceCount = 0;
    g_FrameIndex = frameIndex % kFrameCount;
    g_Frames[g_FrameIndex].Retired.clear();
    auto updateLighting = [&](bool sceneReady) {
        if (!g_LightingConstants)
        {
            return;
        }
        LightingConstants lighting{};
        lighting.Flags = {static_cast<float>(UsesRaytracedShadows()), static_cast<float>(UsesRaytracedShadows()),
                          static_cast<float>(sceneReady),
                          sceneReady && g_Frames[g_FrameIndex].GlassReady
                              ? static_cast<float>(g_Frames[g_FrameIndex].SceneGeometry.size())
                              : 0.0f};
        lighting.Parameters = {clamp(g_Settings.ShadowDistance, 1.0f, 2000.0f),
                               clamp(g_Settings.NormalBias, 0.001f, 0.1f), 0.0f, 0.0f};
        lighting.Lights = g_LightShadowFlags;
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (SUCCEEDED(g_LightingConstants->Map(0, &noRead, &mapped)))
        {
            memcpy(static_cast<BYTE*>(mapped) + g_FrameIndex * kLightingStride, &lighting, sizeof(lighting));
            g_LightingConstants->Unmap(0, nullptr);
        }
    };
    updateLighting(false);
    if (!IsAvailable() || !g_Settings.Enabled ||
        (!UsesRaytracedShadows() && !UsesRaytracedAmbientOcclusion() && !g_Settings.Reflections &&
         !RendererSettings::GetRealtimeGiEnabled() && !RenderQuality::Get().RaytracedRefraction) ||
        !commandList || !depth || !normal)
    {
        g_NrdHistoryValid = false;
        return;
    }
    RenderProfiler::ScopedEvent profile("DXR Shadows / AO", commandList);
    ++g_Serial;
    FrameResources& frame = g_Frames[g_FrameIndex];
    const auto extent = depth->GetDesc();
    ComPtr<ID3D12GraphicsCommandList4> rayCommands;
    if (FAILED(commandList->QueryInterface(IID_PPV_ARGS(&rayCommands))) ||
        !EnsureOutput(frame, static_cast<UINT>(extent.Width), extent.Height) || !BuildScene(rayCommands.Get(), frame))
    {
        return;
    }
    FindSunDirection();
    const EntityID cameraEntity = Camera::GetCameraEntity();
    if (cameraEntity != g_NrdCamera)
    {
        g_NrdHistoryValid = false;
    }
    g_NrdCamera = cameraEntity;
    // NRD 対応の追跡シェーダーは、デノイズ無効時も4つのガイドを出力する。
    const bool guidesReady = !NrdDenoiser::IsCompiled() || EnsureDenoiser(frame);
    if (!guidesReady)
    {
        return;
    }
    XMMATRIX view;
    XMMATRIX projection;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    Constants constants{};
    XMStoreFloat4x4(&constants.InvViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, view * projection)));
    constants.SunDirection = g_SunDirection;
    constants.Stability = {g_Settings.StableSampling ? 1.f : 0.f, 0, 0, 0};
    constants.ExtentSamples = {frame.Width, frame.Height, static_cast<UINT>(clamp(g_Settings.Samples, 1, 8)),
                               static_cast<UINT>(g_Serial)};
    constants.Distances = {clamp(g_Settings.ShadowDistance, 1.0f, 2000.0f),
                           clamp(g_Settings.AmbientRadius, 0.05f, 10.0f), clamp(g_Settings.NormalBias, 0.001f, 0.1f),
                           clamp(g_Settings.SunAngularRadius, 0.0f, 0.05f)};
    constants.Features = {static_cast<UINT>(UsesRaytracedShadows()), static_cast<UINT>(UsesRaytracedAmbientOcclusion()),
                          static_cast<UINT>(NrdDenoiser::IsCompiled() && g_Settings.Denoise), 0};
    // NRD 有効時は SIGMA が最初の確率的な太陽レイだけを、REBLUR が最初の AO ヒット距離だけを使う。
    // その後 ComposeShadow / ComposeAmbient が平均可視性を上書きする。
    // 追加サンプルは無駄になるため、両履歴が有効な場合は1本だけ追跡する。
    // 初回フレームやフォールバックでは、引き続き複数サンプルの平均を使う。
    const bool shadowDenoised = !UsesRaytracedShadows() || g_SunDirection.w <= 0.5f || shadowHistoryValid;
    const bool ambientDenoised = !UsesRaytracedAmbientOcclusion() || ambientHistoryValid;
    if (constants.Features.z != 0 && shadowDenoised && ambientDenoised)
    {
        constants.ExtentSamples.z = 1;
    }
    if (constants.Features.x == 0 && constants.Features.y == 0)
    {
        // 反射にはガイドが必要だが、遮蔽されないサンプルを繰り返しても情報は増えない。
        constants.ExtentSamples.z = 1;
    }
    XMStoreFloat4x4(&constants.WorldToView, XMMatrixTranspose(view));
    NrdFrame nrdFrame;
    XMStoreFloat4x4(&nrdFrame.View, view);
    XMStoreFloat4x4(&nrdFrame.Projection, projection);
    nrdFrame.Jitter = {-nrdFrame.Projection._31 * frame.Width * 0.5f, nrdFrame.Projection._32 * frame.Height * 0.5f};
    nrdFrame.Projection._31 = nrdFrame.Projection._32 = 0;
    nrdFrame.PreviousView = g_NrdHistoryValid ? g_PreviousNrdFrame.View : nrdFrame.View;
    nrdFrame.PreviousProjection = g_NrdHistoryValid ? g_PreviousNrdFrame.Projection : nrdFrame.Projection;
    nrdFrame.PreviousJitter = g_NrdHistoryValid ? g_PreviousNrdFrame.Jitter : nrdFrame.Jitter;
    nrdFrame.LightDirection = {g_SunDirection.x, g_SunDirection.y, g_SunDirection.z};
    nrdFrame.FrameIndex = g_FrameIndex;
    nrdFrame.Sequence = static_cast<UINT>(g_Serial);
    nrdFrame.StableProfile = g_Settings.StableNrdProfile;
    nrdFrame.Range = 2000.0f;
    nrdFrame.AmbientRadius = constants.Distances.y;
    if (ComponentManager::HasComponent<CameraComponent>(cameraEntity))
    {
        nrdFrame.Range = (max)(ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity).FarClip, 1.0f);
    }
    nrdFrame.ResetHistory = !g_NrdHistoryValid;
    const uint64_t historyConfiguration = static_cast<uint64_t>(g_Settings.RenderMode) |
                                          (static_cast<uint64_t>(g_Settings.Reflections) << 8) |
                                          (static_cast<uint64_t>(g_Settings.Denoise) << 9) |
                                          (static_cast<uint64_t>(RendererSettings::GetRealtimeGiEnabled()) << 10) |
                                          (static_cast<uint64_t>(g_Settings.RadianceDenoiser) << 16) |
                                          (static_cast<uint64_t>(g_Settings.StableNrdProfile) << 20) |
                                          (static_cast<uint64_t>(g_Settings.StableSampling) << 21);
    const auto temporalState =
        TemporalHistory::Context::Capture(cameraEntity, frame.Width, frame.Height, historyConfiguration);
    const auto historyDecision = TemporalHistory::Evaluate(g_PreviousTemporalState, temporalState, g_NrdHistoryValid);
    nrdFrame.HistoryResetReasons = historyDecision.Reasons;
    nrdFrame.ResetHistory |= !historyDecision.CanReuse;
    if (nrdFrame.ResetHistory)
    {
        nrdFrame.PreviousView = nrdFrame.View;
        nrdFrame.PreviousProjection = nrdFrame.Projection;
        nrdFrame.PreviousJitter = nrdFrame.Jitter;
    }
    constants.Jitter = {nrdFrame.Jitter.x, nrdFrame.Jitter.y, nrdFrame.PreviousJitter.x, nrdFrame.PreviousJitter.y};
    memcpy(&constants.Features.w, &nrdFrame.Range, sizeof(float));
    void* mapped = nullptr;
    const CD3DX12_RANGE noRead(0, 0);
    if (FAILED(g_Constants->Map(0, &noRead, &mapped)))
    {
        return;
    }
    memcpy(static_cast<BYTE*>(mapped) + g_FrameIndex * kConstantStride, &constants, sizeof(constants));
    g_Constants->Unmap(0, nullptr);
    Transition(commandList, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commandList, normal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* velocity = RenderTargets::GetGBufferResource(GBufferType::VELOCITY);
    ID3D12Resource* material = RenderTargets::GetGBufferResource(GBufferType::MATERIAL);
    if (NrdDenoiser::IsCompiled())
    {
        for (auto& guide : frame.Guides)
        {
            Transition(commandList, guide.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        Transition(commandList, frame.AmbientGuide.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(commandList, velocity, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, material, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    ID3D12DescriptorHeap* heaps[] = {g_Heap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_RootSignature.Get());
    const bool instrumented = RayTelemetry::IsCollecting() && EnsureTelemetryPipeline();
    RayTelemetry::Data().RecordProvider(RayTelemetry::ShadowAo, instrumented);
    commandList->SetPipelineState(instrumented ? g_TelemetryPipeline.Get() : g_Pipeline.Get());
    if (instrumented)
    {
        commandList->SetComputeRootUnorderedAccessView(10, RayTelemetry::Data().Address());
    }
    commandList->SetComputeRootShaderResourceView(0, frame.TopLevel->GetGPUVirtualAddress());
    commandList->SetComputeRootDescriptorTable(1, depthSrv);
    commandList->SetComputeRootDescriptorTable(2, normalSrv);
    commandList->SetComputeRootDescriptorTable(3, Gpu(kUavStart + g_FrameIndex));
    commandList->SetComputeRootConstantBufferView(4,
                                                  g_Constants->GetGPUVirtualAddress() + g_FrameIndex * kConstantStride);
    if (NrdDenoiser::IsCompiled())
    {
        commandList->SetComputeRootDescriptorTable(5, Gpu(kGuideUavStart + g_FrameIndex * 5));
        commandList->SetComputeRootDescriptorTable(6, RenderTargets::GetGBufferSrvHandle(GBufferType::VELOCITY));
        commandList->SetComputeRootDescriptorTable(7, RenderTargets::GetGBufferSrvHandle(GBufferType::MATERIAL));
    }
    {
        RenderProfiler::ScopedEvent traceProfile("DXR Shadow / AO Trace", commandList);
        commandList->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
    }
    Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (NrdDenoiser::IsCompiled())
    {
        RenderProfiler::ScopedEvent denoiseProfile("DXR Shadow / AO Denoise and Compose", commandList);
        for (auto& guide : frame.Guides)
        {
            Transition(commandList, guide.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        Transition(commandList, frame.AmbientGuide.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commandList, velocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commandList, material, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const bool denoise = g_Settings.Denoise && UsesRaytracedShadows() && g_SunDirection.w > 0.5f;
        NrdFrame sunFrame = nrdFrame;
        sunFrame.ResetHistory |= !shadowHistoryValid;
        const auto& oldDirection = g_PreviousNrdFrame.LightDirection;
        const float directionSimilarity = oldDirection.x * nrdFrame.LightDirection.x +
                                          oldDirection.y * nrdFrame.LightDirection.y +
                                          oldDirection.z * nrdFrame.LightDirection.z;
        if (directionSimilarity < 0.99f)
        {
            sunFrame.ResetHistory = true;
        }
        if (denoise &&
            g_NrdSun->Execute(
                commandList,
                {{frame.Guides[0].Get()}, {frame.Guides[1].Get()}, {frame.Guides[2].Get()}, {frame.Guides[3].Get()}},
                sunFrame))
        {
            g_NrdDispatchCount = g_NrdSun->GetDispatchCount();
            g_ShadowHistoryValid = true;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_Device->CreateShaderResourceView(g_NrdSun->GetOutput(), &srv, Cpu(kNrdShadowSrvStart + g_FrameIndex));
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetComputeRootSignature(g_RootSignature.Get());
            commandList->SetPipelineState(g_ComposeShadow.Get());
            commandList->SetComputeRootDescriptorTable(1, depthSrv);
            commandList->SetComputeRootDescriptorTable(3, Gpu(kUavStart + g_FrameIndex));
            commandList->SetComputeRootConstantBufferView(4, g_Constants->GetGPUVirtualAddress() +
                                                                 g_FrameIndex * kConstantStride);
            commandList->SetComputeRootDescriptorTable(8, Gpu(kNrdShadowSrvStart + g_FrameIndex));
            Transition(commandList, g_NrdSun->GetOutput(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
            Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(commandList, g_NrdSun->GetOutput(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (g_Serial == 1)
            {
                WriteGraphicsLog("NRD: SIGMA GPU dispatches=%u\n", g_NrdDispatchCount);
            }
        }
        NrdFrame ambientFrame = nrdFrame;
        ambientFrame.ResetHistory |= !ambientHistoryValid;
        if (g_Settings.Denoise && UsesRaytracedAmbientOcclusion() &&
            g_NrdAmbient->Execute(
                commandList,
                {{frame.Guides[0].Get()}, {frame.Guides[1].Get()}, {frame.Guides[2].Get()}, {frame.AmbientGuide.Get()}},
                ambientFrame))
        {
            g_AmbientDispatchCount = g_NrdAmbient->GetDispatchCount();
            g_AmbientHistoryValid = true;
            g_NrdDispatchCount += g_AmbientDispatchCount;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R16_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_Device->CreateShaderResourceView(g_NrdAmbient->GetOutput(), &srv,
                                               Cpu(kNrdAmbientSrvStart + g_FrameIndex));
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetComputeRootSignature(g_RootSignature.Get());
            commandList->SetPipelineState(g_ComposeAmbient.Get());
            commandList->SetComputeRootDescriptorTable(1, depthSrv);
            commandList->SetComputeRootDescriptorTable(3, Gpu(kUavStart + g_FrameIndex));
            commandList->SetComputeRootConstantBufferView(4, g_Constants->GetGPUVirtualAddress() +
                                                                 g_FrameIndex * kConstantStride);
            commandList->SetComputeRootDescriptorTable(9, Gpu(kNrdAmbientSrvStart + g_FrameIndex));
            Transition(commandList, g_NrdAmbient->GetOutput(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
            Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(commandList, g_NrdAmbient->GetOutput(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        commandList->SetDescriptorHeaps(1, heaps);
    }
    Transition(commandList, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(commandList, normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_Active = true;
    updateLighting(true);
    if (g_Settings.Reflections || RendererSettings::GetRealtimeGiEnabled())
    {
        RenderProfiler::ScopedEvent reflectionProfile("DXR Radiance (Reflections / GI)", commandList);
        const bool radianceReady = RayReflections::Execute(commandList, frame.TopLevel.Get(), frame.SceneGeometry,
                                                           nrdFrame, frame.Guides, g_Settings.Denoise);
        g_ReflectionsActive = radianceReady && g_Settings.Reflections;
        g_GiActive = radianceReady && RendererSettings::GetRealtimeGiEnabled() && RayReflections::IsGiActive();
    }
    if (g_ReflectionsActive || g_GiActive)
    {
        g_NrdDispatchCount += RayReflections::GetDenoiserDispatchCount();
    }
    // カメラと動きの履歴はシーンに属し、有効なデノイズ信号の種類には依存しない。
    g_PreviousNrdFrame = nrdFrame;
    g_PreviousTemporalState = temporalState;
    g_NrdHistoryValid = true;
    g_GuideSerial = g_Serial;
    if (g_Serial == 1)
    {
        WriteGraphicsLog("DXR: first dispatch, instances=%u extent=%ux%u\n", g_InstanceCount, frame.Width,
                         frame.Height);
    }
}

bool Raytracing::IsAvailable()
{
    return g_Pipeline != nullptr;
}

bool Raytracing::UsesRaytracedShadows()
{
    if (!IsAvailable() || !g_Settings.Enabled || g_Settings.RenderMode != Mode::Full || !g_Settings.Shadows)
    {
        return false;
    }
    // 負荷削減：全マテリアルの走査はフレームに 1 回だけ（以前は呼ばれるたびに走査しており、
    // 呼び出し元のライトのループと合わせて「影付きライト数 × マテリアル数」になっていた）
    static uint64_t cachedFence = UINT64_MAX;
    static bool cachedResult = true;
    static size_t cachedMaterialCount = 0;
    const uint64_t fence = GraphicsDevice::GetPendingSubmissionFence();
    const size_t materialCount = World::GetView<MaterialComponent>().size();
    if (fence == cachedFence && materialCount == cachedMaterialCount)
    {
        return cachedResult;
    }
    cachedFence = fence;
    cachedMaterialCount = materialCount;
    cachedResult = true;
    // 現在のインラインレイ判定はジオメトリを不透明として扱う。レイ経路でテクスチャのアルファを
    // 評価できるまでは、切り抜きの影にマテリアル対応のラスタパスを使う。
    for (EntityID entity : World::GetView<MaterialComponent>())
    {
        const auto& source = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        const size_t count = max(size_t(1), source.MaterialSlots.size());
        for (size_t slot = 0; slot < count; ++slot)
        {
            const auto& material = MaterialManager::Resolve(source, slot);
            if ((MaterialSystem::ResolveShadowMode(material) == MaterialShadowMode::AlphaTest ||
                 MaterialSystem::ResolveShadowMode(material) == MaterialShadowMode::Transmittance) &&
                MaterialSystem::CastsShadow(material))
            {
                cachedResult = false;
                return false;
            }
        }
    }
    return true;
}

bool Raytracing::UsesRaytracedAmbientOcclusion()
{
    return IsAvailable() && g_Settings.Enabled && g_Settings.RenderMode == Mode::Full && g_Settings.AmbientOcclusion;
}

bool Raytracing::IsActive()
{
    return g_Active;
}

const char* Raytracing::GetStatus()
{
    return g_Status;
}

Raytracing::Settings& Raytracing::GetSettings()
{
    return g_Settings;
}

D3D12_GPU_DESCRIPTOR_HANDLE Raytracing::GetResultSrv()
{
    return Gpu(g_Active ? kSrvStart + g_FrameIndex : kFallbackSrv);
}

XMFLOAT4 Raytracing::GetSunDirection()
{
    return g_SunDirection;
}

Raytracing::BlasStatistics Raytracing::GetBlasStatistics()
{
    return g_BlasStats;
}

UINT Raytracing::GetInstanceCount()
{
    return g_InstanceCount;
}

UINT Raytracing::GetDenoiserDispatchCount()
{
    return g_NrdDispatchCount;
}

bool Raytracing::GetNrdGuides(ID3D12Resource* guides[3], NrdFrame& frame, uint64_t& serial)
{
    if (!guides || !g_Active || !NrdDenoiser::IsCompiled() || g_GuideSerial == 0)
    {
        return false;
    }
    for (UINT index = 0; index < 3; ++index)
    {
        if (!g_SharedGuides[index])
        {
            return false;
        }
        guides[index] = g_SharedGuides[index].Get();
    }
    frame = g_PreviousNrdFrame;
    serial = g_GuideSerial;
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Raytracing::GetSceneAddress()
{
    const auto& frame = g_Frames[g_FrameIndex];
    return g_Active && frame.TopLevel ? frame.TopLevel->GetGPUVirtualAddress() : 0;
}

UINT Raytracing::GetAmbientDenoiserDispatchCount()
{
    return g_AmbientDispatchCount;
}

bool Raytracing::AreReflectionsActive()
{
    return g_ReflectionsActive;
}

D3D12_GPU_DESCRIPTOR_HANDLE Raytracing::GetReflectionSrv()
{
    return g_ReflectionsActive ? RayReflections::GetResultSrv(g_FrameIndex) : GetResultSrv();
}

D3D12_GPU_VIRTUAL_ADDRESS Raytracing::GetLightingConstantsAddress(UINT frameIndex)
{
    return g_LightingConstants
               ? g_LightingConstants->GetGPUVirtualAddress() + (frameIndex % kFrameCount) * kLightingStride
               : 0;
}

void Raytracing::BeginLightList()
{
    g_LightShadowFlags = {};
}

void Raytracing::SetLightShadowFlag(UINT index, bool castShadow)
{
    if (index < g_LightShadowFlags.size())
    {
        g_LightShadowFlags[index].x = static_cast<UINT>(castShadow);
    }
}

void Raytracing::BindGlass(ID3D12GraphicsCommandList* commands)
{
    if (commands && g_Heap)
    {
        commands->SetGraphicsRootDescriptorTable(36,
                                                 Gpu(GlassTableIndex(GraphicsDevice::GetFrameIndex() % kFrameCount)));
    }
}

void Raytracing::BeginGlassRead(ID3D12GraphicsCommandList* commands)
{
    auto& frame = g_Frames[GraphicsDevice::GetFrameIndex() % kFrameCount];
    if (!commands || !g_Active || !frame.GlassReady || frame.GlassReading)
    {
        return;
    }
    frame.GlassRestore.clear();
    frame.GlassSkinnedVertices.clear();
    vector<D3D12_RESOURCE_BARRIER> before;
    map<ID3D12Resource*, D3D12_RESOURCE_STATES> resources;
    for (const auto& g : frame.SceneGeometry)
    {
        if (g.Vertices)
        {
            resources.emplace(g.Vertices.Get(), g.VertexState);
        }
        if (g.Indices)
        {
            resources.emplace(g.Indices.Get(), g.IndexState);
        }
    }
    for (const auto& [resource, originalState] : resources)
    {
        if ((originalState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0)
        {
            continue;
        }
        auto readState = static_cast<D3D12_RESOURCE_STATES>(originalState | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (originalState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            // UAV 書き込みとシェーダー読み取りは同時にできない。このパスが終わるまで、スキニング済み頂点を
            // ラスタライズとガラスの両方から読み取れる状態に保つ。
            readState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            frame.GlassSkinnedVertices.insert(resource);
        }
        before.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource, originalState, readState));
        frame.GlassRestore.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource, readState, originalState));
    }
    if (!before.empty())
    {
        commands->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
    }
    frame.GlassReading = true;
}

bool Raytracing::IsReadingSkinnedVerticesForGlass(ID3D12Resource* resource)
{
    const auto& frame = g_Frames[GraphicsDevice::GetFrameIndex() % kFrameCount];
    return frame.GlassReading && frame.GlassSkinnedVertices.contains(resource);
}

void Raytracing::EndGlassRead(ID3D12GraphicsCommandList* commands)
{
    auto& frame = g_Frames[GraphicsDevice::GetFrameIndex() % kFrameCount];
    if (!commands || !frame.GlassReading)
    {
        return;
    }
    if (!frame.GlassRestore.empty())
    {
        commands->ResourceBarrier(static_cast<UINT>(frame.GlassRestore.size()), frame.GlassRestore.data());
    }
    frame.GlassRestore.clear();
    frame.GlassSkinnedVertices.clear();
    frame.GlassReading = false;
}

void Raytracing::BindLighting(ID3D12GraphicsCommandList* commands, UINT sceneIndex, UINT constantsIndex)
{
    if (!commands || !g_LightingConstants)
    {
        return;
    }
    const UINT slot = GraphicsDevice::GetFrameIndex() % kFrameCount;
    commands->SetGraphicsRootShaderResourceView(
        sceneIndex, g_Frames[slot].TopLevel ? g_Frames[slot].TopLevel->GetGPUVirtualAddress() : 0);
    commands->SetGraphicsRootConstantBufferView(constantsIndex,
                                                g_LightingConstants->GetGPUVirtualAddress() + slot * kLightingStride);
}

ID3D12Resource* Raytracing::GetResultResource()
{
    if (!g_Active)
    {
        return nullptr;
    }
    return g_Frames[g_FrameIndex].Result.Get();
}

bool Raytracing::IsGiActive()
{
    return g_GiActive;
}

D3D12_GPU_DESCRIPTOR_HANDLE Raytracing::GetGiSrv()
{
    return RayReflections::GetGiSrv(g_FrameIndex);
}
