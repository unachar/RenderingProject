#pragma once
// =============================================================================================
// MegaLights
//   M1：ライトバッファ + クラスター（フロクセル）ライトグリッド（ML0 / ML1）
//   M2：確率的ライトサンプリング（ML3）+ 影レイ（ML5）。シェーディング（ML6）は DeferredLightingPS
//       ・検証用：リファレンスモード（全ライト × 面上の複数点の影レイ）、静止時の累積平均、RMSE
//   M3：デノイズ（ML8）。MegaLightsShadePS が ML6 の結果をデモジュレートして書き、
//       時間方向（リプロジェクト + モーメント）→ À-Trous（分散で輝度の許容を決める）→ ディファードで掛け戻す
//   M4：可視ライトハッシュによるガイド（ML7）。8x8 タイルごとに「前フレームに影レイが通ったライト」の
//       128bit ブルームフィルタを持ち、載っていないライトの選択確率を下げる（確率に入れるので不偏のまま）
//   M5：タイル分類（ML2）+ コンパクション + スクリーントレース（ML4）
//       サンプリングは対象タイルだけを間接ディスパッチ、影レイは「必要なサンプルだけ」を詰めたリストで間接ディスパッチ。
//       ワールドトレースの前に深度バッファを短くマーチし、遮蔽が見つかったサンプルはレイを飛ばさない
//   M6：NRD（REBLUR_DIFFUSE）でのデノイズ（自前の SVGF と切り替え）、フロクセルフォグの MegaLights 化
//       フォグはグリッドを「視錐台の手前側も含めて」構築し、フロクセルごとにセルのライトから確率的に選ぶ（上限 32
//       灯を撤廃）
//   M7：Forward / 半透明（OIT 含む）からのグリッド参照、テクスチャ付き矩形ライト
//   半解像度サンプリング：サンプル空間を画面の 1/2 にし、各サンプルは 2x2 の代表ピクセル（毎フレーム回転）で作る。
//       ディファードは近傍 2x2 のサンプルを深度・法線で重み付けして使う（近似）
//   設計書: MegaLights_Design.md / MegaLights_M2_Design.md
//   ・ローカルライト（Point / Spot / Rect）を StructuredBuffer に載せる（最大 4096 灯）
//   ・16px タイル × 32 スライスのセルごとにライトリストを GPU で構築
//   ・シャドウマップを持つライト / ディレクショナル / Decal / Volume は従来の経路のまま
//   ヘッダーのみ（.vcxproj への追加不要）。シェーダーは実行時に DXC でコンパイルする。
// =============================================================================================
#include "pch.h"
#include "graphicslog.h"
#include "rendererstate.h"
#include "renderprofiler.h"
#include "raytracing.h"
#include "nrddenoiser.h"
#include <memory>
#include "camera.h"
#include <cmath>
#include <algorithm>

class MegaLights final
{
  public:
    enum class Mode : int
    {
        Deterministic = 0, // M1：セル内の全ライトを評価
        Stochastic = 1,    // M2：N 個のサンプル + 影レイ（PBR 面のみ。DXR が必要）
        Reference = 2,     // 検証：全ライト × 面上の複数点の影レイ（PBR 面のみ。DXR が必要）
    };

    struct Settings
    {
        bool Enabled = false; // ディファード時のローカルライトをクラスターグリッドで評価する
        int DebugView = 0;    // 0: なし, 1: セルのライト数, 2: 奥行きスライス, 3: 選ばれたライト, 4: 可視率, 5: 1/p, 6:
                              // 累積平均, 7: リファレンスとの差
        float SliceFar = 500.0f; // スライス分割の奥行き上限（m）。これより奥は最後のスライスにまとめる
        int HeatmapMax = 32;     // ライト数ヒートマップの上限
        // ---- M2
        int EvaluationMode = static_cast<int>(Mode::Deterministic);
        int SamplesPerPixel = 2;     // 1 / 2 / 4（解像度に応じて自動で下げる）
        bool HalfResolution = false; // 半解像度サンプリング（サンプル数・影レイが 1/4）
        // ---- M7
        bool Translucency = true; // Forward / 半透明もグリッドで評価する（グリッドを視錐台全体で構築）
        float UniformMix = 0.1f;  // 一様選択の混合率 α（見積もりが外れても確率を 0 にしない）
        int ReferenceRays = 16;   // リファレンスの影レイ数（面光源・球ライト）
        float InvPdfDisplayMax = 64.0f;
        bool Accumulate = false; // 静止時の累積平均（検証用）
        // ---- M4（可視ライトハッシュ）
        bool Guide = true;
        float GuideMinWeight =
            0.1f;                 // ハッシュに載っていないライトの重み（0 より大きく。遮られがちなライトも時々調べる）
        float GuideKeep = 0.875f; // 前フレームのハッシュのビットを残す確率（1 - 減衰）
        // ---- M5（スクリーントレース）
        // ---- M6
        int Denoiser = 0;          // 0: 自前（SVGF）, 1: NRD（REBLUR）。NRD が使えなければ自前に戻す
        bool VolumetricFog = true; // フロクセルフォグのローカルライトを MegaLights で評価する
        int FogSamples = 2;        // フロクセルあたりのライトサンプル数（1〜4）
        bool ScreenTrace = true;
        int ScreenTraceSteps = 12;
        float ScreenTraceMaxPixels = 64.0f; // マーチする最大距離（ピクセル）
        float ScreenTraceThickness = 0.25f; // 深度バッファの面の厚みの仮定（m）
        float ScreenTraceSelfPixels = 1.5f; // 始点からこのピクセル数以内は調べない（自己遮蔽よけ）
        // ---- M3（デノイズ）
        bool Denoise = true;
        int DenoiseIterations = 3; // À-Trous の回数（間隔 1, 2, 4, ...）
        int MaxHistory = 32;       // 時間方向の履歴の上限（フレーム）
        int AntiLagHistory = 4;    // ライトが変わったフレームの履歴の上限
        bool FireflySuppression = true;
        bool HistoryFeedback = true; // À-Trous の 1 回目を履歴へ書き戻す
        float AlphaMin = 0.05f;
        float MomentsAlphaMin = 0.2f;
        float DepthTolerance = 0.1f;
        float NormalThreshold = 0.9f;
        float PhiNormal = 64.0f;
        float PhiLuminance = 4.0f;
        float PhiPlane = 2.0f;
    };

    struct Stats
    {
        UINT SubmittedLights = 0; // グリッドに載せたライト
        UINT DroppedLights = 0;   // 上限（4096）を超えて載せられなかったライト
        UINT TilesX = 0, TilesY = 0, Slices = 0;
        UINT CoarseIndices = 0, FineIndices = 0; // 数フレーム前の GPU 実測
        UINT OverflowCells = 0, MaxLightsPerCell = 0;
        bool Built = false;
        // ---- M2
        Mode ActiveMode = Mode::Deterministic; // このフレームに実際に使った評価方式
        const char* FallbackReason = nullptr;  // 要求した方式が使えなかった理由
        UINT SamplesPerPixel = 0;
        double SampleBufferMB = 0.0;
        UINT AccumulatedFrames = 0;
        bool HasReference = false;
        bool ErrorValid = false;
        double RelativeRmse = 0.0; // sqrt(Σ(acc - ref)² / Σref²)（輝度）
        UINT ErrorFrames = 0;      // RMSE を測ったときの累積フレーム数
        // ---- M3
        bool Denoised = false;
        bool AntiLag = false;
        bool HistoryReset = false;
        double DenoiseBufferMB = 0.0;
        const char* DenoiseFallback = nullptr;
        // ---- M4
        bool Guided = false;    // このフレームのサンプリングでハッシュを使った
        bool HashValid = false; // 前フレームのハッシュが使える
        UINT HashTilesX = 0, HashTilesY = 0;
        // ---- M5（数フレーム前の GPU 実測）
        UINT ActiveTiles = 0, TotalTiles = 0;
        UINT TraceRequests = 0; // 影レイが必要だったサンプル
        UINT ScreenHits = 0;    // スクリーントレースで遮蔽が見つかった（ワールドトレース不要）
        UINT WorldTraces = 0;   // ワールドトレースしたサンプル
        bool TraceStatsValid = false;
        // ---- M6
        const char* DenoiserUsed = nullptr; // このフレームに使ったデノイザー
        const char* NrdFallback = nullptr;
        bool VolumetricGrid = false; // フォグ用にグリッドを視錐台全体で構築した
        // ---- M7 / 半解像度
        bool ForwardGrid = false; // Forward / 半透明がグリッドを使う
        UINT SampleWidth = 0, SampleHeight = 0, SampleScale = 1;
    };

    // HLSL の MegaLightData と同じ並び（80 byte）
    struct GpuLight
    {
        XMFLOAT4 PositionType;
        XMFLOAT4 DirectionRange;
        XMFLOAT4 Color;
        XMFLOAT4 Extra;
        XMFLOAT4 Flags; // x: 影を落とす, y: ソース半径, z: Entity ID（uint のビット。可視ライトハッシュのキー）
    };

    static_assert(sizeof(GpuLight) == 80, "MegaLightData layout mismatch");

    static constexpr UINT kMaxLights = 4096;
    static constexpr UINT kTileSize = 16;
    static constexpr UINT kSlices = 32;
    static constexpr UINT kCoarseTiles = 4;
    static constexpr UINT kCoarseSlices = 4;
    static constexpr UINT kCoarseCapacity = 1u << 20; // 粗リスト（uint）: 4 MB
    static constexpr UINT kFineCapacity = 1u << 22;   // 細リスト（uint）: 16 MB
    static constexpr UINT kMaxSamplesPerPixel = 4;
    static constexpr UINT64 kSampleBudgetBytes = 160ull << 20; // サンプルバッファの上限（N を自動で下げる）
    static constexpr UINT kFrames = RendererState::g_kFRAME_COUNT;

    static Settings& GetSettings()
    {
        return s_Settings;
    }

    static const Stats& GetStats()
    {
        return s_Stats;
    }

    static bool IsAvailable()
    {
        return s_Available;
    }

    static bool IsStochasticAvailable()
    {
        return s_Available && s_SampleAvailable;
    }

    // CPU 側でライトをグリッドへ振り分けるか（このフレームの判断）
    static bool IsEnabled()
    {
        return s_Available && s_Settings.Enabled;
    }

    // 検証の操作（UI から）
    static void RequestAccumulationReset()
    {
        s_ResetRequested = true;
    }

    static void RequestSaveReference()
    {
        s_SaveReferenceRequested = true;
    }

    static void ClearReference()
    {
        s_HasReference = false;
        s_Stats.HasReference = false;
        s_Stats.ErrorValid = false;
    }

    static void RequestHistoryReset()
    {
        s_DenoiseResetRequested = true;
        s_HashResetRequested = true;
    }

    static void RequestHashReset()
    {
        s_HashResetRequested = true;
    }

    static bool IsDenoiseAvailable()
    {
        return s_Available && s_DenoiseAvailable && !s_ShadeFailed;
    }

    static bool Initialize(ID3D12Device* device)
    {
        Shutdown();
        if (!device)
        {
            return false;
        }
        s_Device = device;

        // 定数バッファは最初に作る（グラフィックス側は常にこれを b10 にバインドするため、
        // シェーダーのコンパイルに失敗しても「無効」の定数を読ませられるようにする）
        if (!CreateBuffer(s_Constants, kConstantStride * kFrames, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_GENERIC_READ, L"MegaLights Constants") ||
            !CreateBuffer(s_LightUpload, kLightStride * kFrames, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_GENERIC_READ, L"MegaLights Lights") ||
            !CreateBuffer(s_Dummy, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COMMON, L"MegaLights Dummy UAV"))
        {
            WriteGraphicsLog("MegaLights: upload buffers failed\n");
            Shutdown();
            return false;
        }
        const CD3DX12_RANGE noRead(0, 0);
        if (FAILED(s_Constants->Map(0, &noRead, reinterpret_cast<void**>(&s_MappedConstants))) ||
            FAILED(s_LightUpload->Map(0, &noRead, reinterpret_cast<void**>(&s_MappedLights))))
        {
            Shutdown();
            return false;
        }
        memset(s_MappedConstants, 0, kConstantStride * kFrames);

        if (!CreateGridPipelines(device))
        {
            return false;
        }

        const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!CreateBuffer(s_CoarseIndices, sizeof(UINT) * kCoarseCapacity, D3D12_HEAP_TYPE_DEFAULT, uav,
                          D3D12_RESOURCE_STATE_COMMON, L"MegaLights Coarse Indices") ||
            !CreateBuffer(s_FineIndices, sizeof(UINT) * kFineCapacity, D3D12_HEAP_TYPE_DEFAULT, uav,
                          D3D12_RESOURCE_STATE_COMMON, L"MegaLights Fine Indices") ||
            !CreateBuffer(s_Counters, 16, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                          L"MegaLights Counters") ||
            !CreateBuffer(s_LightVisible, kLightVisibleBytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                          L"MegaLights Light Visibility") ||
            !CreateBuffer(s_Readback, kReadbackStride * kFrames, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, L"MegaLights Readback"))
        {
            WriteGraphicsLog("MegaLights: grid buffers failed\n");
            ShutdownPipelines();
            return false;
        }
        s_CoarseIndicesState = s_FineIndicesState = s_CountersState = D3D12_RESOURCE_STATE_COMMON;
        s_Available = true;
        // M2 / M3 は失敗しても M1 は使える
        s_SampleAvailable = CreateSamplePipelines(device);
        s_DenoiseAvailable = s_SampleAvailable && CreateDenoisePipelines(device);
        s_NrdAvailable = s_DenoiseAvailable && NrdDenoiser::IsCompiled() && CreateNrdPipelines(device);
        WriteGraphicsLog("MegaLights: initialized (clustered light grid, max %u lights, stochastic=%d)\n", kMaxLights,
                         s_SampleAvailable ? 1 : 0);
        return true;
    }

    static void Shutdown()
    {
        ShutdownPipelines();
        if (s_Constants && s_MappedConstants)
        {
            s_Constants->Unmap(0, nullptr);
        }
        if (s_LightUpload && s_MappedLights)
        {
            s_LightUpload->Unmap(0, nullptr);
        }
        s_MappedConstants = nullptr;
        s_MappedLights = nullptr;
        s_Constants.Reset();
        s_LightUpload.Reset();
        s_Dummy.Reset();
        s_ShadePso.Reset();
        s_ShadeFailed = false;
        s_Device.Reset();
    }

    // ---- CPU：ライトの収集（LightingResources::UpdateLightConstantBuffer から 1 フレーム 1 回）
    // 戻り値 false の場合、このフレームはすでに収集済み（同じフレームで再度呼ばれた）
    static bool BeginFrame(UINT frameIndex, uint64_t frameSerial)
    {
        if (frameSerial == s_LastSerial && s_HasSerial)
        {
            return false;
        }
        s_HasSerial = true;
        s_LastSerial = frameSerial;
        s_Slot = frameIndex % kFrames;
        ++s_FrameCounter;
        RetireTick();
        ReadStats();
        s_LightCount = 0;
        s_LightHash = 1469598103934665603ull;
        for (EntityID entity : s_EntityMarked)
        {
            if (entity < s_EntityInGrid.size())
            {
                s_EntityInGrid[entity] = 0;
            }
        }
        s_EntityMarked.clear();
        s_Stats.SubmittedLights = 0;
        s_Stats.DroppedLights = 0;
        s_Stats.Built = false;
        s_SlotBuilt[s_Slot] = false;
        s_SlotForward[s_Slot] = false;
        s_SampleWidth = s_SampleHeight = 0;
        s_SampleScale = 1;
        WriteConstants(false, {}, 0, 0, 0, 0, 0, 0);
        WriteForwardDescriptors(s_Slot, false);
        return true;
    }

    // 追加できたら true（このライトの不透明面ライティングはグリッドが受け持つ）
    // ライトのオクルージョン（GPU の判定を読み戻した結果）：数フレーム続けて「見えている不透明面に届かない」ライト
    static bool IsLightOccluded(EntityID entity)
    {
        return entity < s_OccludedFrames.size() && s_OccludedFrames[entity] >= kOcclusionFrames;
    }

    static UINT GetOccludedLightCount()
    {
        return s_OccludedCount;
    }

    static bool IsEntityInGrid(EntityID entity)
    {
        return s_HasSerial && entity < s_EntityInGrid.size() && s_EntityInGrid[entity] != 0;
    }

    static bool AddLight(EntityID entity, const GpuLight& light)
    {
        if (!s_MappedLights || !IsEnabled())
        {
            return false;
        }
        if (s_LightCount >= kMaxLights)
        {
            ++s_Stats.DroppedLights;
            return false;
        }
        memcpy(s_MappedLights + s_Slot * kLightStride + s_LightCount * sizeof(GpuLight), &light, sizeof(GpuLight));
        HashBytes(s_LightHash, &light, sizeof(light)); // 累積のリセット判定用（アップロードヒープは読み戻さない）
        if (entity >= s_EntityInGrid.size())
        {
            s_EntityInGrid.resize(static_cast<size_t>(entity) + 1, 0);
        }
        s_EntityInGrid[entity] = 1;
        s_EntityMarked.push_back(entity);
        ++s_LightCount;
        s_Stats.SubmittedLights = s_LightCount;
        return true;
    }

    // ---- GPU：グリッドの構築 + M2 のサンプリング（GBuffer 完成後・TLAS 構築後・ディファードライティングの前）
    //   gbuffer: BaseColor, Normal, Depth, Material, Shadow, Velocity（PIXEL_SHADER_RESOURCE
    //   で渡す。終了時に同じ状態へ戻す） gbufferTable: ポストプロセスと同じ GBuffer の SRV テーブル（BaseColor から 8
    //   個連続。Velocity は 8 番目）
    static constexpr UINT kBuildGBufferCount = 6;

    static bool Build(ID3D12GraphicsCommandList* commands, ID3D12DescriptorHeap* heap,
                      ID3D12Resource* const gbuffer[kBuildGBufferCount], D3D12_GPU_DESCRIPTOR_HANDLE gbufferTable,
                      D3D12_GPU_DESCRIPTOR_HANDLE depthSrv, bool froxelFogActive = false)
    {
        s_Stats.VolumetricGrid = false;
        s_Stats.Built = false;
        s_Stats.ActiveMode = Mode::Deterministic;
        s_Stats.FallbackReason = nullptr;
        ID3D12Resource* depth = gbuffer ? gbuffer[2] : nullptr;
        if (!commands || !heap || !depth || !IsEnabled() || !s_HasSerial || s_SlotBuilt[s_Slot])
        {
            return false;
        }
        const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
        const UINT width = static_cast<UINT>(depthDesc.Width);
        const UINT height = depthDesc.Height;
        if (width == 0 || height == 0)
        {
            return false;
        }
        const UINT tilesX = (width + kTileSize - 1) / kTileSize;
        const UINT tilesY = (height + kTileSize - 1) / kTileSize;
        const UINT coarseX = (tilesX + kCoarseTiles - 1) / kCoarseTiles;
        const UINT coarseY = (tilesY + kCoarseTiles - 1) / kCoarseTiles;
        const UINT coarseZ = kSlices / kCoarseSlices;
        if (!EnsureGrid(tilesX, tilesY, coarseX * coarseY * coarseZ))
        {
            return false;
        }

        // ---- M2：このフレームの評価方式を決める
        const Mode requested = static_cast<Mode>(std::clamp(s_Settings.EvaluationMode, 0, 2));
        const D3D12_GPU_VIRTUAL_ADDRESS scene = Raytracing::GetSceneAddress();
        Mode mode = requested;
        UINT samplesPerPixel = 0;
        if (requested != Mode::Deterministic)
        {
            if (!s_SampleAvailable)
            {
                mode = Mode::Deterministic;
                s_Stats.FallbackReason = "M2 シェーダーの初期化に失敗";
            }
            else if (scene == 0)
            {
                mode = Mode::Deterministic;
                s_Stats.FallbackReason = "レイトレーシングが無効（TLAS なし）";
            }
        }
        if (mode == Mode::Stochastic)
        {
            // 半解像度：サンプル空間は画面の 1/2（切り上げ）
            s_SampleScale = s_Settings.HalfResolution ? 2u : 1u;
            s_SampleWidth = (width + s_SampleScale - 1) / s_SampleScale;
            s_SampleHeight = (height + s_SampleScale - 1) / s_SampleScale;
            samplesPerPixel = ChooseSamplesPerPixel(s_SampleWidth, s_SampleHeight);
            if (samplesPerPixel == 0 || !EnsureSamples(s_SampleWidth, s_SampleHeight, samplesPerPixel))
            {
                mode = Mode::Deterministic;
                s_Stats.FallbackReason = "サンプルバッファを確保できません";
                samplesPerPixel = 0;
            }
        }
        // ---- M3：デノイズするか
        bool denoise = false;
        s_Stats.DenoiseFallback = nullptr;
        if (mode == Mode::Stochastic && s_Settings.Denoise)
        {
            if (!s_DenoiseAvailable || s_ShadeFailed)
            {
                s_Stats.DenoiseFallback = "デノイズのシェーダーを初期化できません";
            }
            else if (!EnsureDenoise(width, height))
            {
                s_Stats.DenoiseFallback = "デノイズのバッファを確保できません";
            }
            else
            {
                denoise = true;
            }
        }
        // ---- M4：可視ライトハッシュ（前フレームの分を読み、今フレームの分を作る）
        const UINT hashTilesX = (width + 7) / 8, hashTilesY = (height + 7) / 8;
        bool guide = false;
        bool hashBuild = false;
        if (mode == Mode::Stochastic && s_Settings.Guide && s_HashAvailable && EnsureHash(hashTilesX, hashTilesY))
        {
            hashBuild = true;
            const EntityID camera = Camera::GetCameraEntity();
            // ハッシュはライトの Entity ID で引く（インデックスはフレームごとに並びが変わるため）
            guide =
                s_HashValid && s_HashLastFrame + 1 == s_FrameCounter && camera == s_HashCamera && !s_HashResetRequested;
            s_HashCamera = camera;
        }
        s_HashResetRequested = false;
        s_Stats.HashValid = guide;
        s_Stats.Guided = guide;
        s_Stats.HashTilesX = hashTilesX;
        s_Stats.HashTilesY = hashTilesY;
        s_Stats.AntiLag = s_LightHash != s_PreviousLightHash;
        s_PreviousLightHash = s_LightHash;
        const bool accumulate = s_Settings.Accumulate && s_SampleAvailable && EnsureAccumulation(width, height);
        if (s_HasReference && (s_ReferenceWidth != width || s_ReferenceHeight != height))
        {
            ClearReference();
        }
        UpdateAccumulationState(width, height, mode, samplesPerPixel, accumulate);

        RenderProfiler::ScopedEvent profile("MegaLights", commands);
        // M6 / M7：フォグか Forward（半透明）がグリッドを使うなら、ジオメトリの手前のスライスもすべて埋める
        const bool forwardGrid = s_Settings.Translucency;
        const bool volumetric = (froxelFogActive && s_Settings.VolumetricFog) || forwardGrid;
        WriteConstants(true, depthDesc, tilesX, tilesY, coarseX, coarseY, samplesPerPixel,
                       BuildFlags(mode, accumulate, denoise, guide));
        if (volumetric && s_MappedConstants)
        {
            Constants* written = reinterpret_cast<Constants*>(s_MappedConstants + s_Slot * kConstantStride);
            written->Grid.w |= kGridVolumetric;
            if (forwardGrid)
            {
                written->Grid.w |= kGridForward;
            }
        }
        const UINT slot = s_Slot;

        // GBuffer：ピクセルシェーダーリソース → コンピュートでも読める状態に
        const D3D12_RESOURCE_STATES gbufferRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const D3D12_RESOURCE_STATES bothRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const bool sampling = mode == Mode::Stochastic;
        vector<D3D12_RESOURCE_BARRIER> barriers;
        for (UINT index = 0; index < kBuildGBufferCount; ++index)
        {
            if (!gbuffer[index] || (!sampling && index != 2))
            {
                continue;
            }
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(gbuffer[index], gbufferRead, bothRead));
        }
        Transition(barriers, s_TileMasks.Get(), s_TileMasksState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_CoarseCells.Get(), s_CoarseCellsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_CoarseIndices.Get(), s_CoarseIndicesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_FineCells.Get(), s_FineCellsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_FineIndices.Get(), s_FineIndicesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_Counters.Get(), s_CountersState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_LightVisible.Get(), s_LightVisibleState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

        ID3D12DescriptorHeap* heaps[] = {heap};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(s_Root.Get());
        commands->SetComputeRootConstantBufferView(0, s_Constants->GetGPUVirtualAddress() + slot * kConstantStride);
        commands->SetComputeRootShaderResourceView(1, s_LightUpload->GetGPUVirtualAddress() + slot * kLightStride);
        commands->SetComputeRootDescriptorTable(2, depthSrv);
        commands->SetComputeRootUnorderedAccessView(3, s_TileMasks->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(4, s_CoarseCells->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(5, s_CoarseIndices->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(6, s_FineCells->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(7, s_FineIndices->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(8, s_Counters->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(9, s_LightVisible->GetGPUVirtualAddress());

        commands->SetPipelineState(s_ClearCounters.Get());
        commands->Dispatch(1, 1, 1);
        commands->SetPipelineState(s_TileSliceMask.Get());
        commands->Dispatch(tilesX, tilesY, 1);
        UavBarrier(commands, {s_Counters.Get(), s_TileMasks.Get(), s_LightVisible.Get()});
        commands->SetPipelineState(s_CoarseCull.Get());
        commands->Dispatch(coarseX, coarseY, coarseZ);
        UavBarrier(commands, {s_Counters.Get(), s_CoarseCells.Get(), s_CoarseIndices.Get()});
        commands->SetPipelineState(s_FineCull.Get());
        commands->Dispatch(coarseX, coarseY, coarseZ);

        // グリッドはコンピュート（M2）とピクセルシェーダーの両方で読む
        barriers.clear();
        Transition(barriers, s_FineCells.Get(), s_FineCellsState, bothRead);
        Transition(barriers, s_FineIndices.Get(), s_FineIndicesState, bothRead);
        Transition(barriers, s_Counters.Get(), s_CountersState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(barriers, s_LightVisible.Get(), s_LightVisibleState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (accumulate)
        {
            Transition(barriers, s_Accumulation.Get(), s_AccumulationState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        commands->CopyBufferRegion(s_Readback.Get(), slot * kReadbackStride, s_Counters.Get(), 0, 16);
        commands->CopyBufferRegion(s_Readback.Get(), slot * kReadbackStride + 16, s_LightVisible.Get(), 0,
                                   kLightVisibleBytes);
        s_SlotLights[slot] = s_EntityMarked; // 読み戻し時にライトの番号 → Entity を引くため
        s_ReadbackPending[slot] = true;

        // ---- M2：ML3 ライトの選択 → ML5 影レイ
        if (sampling)
        {
            RenderProfiler::ScopedEvent sampleProfile("MegaLights Sample + Trace", commands);
            const UINT hashCurrent = s_HashIndex, hashPrevious = s_HashIndex ^ 1u;
            barriers.clear();
            Transition(barriers, s_Samples.Get(), s_SamplesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(barriers, s_TileList.Get(), s_TileListState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(barriers, s_TraceList.Get(), s_TraceListState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(barriers, s_WorldList.Get(), s_WorldListState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(barriers, s_TraceCounters.Get(), s_TraceCountersState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (hashBuild)
            {
                Transition(barriers, s_Hash[hashPrevious].Get(), s_HashState[hashPrevious],
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Transition(barriers, s_Hash[hashCurrent].Get(), s_HashState[hashCurrent],
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (!barriers.empty())
            {
                commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            }
            BindSampleRoot(commands, slot, scene, gbufferTable, hashBuild ? s_Hash[hashPrevious].Get() : nullptr,
                           hashBuild ? s_Hash[hashCurrent].Get() : nullptr);
            const UINT groupsX = (width + 7) / 8, groupsY = (height + 7) / 8; // 画面の 8x8（ハッシュ）
            const UINT sampleGroupsX = (s_SampleWidth + 7) / 8,
                       sampleGroupsY = (s_SampleHeight + 7) / 8; // サンプル空間の 8x8
            // M5：タイル分類 → 対象タイルだけサンプリング → 影レイが必要なサンプルだけスクリーントレース →
            // 残りをワールドトレース
            auto indirect = [&](ID3D12PipelineState* argsPso, UINT64 argsOffset, ID3D12PipelineState* pso,
                                std::initializer_list<ID3D12Resource*> afterward) {
                vector<D3D12_RESOURCE_BARRIER> list;
                Transition(list, s_DispatchArgs.Get(), s_DispatchArgsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (!list.empty())
                {
                    commands->ResourceBarrier(static_cast<UINT>(list.size()), list.data());
                }
                commands->SetPipelineState(argsPso);
                commands->Dispatch(1, 1, 1);
                list.clear();
                Transition(list, s_DispatchArgs.Get(), s_DispatchArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                commands->ResourceBarrier(static_cast<UINT>(list.size()), list.data());
                commands->SetPipelineState(pso);
                commands->ExecuteIndirect(s_DispatchSignature.Get(), 1, s_DispatchArgs.Get(), argsOffset, nullptr, 0);
                UavBarrier(commands, afterward);
            };
            commands->SetPipelineState(s_ClearTraceCounters.Get());
            commands->Dispatch(1, 1, 1);
            UavBarrier(commands, {s_TraceCounters.Get()});
            commands->SetPipelineState(s_ClassifyTiles.Get());
            commands->Dispatch(sampleGroupsX, sampleGroupsY, 1);
            UavBarrier(commands, {s_TraceCounters.Get(), s_TileList.Get(), s_Samples.Get()});
            indirect(s_ArgsTiles.Get(), 0, s_SampleLights.Get(),
                     {s_Samples.Get(), s_TraceList.Get(), s_TraceCounters.Get()});
            indirect(s_ArgsTrace.Get(), 12, s_ScreenTrace.Get(),
                     {s_Samples.Get(), s_WorldList.Get(), s_TraceCounters.Get()});
            indirect(s_ArgsWorld.Get(), 24, s_WorldTrace.Get(), {s_Samples.Get()});
            {
                vector<D3D12_RESOURCE_BARRIER> list;
                Transition(list, s_TraceCounters.Get(), s_TraceCountersState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                commands->ResourceBarrier(static_cast<UINT>(list.size()), list.data());
                commands->CopyBufferRegion(s_TraceReadback.Get(), slot * 16, s_TraceCounters.Get(), 0, 16);
                s_TraceReadbackPending[slot] = true;
                s_TraceTotalTiles[slot] = sampleGroupsX * sampleGroupsY;
                list.clear();
                Transition(list, s_TraceCounters.Get(), s_TraceCountersState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commands->ResourceBarrier(static_cast<UINT>(list.size()), list.data());
            }
            if (hashBuild)
            {
                // ML7：影レイが通ったライトを 8x8 タイルのハッシュへ（前フレームの分は確率的に減衰させて引き継ぐ）
                UavBarrier(commands, {s_Samples.Get()});
                commands->SetPipelineState(s_BuildHash.Get());
                commands->Dispatch(groupsX, groupsY, 1);
                barriers.clear();
                Transition(barriers, s_Hash[hashCurrent].Get(), s_HashState[hashCurrent],
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                s_HashDisplay = s_Hash[hashCurrent].Get();
                s_HashIndex = hashPrevious;
                s_HashValid = true;
                s_HashLastFrame = s_FrameCounter;
            }
            barriers.clear();
            Transition(barriers, s_Samples.Get(), s_SamplesState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

        // ---- 検証：リファレンスの保存と誤差の計測（累積は前フレームまでの平均）
        if (accumulate)
        {
            RunVerification(commands, slot, scene, gbufferTable, width, height);
        }

        // GBuffer を元の状態へ
        barriers.clear();
        for (UINT index = 0; index < kBuildGBufferCount; ++index)
        {
            if (!gbuffer[index] || (!sampling && index != 2))
            {
                continue;
            }
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(gbuffer[index], bothRead, gbufferRead));
        }
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

        s_SlotBuilt[slot] = true;
        s_SlotSampled[slot] = sampling;
        s_SlotDenoise[slot] = denoise;
        s_SlotHash[slot] = sampling && hashBuild;
        s_SlotVolumetric[slot] = volumetric;
        s_SlotForward[slot] = forwardGrid;
        s_Stats.VolumetricGrid = volumetric && froxelFogActive && s_Settings.VolumetricFog;
        s_Stats.ForwardGrid = forwardGrid;
        s_Stats.SampleWidth = sampling ? s_SampleWidth : 0;
        s_Stats.SampleHeight = sampling ? s_SampleHeight : 0;
        s_Stats.SampleScale = s_SampleScale;
        WriteForwardDescriptors(slot, true);
        s_Stats.Denoised = denoise;
        s_Width = width;
        s_Height = height;
        s_Stats.Built = true;
        s_Stats.TilesX = tilesX;
        s_Stats.TilesY = tilesY;
        s_Stats.Slices = kSlices;
        s_Stats.ActiveMode = mode;
        s_Stats.SamplesPerPixel = samplesPerPixel;
        s_Stats.AccumulatedFrames = accumulate ? s_AccumulatedFrames + 1 : 0;
        if (accumulate && s_AccumulatedFrames < (1u << 24))
        {
            ++s_AccumulatedFrames;
        }
        return true;
    }

    // ---- グラフィックス（ポストプロセスのルートシグネチャ 28〜34）
    // 構築していないフレームでも、必ず有効なアドレスを入れる（定数の「有効」フラグは 0）
    static void BindGraphics(ID3D12GraphicsCommandList* commands, UINT lightsParameter, UINT cellsParameter,
                             UINT indicesParameter, UINT constantsParameter, UINT samplesParameter,
                             UINT accumulationParameter, UINT referenceParameter, UINT rawParameter,
                             UINT denoisedParameter, UINT hashParameter)
    {
        if (!commands || !s_Constants || !s_LightUpload)
        {
            return;
        }
        const UINT slot = s_Slot;
        const D3D12_GPU_VIRTUAL_ADDRESS lights = s_LightUpload->GetGPUVirtualAddress() + slot * kLightStride;
        const D3D12_RESOURCE_STATES bothRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const bool built = s_SlotBuilt[slot] && s_FineCells && s_FineIndices && s_FineCellsState == bothRead;
        const bool sampled =
            built && s_SlotSampled[slot] && s_Samples && s_SamplesState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const bool accumulation =
            built && s_Accumulation && s_AccumulationState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const bool reference = s_Reference && s_ReferenceState == bothRead;
        commands->SetGraphicsRootShaderResourceView(lightsParameter, lights);
        commands->SetGraphicsRootShaderResourceView(cellsParameter,
                                                    built ? s_FineCells->GetGPUVirtualAddress() : lights);
        commands->SetGraphicsRootShaderResourceView(indicesParameter,
                                                    built ? s_FineIndices->GetGPUVirtualAddress() : lights);
        commands->SetGraphicsRootConstantBufferView(constantsParameter,
                                                    s_Constants->GetGPUVirtualAddress() + slot * kConstantStride);
        commands->SetGraphicsRootShaderResourceView(samplesParameter,
                                                    sampled ? s_Samples->GetGPUVirtualAddress() : lights);
        commands->SetGraphicsRootUnorderedAccessView(accumulationParameter, accumulation
                                                                                ? s_Accumulation->GetGPUVirtualAddress()
                                                                                : s_Dummy->GetGPUVirtualAddress());
        commands->SetGraphicsRootShaderResourceView(referenceParameter,
                                                    reference ? s_Reference->GetGPUVirtualAddress() : lights);
        // M3：MegaLightsShadePS は Raw に書く（それ以外のパスは書かない）。ディファードはデノイズ済みを読む
        const bool rawWritable = s_Raw && s_RawState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        commands->SetGraphicsRootUnorderedAccessView(rawParameter, rawWritable ? s_Raw->GetGPUVirtualAddress()
                                                                               : s_Dummy->GetGPUVirtualAddress());
        ID3D12Resource* denoised = s_FinalIsA ? s_FilterA.Get() : s_FilterB.Get();
        const D3D12_RESOURCE_STATES denoisedState = s_FinalIsA ? s_FilterAState : s_FilterBState;
        const bool denoisedReady = sampled && s_SlotDenoise[slot] && s_DenoiseDone && denoised &&
                                   denoisedState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commands->SetGraphicsRootShaderResourceView(denoisedParameter,
                                                    denoisedReady ? denoised->GetGPUVirtualAddress() : lights);
        // M4：デバッグ表示用（このフレームに作ったハッシュ）
        const bool hashReady = sampled && s_SlotHash[slot] && s_HashDisplay;
        commands->SetGraphicsRootShaderResourceView(hashParameter,
                                                    hashReady ? s_HashDisplay->GetGPUVirtualAddress() : lights);
    }

    // ---- M6：フロクセルフォグ（AdvancedEffects）から
    // このフレームのグリッドがフォグ用に構築されている（= グリッドのライトはフォグ側でも MegaLights が受け持つ）
    static bool IsVolumetricGridActive()
    {
        const D3D12_RESOURCE_STATES bothRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return s_Available && s_SlotBuilt[s_Slot] && s_SlotVolumetric[s_Slot] && s_FineCells &&
               s_FineCellsState == bothRead;
    }

    static int GetFogSamples()
    {
        return std::clamp(s_Settings.FogSamples, 1, 4);
    }

    // IsVolumetricGridActive() が false でも有効なアドレスを入れる（シェーダー側は定数で判定）
    static bool BindFogCompute(ID3D12GraphicsCommandList* commands, UINT constantsParameter, UINT lightsParameter,
                               UINT cellsParameter, UINT indicesParameter, D3D12_GPU_VIRTUAL_ADDRESS fallbackConstants,
                               D3D12_GPU_VIRTUAL_ADDRESS fallbackBuffer)
    {
        const bool active = IsVolumetricGridActive();
        const UINT slot = s_Slot;
        commands->SetComputeRootConstantBufferView(
            constantsParameter,
            s_Constants ? s_Constants->GetGPUVirtualAddress() + slot * kConstantStride : fallbackConstants);
        const D3D12_GPU_VIRTUAL_ADDRESS lights =
            s_LightUpload ? s_LightUpload->GetGPUVirtualAddress() + slot * kLightStride : fallbackBuffer;
        commands->SetComputeRootShaderResourceView(lightsParameter, lights);
        commands->SetComputeRootShaderResourceView(cellsParameter,
                                                   active ? s_FineCells->GetGPUVirtualAddress() : lights);
        commands->SetComputeRootShaderResourceView(indicesParameter,
                                                   active ? s_FineIndices->GetGPUVirtualAddress() : lights);
        return active;
    }

    // ---- M7：Forward / 半透明（モデルのルートシグネチャ）
    // エンジンのディスクリプタヒープ内の MegaLights 用の領域（フレームごとに t44, t45, t46, b10）
    static constexpr UINT kForwardDescriptorsPerFrame = 4;
    static constexpr UINT kEngineHeapDescriptors = kForwardDescriptorsPerFrame * kFrames;

    static void SetEngineHeap(ID3D12DescriptorHeap* heap, UINT startIndex)
    {
        s_EngineHeap = heap;
        s_EngineHeapStart = startIndex;
        if (s_Device)
        {
            s_EngineIncrement = s_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        for (UINT frame = 0; frame < kFrames; ++frame)
        {
            WriteForwardDescriptors(frame, false);
        }
    }

    static void BindForward(ID3D12GraphicsCommandList* commands, UINT gridParameter, UINT texturesParameter,
                            D3D12_GPU_DESCRIPTOR_HANDLE textureTable)
    {
        if (!commands || !s_EngineHeap || !s_ForwardDescriptorsReady[s_Slot])
        {
            return;
        }
        commands->SetGraphicsRootDescriptorTable(
            gridParameter,
            CD3DX12_GPU_DESCRIPTOR_HANDLE(s_EngineHeap->GetGPUDescriptorHandleForHeapStart(),
                                          s_EngineHeapStart + s_Slot * kForwardDescriptorsPerFrame, s_EngineIncrement));
        commands->SetGraphicsRootDescriptorTable(texturesParameter, textureTable);
    }

    // ---- M3：ディファードの直前に呼ぶ
    static bool WantsShadePass()
    {
        return s_SlotBuilt[s_Slot] && s_SlotSampled[s_Slot] && s_SlotDenoise[s_Slot];
    }

    // MegaLightsShadePS のパイプライン（ポストプロセスのルートシグネチャ・シーンカラーの形式で初回に作る）
    static ID3D12PipelineState* GetShadePso(ID3D12RootSignature* root, DXGI_FORMAT sceneFormat)
    {
        if (s_ShadePso && s_ShadeFormat == sceneFormat)
        {
            return s_ShadePso.Get();
        }
        if (s_ShadeFailed || !root || !s_Device)
        {
            return nullptr;
        }
        s_ShadePso.Reset();
        ComPtr<ID3DBlob> vs, ps, errors;
        if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/postProcessVS.cso", &vs)))
        {
            if (FAILED(D3DCompileFromFile(L"shader/hlsl/postProcessVS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                          "main", "vs_6_5", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors)))
            {
                WriteGraphicsLog("MegaLights: postProcessVS unavailable %s\n",
                                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                s_ShadeFailed = true;
                return nullptr;
            }
        }
        errors.Reset();
        const D3D_SHADER_MACRO defines[] = {{"SHADER_RAYTRACING", "1"}, {}};
        if (FAILED(D3DCompileFromFile(L"shader/hlsl/DeferredLightingPS.hlsl",
                                      Raytracing::IsAvailable() ? defines : nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                      "MegaLightsShadePS", "ps_6_5", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors)))
        {
            WriteGraphicsLog("MegaLights: MegaLightsShadePS compile failed: %s\n",
                             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            s_ShadeFailed = true;
            return nullptr;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
        desc.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
        desc.pRootSignature = root;
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0; // カラーは書かない（UAV だけ）
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = sceneFormat;
        desc.SampleDesc.Count = 1;
        if (FAILED(s_Device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&s_ShadePso))))
        {
            WriteGraphicsLog("MegaLights: MegaLightsShadePS pipeline failed\n");
            s_ShadeFailed = true;
            return nullptr;
        }
        s_ShadeFormat = sceneFormat;
        WriteGraphicsLog("MegaLights: denoise shade pass ready\n");
        return s_ShadePso.Get();
    }

    // シェードパスを作れなかったフレーム：ディファードは M2 の直接評価に戻す（定数はまだ GPU が読んでいない）
    static void CancelDenoise()
    {
        s_SlotDenoise[s_Slot] = false;
        s_Stats.Denoised = false;
        if (s_MappedConstants)
        {
            Constants* constants = reinterpret_cast<Constants*>(s_MappedConstants + s_Slot * kConstantStride);
            constants->Stochastic.x &= ~kFlagDenoised;
        }
    }

    static void BeginShade(ID3D12GraphicsCommandList* commands)
    {
        vector<D3D12_RESOURCE_BARRIER> barriers;
        Transition(barriers, s_Raw.Get(), s_RawState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!barriers.empty())
        {
            commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
        s_DenoiseDone = false;
    }

    // gbuffer: Normal, Depth, Velocity（PIXEL_SHADER_RESOURCE。終了時に同じ状態へ戻す）
    // gbufferTable: GBuffer の SRV テーブル（BaseColor から 8 個連続：t8〜t15）
    static void Denoise(ID3D12GraphicsCommandList* commands, ID3D12DescriptorHeap* heap,
                        ID3D12Resource* const gbuffer[3], D3D12_GPU_DESCRIPTOR_HANDLE gbufferTable)
    {
        if (!commands || !heap || !WantsShadePass() || !s_Raw)
        {
            return;
        }
        RenderProfiler::ScopedEvent profile("MegaLights Denoise", commands);
        const UINT slot = s_Slot;
        const D3D12_RESOURCE_STATES gbufferRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const D3D12_RESOURCE_STATES bothRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const UINT current = s_HistoryIndex, previous = s_HistoryIndex ^ 1u;

        // 履歴の有効性：解像度・カメラ・連続性（前のフレームもデノイズした）・手動リセット
        const EntityID camera = Camera::GetCameraEntity();
        bool historyValid = s_DenoiseHistoryValid && camera == s_DenoiseCamera &&
                            s_LastDenoiseFrame + 1 == s_FrameCounter && !s_DenoiseResetRequested;
        s_Stats.HistoryReset = !historyValid;
        s_DenoiseResetRequested = false;
        s_Stats.NrdFallback = nullptr;

        // ---- M6：NRD（REBLUR）
        if (s_Settings.Denoiser == 1)
        {
            if (DenoiseNrd(commands, historyValid))
            {
                s_Stats.DenoiserUsed = "NRD (REBLUR)";
                s_FinalIsA = true;
                s_DenoiseHistoryValid = false; // 自前の履歴はこのフレームに更新していない
                s_DenoiseCamera = camera;
                s_LastDenoiseFrame = s_FrameCounter;
                s_DenoiseDone = true;
                s_NrdLastFrame = s_FrameCounter;
                return;
            }
            if (!s_Stats.NrdFallback)
            {
                s_Stats.NrdFallback = "NRD を実行できませんでした";
            }
        }
        s_Stats.DenoiserUsed = "自前 (SVGF)";

        vector<D3D12_RESOURCE_BARRIER> barriers;
        for (UINT index = 0; index < 3; ++index)
        {
            if (gbuffer[index])
            {
                barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(gbuffer[index], gbufferRead, bothRead));
            }
        }
        Transition(barriers, s_Raw.Get(), s_RawState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(barriers, s_HistoryColor[previous].Get(), s_HistoryColorState[previous],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(barriers, s_HistoryMoments[previous].Get(), s_HistoryMomentsState[previous],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(barriers, s_HistoryColor[current].Get(), s_HistoryColorState[current],
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_HistoryMoments[current].Get(), s_HistoryMomentsState[current],
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_FilterA.Get(), s_FilterAState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_FilterB.Get(), s_FilterBState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!barriers.empty())
        {
            commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

        DenoiseConstants constants{};
        constants.PrevViewZRow = s_PreviousViewZRow;
        constants.HistoryValid = historyValid ? 1u : 0u;
        const UINT maxHistory = static_cast<UINT>(std::clamp(s_Settings.MaxHistory, 1, 256));
        constants.MaxHistory =
            s_Stats.AntiLag ? (std::min)(maxHistory, static_cast<UINT>(std::clamp(s_Settings.AntiLagHistory, 1, 256)))
                            : maxHistory;
        constants.StepSize = 1;
        constants.Flags = (s_Settings.FireflySuppression ? 1u : 0u);
        constants.AlphaMin = std::clamp(s_Settings.AlphaMin, 0.001f, 1.0f);
        constants.MomentsAlphaMin = std::clamp(s_Settings.MomentsAlphaMin, 0.001f, 1.0f);
        constants.DepthTolerance = std::clamp(s_Settings.DepthTolerance, 0.001f, 1.0f);
        constants.NormalThreshold = std::clamp(s_Settings.NormalThreshold, -1.0f, 1.0f);
        constants.PhiNormal = std::clamp(s_Settings.PhiNormal, 1.0f, 256.0f);
        constants.PhiLuminance = std::clamp(s_Settings.PhiLuminance, 0.1f, 100.0f);
        constants.PhiPlane = std::clamp(s_Settings.PhiPlane, 0.05f, 100.0f);

        ID3D12DescriptorHeap* heaps[] = {heap};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(s_DenoiseRoot.Get());
        commands->SetComputeRootConstantBufferView(0, s_Constants->GetGPUVirtualAddress() + slot * kConstantStride);
        commands->SetComputeRootDescriptorTable(2, gbufferTable);
        const UINT groupsX = (s_Width + 7) / 8, groupsY = (s_Height + 7) / 8;

        // 時間方向：Raw + 前フレームの履歴 → FilterA と今フレームの履歴
        commands->SetComputeRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
        commands->SetComputeRootShaderResourceView(3, s_Raw->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(4, s_HistoryColor[previous]->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(5, s_HistoryMoments[previous]->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(6, s_FilterA->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(7, s_HistoryColor[current]->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(8, s_HistoryMoments[current]->GetGPUVirtualAddress());
        commands->SetPipelineState(s_Temporal.Get());
        commands->Dispatch(groupsX, groupsY, 1);

        // À-Trous：A → B → A ...
        const int iterations = std::clamp(s_Settings.DenoiseIterations, 0, 5);
        bool inputIsA = true;
        commands->SetPipelineState(s_ATrous.Get());
        for (int iteration = 0; iteration < iterations; ++iteration)
        {
            ID3D12Resource* input = inputIsA ? s_FilterA.Get() : s_FilterB.Get();
            ID3D12Resource* output = inputIsA ? s_FilterB.Get() : s_FilterA.Get();
            D3D12_RESOURCE_STATES& inputState = inputIsA ? s_FilterAState : s_FilterBState;
            D3D12_RESOURCE_STATES& outputState = inputIsA ? s_FilterBState : s_FilterAState;
            barriers.clear();
            Transition(barriers, input, inputState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(barriers, output, outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(s_HistoryColor[current].Get()));
            commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            constants.StepSize = 1u << iteration;
            constants.Flags =
                (s_Settings.FireflySuppression ? 1u : 0u) | ((iteration == 0 && s_Settings.HistoryFeedback) ? 2u : 0u);
            commands->SetComputeRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
            commands->SetComputeRootShaderResourceView(3, input->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(6, output->GetGPUVirtualAddress());
            commands->Dispatch(groupsX, groupsY, 1);
            inputIsA = !inputIsA;
        }
        s_FinalIsA = inputIsA;

        // ディファードで読む状態へ。今フレームの履歴は次フレームに「前フレーム」として読む
        barriers.clear();
        if (s_FinalIsA)
        {
            Transition(barriers, s_FilterA.Get(), s_FilterAState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else
        {
            Transition(barriers, s_FilterB.Get(), s_FilterBState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        Transition(barriers, s_HistoryColor[current].Get(), s_HistoryColorState[current],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(barriers, s_HistoryMoments[current].Get(), s_HistoryMomentsState[current],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (UINT index = 0; index < 3; ++index)
        {
            if (gbuffer[index])
            {
                barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(gbuffer[index], bothRead, gbufferRead));
            }
        }
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

        // 次フレーム用：前フレームのビュー Z の行（mul(float4(world, 1), view) の z）
        XMMATRIX view = XMMatrixIdentity();
        XMMATRIX projection = XMMatrixIdentity();
        Camera::GetCameraMatrices(camera, view, projection);
        XMFLOAT4X4 v;
        XMStoreFloat4x4(&v, view);
        s_PreviousViewZRow = XMFLOAT4(v._13, v._23, v._33, v._43);
        s_HistoryIndex = previous;
        s_DenoiseHistoryValid = true;
        s_DenoiseCamera = camera;
        s_LastDenoiseFrame = s_FrameCounter;
        s_DenoiseDone = true;
    }

  private:
    struct Constants
    {
        XMFLOAT4X4 WorldToView;
        XMFLOAT4 Projection;
        XMFLOAT4 Depth;
        XMFLOAT4 Depth2;
        XMUINT4 Grid;
        XMUINT4 Counts;
        XMUINT4 Screen;
        // ---- M2
        XMUINT4 Stochastic;        // フラグ, N, フレーム番号, 累積フレーム数
        XMFLOAT4 StochasticParams; // α, 法線バイアス, 1/p の表示上限, リファレンスの影レイ数
        XMFLOAT4X4 ViewToWorld;
        // ---- M5
        XMFLOAT4 ScreenTrace; // ステップ数, 最大距離（px）, 厚み（m）, 自己遮蔽よけ（px）
        XMUINT4 SampleGrid;   // サンプル空間の幅, 高さ, 縮小率, 代表ピクセルの回転
    };

    static_assert(sizeof(Constants) == 288, "MegaLightGridConstants layout mismatch");
    static constexpr UINT kConstantStride = 512;
    static constexpr UINT64 kLightStride = (sizeof(GpuLight) * kMaxLights + 255u) & ~255ull;
    static constexpr UINT kFlagStochastic = 1u, kFlagReference = 2u, kFlagAccumulate = 4u, kFlagHasReference = 8u,
                          kFlagDenoised = 16u, kFlagGuide = 32u, kFlagScreenTrace = 64u;
    static constexpr UINT kGridVolumetric = 1u << 16; // Grid.w：フォグ用に視錐台全体を構築
    static constexpr UINT kGridForward = 1u << 17;    // Grid.w：Forward / 半透明もグリッドで評価（M7）

    struct DenoiseConstants
    {
        XMFLOAT4 PrevViewZRow;
        UINT HistoryValid, MaxHistory, StepSize, Flags;
        float AlphaMin, MomentsAlphaMin, DepthTolerance, NormalThreshold;
        float PhiNormal, PhiLuminance, PhiPlane, Padding;
    };

    static_assert(sizeof(DenoiseConstants) == 64, "DenoiseConstants layout mismatch");

    // M3 のルートシグネチャ：0 b0 / 1 b1（定数 16）/ 2 テーブル t8〜t15（GBuffer）/ 3 t0 / 4 t1 / 5 t2 / 6 u0 / 7 u1 /
    // 8 u2
    static bool CreateDenoisePipelines(ID3D12Device* device)
    {
        CD3DX12_DESCRIPTOR_RANGE gbufferRange;
        gbufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 8);
        CD3DX12_ROOT_PARAMETER parameters[9];
        parameters[0].InitAsConstantBufferView(0);
        parameters[1].InitAsConstants(sizeof(DenoiseConstants) / 4, 1);
        parameters[2].InitAsDescriptorTable(1, &gbufferRange);
        parameters[3].InitAsShaderResourceView(0);
        parameters[4].InitAsShaderResourceView(1);
        parameters[5].InitAsShaderResourceView(2);
        parameters[6].InitAsUnorderedAccessView(0);
        parameters[7].InitAsUnorderedAccessView(1);
        parameters[8].InitAsUnorderedAccessView(2);
        if (!CreateRootSignature(device, CD3DX12_ROOT_SIGNATURE_DESC(_countof(parameters), parameters, 0, nullptr),
                                 s_DenoiseRoot) ||
            !CompileEntries(device, L"shader/hlsl/MegaLightsDenoiseCS.hlsl", s_DenoiseRoot.Get(),
                            {{"Temporal", &s_Temporal}, {"ATrous", &s_ATrous}}))
        {
            s_DenoiseRoot.Reset();
            s_Temporal.Reset();
            s_ATrous.Reset();
            WriteGraphicsLog("WARNING: MegaLights M3 unavailable (denoise disabled)\n");
            return false;
        }
        return true;
    }

    // ---- M6：NRD の入出力パス
    // ルート：0 b0 / 1 t0 Raw / 2 テーブル t1〜t2（ViewZ ガイド, NRD 出力）/ 3 テーブル u0（NRD 入力）/ 4 u1
    // デノイズ済み
    static bool CreateNrdPipelines(ID3D12Device* device)
    {
        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        CD3DX12_ROOT_PARAMETER parameters[5];
        parameters[0].InitAsConstantBufferView(0);
        parameters[1].InitAsShaderResourceView(0);
        parameters[2].InitAsDescriptorTable(1, &ranges[0]);
        parameters[3].InitAsDescriptorTable(1, &ranges[1]);
        parameters[4].InitAsUnorderedAccessView(1);
        const D3D_SHADER_MACRO defines[] = {{"USE_NRD", "1"}, {}};
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heap.NumDescriptors = kNrdDescriptorsPerFrame * kFrames;
        if (!CreateRootSignature(device, CD3DX12_ROOT_SIGNATURE_DESC(_countof(parameters), parameters, 0, nullptr),
                                 s_NrdRoot) ||
            !CompileEntries(device, L"shader/hlsl/MegaLightsNrdCS.hlsl", s_NrdRoot.Get(),
                            {{"NrdPack", &s_NrdPack}, {"NrdUnpack", &s_NrdUnpack}}, defines) ||
            FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s_NrdHeap))))
        {
            s_NrdRoot.Reset();
            s_NrdPack.Reset();
            s_NrdUnpack.Reset();
            s_NrdHeap.Reset();
            WriteGraphicsLog("WARNING: MegaLights NRD path unavailable (built-in denoiser only)\n");
            return false;
        }
        s_NrdIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    static bool EnsureNrd(UINT width, UINT height)
    {
        if (s_Nrd && s_NrdNoisy && s_NrdWidth == width && s_NrdHeight == height)
        {
            return true;
        }
        // 作り直し：古いものは GPU が使い終わるまで保持
        if (s_Nrd)
        {
            s_RetiredNrd.push_back({move(s_Nrd), kFrames + 1});
        }
        Retire(s_NrdNoisy);
        auto denoiser = make_unique<NrdDenoiser>();
        if (!denoiser->Initialize(s_Device.Get(), NrdSignal::MegaLights, width, height))
        {
            return false;
        }
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1, 1, 0,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(s_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&s_NrdNoisy))))
        {
            return false;
        }
        s_NrdNoisy->SetName(L"MegaLights NRD Noisy Input");
        s_NrdNoisyState = D3D12_RESOURCE_STATE_COMMON;
        s_Nrd = move(denoiser);
        s_NrdWidth = width;
        s_NrdHeight = height;
        return true;
    }

    static bool DenoiseNrd(ID3D12GraphicsCommandList* commands, bool historyValid)
    {
        if (!s_NrdAvailable)
        {
            s_Stats.NrdFallback = "NRD が組み込まれていない / シェーダーの初期化に失敗";
            return false;
        }
        ID3D12Resource* guides[3] = {};
        NrdFrame frame;
        uint64_t serial = 0;
        if (!Raytracing::GetNrdGuides(guides, frame, serial) || serial == s_NrdGuideSerial)
        {
            s_Stats.NrdFallback = "このフレームの NRD ガイドがありません（レイトレーシングのデノイズ設定を確認）";
            return false;
        }
        const D3D12_RESOURCE_DESC guideDesc = guides[2]->GetDesc();
        if (guideDesc.Width != s_Width || guideDesc.Height != s_Height)
        {
            s_Stats.NrdFallback = "NRD ガイドと解像度が一致しません";
            return false;
        }
        if (!EnsureNrd(s_Width, s_Height))
        {
            s_Stats.NrdFallback = "NRD の初期化に失敗";
            return false;
        }
        s_NrdGuideSerial = serial;
        RenderProfiler::ScopedEvent profile("MegaLights NRD", commands);

        // 記述子（フレームごと）：0 ViewZ ガイド, 1 NRD 出力, 2 NRD 入力（UAV）
        const UINT base = s_Slot * kNrdDescriptorsPerFrame;
        auto cpu = [&](UINT offset) {
            return CD3DX12_CPU_DESCRIPTOR_HANDLE(s_NrdHeap->GetCPUDescriptorHandleForHeapStart(), base + offset,
                                                 s_NrdIncrement);
        };
        auto gpu = [&](UINT offset) {
            return CD3DX12_GPU_DESCRIPTOR_HANDLE(s_NrdHeap->GetGPUDescriptorHandleForHeapStart(), base + offset,
                                                 s_NrdIncrement);
        };
        auto srv = [&](ID3D12Resource* resource, UINT offset) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = resource->GetDesc().Format;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            s_Device->CreateShaderResourceView(resource, &view, cpu(offset));
        };
        srv(guides[2], 0);
        srv(s_Nrd->GetOutput(), 1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        s_Device->CreateUnorderedAccessView(s_NrdNoisy.Get(), nullptr, &uav, cpu(2));

        const D3D12_RESOURCE_STATES pixelRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const D3D12_RESOURCE_STATES bothRead = pixelRead | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const UINT groupsX = (s_Width + 7) / 8, groupsY = (s_Height + 7) / 8;
        auto bind = [&]() {
            ID3D12DescriptorHeap* heaps[] = {s_NrdHeap.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(s_NrdRoot.Get());
            commands->SetComputeRootConstantBufferView(0,
                                                       s_Constants->GetGPUVirtualAddress() + s_Slot * kConstantStride);
            commands->SetComputeRootShaderResourceView(1, s_Raw->GetGPUVirtualAddress());
            commands->SetComputeRootDescriptorTable(2, gpu(0));
            commands->SetComputeRootDescriptorTable(3, gpu(2));
            commands->SetComputeRootUnorderedAccessView(4, s_FilterA->GetGPUVirtualAddress());
        };

        // 1) Raw → NRD 入力
        vector<D3D12_RESOURCE_BARRIER> barriers;
        Transition(barriers, s_Raw.Get(), s_RawState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(barriers, s_NrdNoisy.Get(), s_NrdNoisyState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(guides[2], pixelRead, bothRead));
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        bind();
        commands->SetPipelineState(s_NrdPack.Get());
        commands->Dispatch(groupsX, groupsY, 1);
        barriers.clear();
        Transition(barriers, s_NrdNoisy.Get(), s_NrdNoisyState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(guides[2], bothRead, pixelRead));
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

        // 2) REBLUR（ガイドは Raytracing と共有。状態は Execute が元に戻す）
        frame.FrameIndex = s_Slot;
        frame.ResetHistory = frame.ResetHistory || !historyValid || s_NrdLastFrame + 1 != s_FrameCounter;
        const bool executed = s_Nrd->Execute(commands,
                                             {{guides[0], pixelRead},
                                              {guides[1], pixelRead},
                                              {guides[2], pixelRead},
                                              {s_NrdNoisy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}},
                                             frame);
        if (!executed)
        {
            // Execute は記録前に検証して失敗する（状態は変わっていない）。ヒープは呼び出し側で戻る
            s_Stats.NrdFallback = "NRD の実行に失敗（graphics ログを確認）";
            return false;
        }

        // 3) NRD 出力 → デノイズ済みバッファ（FilterA）
        ID3D12Resource* output = s_Nrd->GetOutput();
        barriers.clear();
        barriers.push_back(
            CD3DX12_RESOURCE_BARRIER::Transition(output, pixelRead, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        Transition(barriers, s_FilterA.Get(), s_FilterAState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        bind();
        commands->SetPipelineState(s_NrdUnpack.Get());
        commands->Dispatch(groupsX, groupsY, 1);
        barriers.clear();
        barriers.push_back(
            CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, pixelRead));
        Transition(barriers, s_FilterA.Get(), s_FilterAState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        return true;
    }

    // Raw / 履歴 2 組 / À-Trous の 2 枚（ピクセルあたり 16B × 7）
    static bool EnsureDenoise(UINT width, UINT height)
    {
        const UINT64 pixels = static_cast<UINT64>(width) * height;
        if (pixels <= s_DenoiseCapacity && width == s_DenoiseWidth && height == s_DenoiseHeight)
        {
            return true;
        }
        Retire(s_Raw);
        Retire(s_FilterA);
        Retire(s_FilterB);
        for (UINT index = 0; index < 2; ++index)
        {
            Retire(s_HistoryColor[index]);
            Retire(s_HistoryMoments[index]);
        }
        const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const UINT64 bytes = pixels * 16ull;
        bool ok =
            CreateBuffer(s_Raw, bytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON, L"MegaLights Raw") &&
            CreateBuffer(s_FilterA, bytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                         L"MegaLights Filter A") &&
            CreateBuffer(s_FilterB, bytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                         L"MegaLights Filter B");
        for (UINT index = 0; ok && index < 2; ++index)
        {
            ok = CreateBuffer(s_HistoryColor[index], bytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                              L"MegaLights History Color") &&
                 CreateBuffer(s_HistoryMoments[index], bytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                              L"MegaLights History Moments");
        }
        if (!ok)
        {
            s_Raw.Reset();
            s_FilterA.Reset();
            s_FilterB.Reset();
            for (UINT index = 0; index < 2; ++index)
            {
                s_HistoryColor[index].Reset();
                s_HistoryMoments[index].Reset();
            }
            s_DenoiseCapacity = 0;
            return false;
        }
        s_RawState = s_FilterAState = s_FilterBState = D3D12_RESOURCE_STATE_COMMON;
        for (UINT index = 0; index < 2; ++index)
        {
            s_HistoryColorState[index] = s_HistoryMomentsState[index] = D3D12_RESOURCE_STATE_COMMON;
        }
        s_DenoiseCapacity = pixels;
        s_DenoiseWidth = width;
        s_DenoiseHeight = height;
        s_DenoiseHistoryValid = false; // 新しいバッファの中身は未定義
        s_Stats.DenoiseBufferMB = static_cast<double>(bytes * 7ull) / (1024.0 * 1024.0);
        return true;
    }

    // Forward 用のディスクリプタ（ライト / セル / セル内ライト / 定数）。構築していないフレームはライトバッファを指す
    static void WriteForwardDescriptors(UINT slot, bool built)
    {
        s_ForwardDescriptorsReady[slot] = false;
        if (!s_EngineHeap || !s_Device || !s_LightUpload || !s_Constants)
        {
            return;
        }
        auto cpu = [&](UINT offset) {
            return CD3DX12_CPU_DESCRIPTOR_HANDLE(s_EngineHeap->GetCPUDescriptorHandleForHeapStart(),
                                                 s_EngineHeapStart + slot * kForwardDescriptorsPerFrame + offset,
                                                 s_EngineIncrement);
        };
        auto buffer = [&](ID3D12Resource* resource, UINT64 first, UINT count, UINT stride, UINT offset) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_UNKNOWN;
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer.FirstElement = first;
            view.Buffer.NumElements = (std::max)(count, 1u);
            view.Buffer.StructureByteStride = stride;
            s_Device->CreateShaderResourceView(resource, &view, cpu(offset));
        };
        static_assert(kLightStride % sizeof(GpuLight) == 0, "light slots must align to whole lights");
        buffer(s_LightUpload.Get(), slot * (kLightStride / sizeof(GpuLight)), kMaxLights, sizeof(GpuLight), 0);
        const bool grid = built && s_FineCells && s_FineIndices;
        if (grid)
        {
            buffer(s_FineCells.Get(), 0, static_cast<UINT>(s_FineCellCapacity), 8, 1);
            buffer(s_FineIndices.Get(), 0, kFineCapacity, 4, 2);
        }
        else
        {
            buffer(s_LightUpload.Get(), 0, 16, 8, 1);
            buffer(s_LightUpload.Get(), 0, 16, 4, 2);
        }
        D3D12_CONSTANT_BUFFER_VIEW_DESC constants{};
        constants.BufferLocation = s_Constants->GetGPUVirtualAddress() + slot * kConstantStride;
        constants.SizeInBytes = kConstantStride;
        s_Device->CreateConstantBufferView(&constants, cpu(3));
        s_ForwardDescriptorsReady[slot] = true;
    }

    static void HashBytes(uint64_t& hash, const void* data, size_t size)
    {
        const BYTE* bytes = static_cast<const BYTE*>(data);
        for (size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    }

    static bool CreateBuffer(ComPtr<ID3D12Resource>& output, UINT64 size, D3D12_HEAP_TYPE heapType,
                             D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name)
    {
        const CD3DX12_HEAP_PROPERTIES heapProperties(heapType);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
        if (FAILED(s_Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                     IID_PPV_ARGS(output.ReleaseAndGetAddressOf()))))
        {
            return false;
        }
        output->SetName(name);
        return true;
    }

    static bool CompileEntries(ID3D12Device* device, const wchar_t* file, ID3D12RootSignature* root,
                               std::initializer_list<pair<const char*, ComPtr<ID3D12PipelineState>*>> entries,
                               const D3D_SHADER_MACRO* defines = nullptr)
    {
        for (const auto& entry : entries)
        {
            ComPtr<ID3DBlob> blob, errors;
            if (FAILED(D3DCompileFromFile(file, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry.first, "cs_6_5",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
            {
                WriteGraphicsLog("MegaLights: %s compile failed: %s\n", entry.first,
                                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = root;
            desc.CS = CD3DX12_SHADER_BYTECODE(blob.Get());
            if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(entry.second->ReleaseAndGetAddressOf()))))
            {
                WriteGraphicsLog("MegaLights: %s pipeline failed\n", entry.first);
                return false;
            }
        }
        return true;
    }

    static bool CreateRootSignature(ID3D12Device* device, const CD3DX12_ROOT_SIGNATURE_DESC& desc,
                                    ComPtr<ID3D12RootSignature>& root)
    {
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(
                device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))))
        {
            WriteGraphicsLog("MegaLights: root signature failed %s\n",
                             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return false;
        }
        return true;
    }

    static bool CreateGridPipelines(ID3D12Device* device)
    {
        CD3DX12_DESCRIPTOR_RANGE depthRange;
        depthRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_ROOT_PARAMETER parameters[10];
        parameters[0].InitAsConstantBufferView(0);
        parameters[1].InitAsShaderResourceView(1);
        parameters[2].InitAsDescriptorTable(1, &depthRange);
        for (UINT uav = 0; uav < 7; ++uav)
        {
            parameters[3 + uav].InitAsUnorderedAccessView(uav);
        }
        if (!CreateRootSignature(device, CD3DX12_ROOT_SIGNATURE_DESC(_countof(parameters), parameters, 0, nullptr),
                                 s_Root) ||
            !CompileEntries(device, L"shader/hlsl/MegaLightsGridCS.hlsl", s_Root.Get(),
                            {{"ClearCounters", &s_ClearCounters},
                             {"TileSliceMask", &s_TileSliceMask},
                             {"CoarseCull", &s_CoarseCull},
                             {"FineCull", &s_FineCull}}))
        {
            ShutdownPipelines();
            return false;
        }
        return true;
    }

    // M2 のルートシグネチャ：0 b0 / 1 t1 ライト / 2 t3 セル / 3 t4 セル内ライト / 4 t5 TLAS / 5 t7 リファレンス
    //                       6 テーブル t8〜t15（GBuffer。t15 = Velocity）/ 7 u0 サンプル / 8 u1 累積 / 9 u2
    //                       誤差の部分和 10 t6 前フレームのハッシュ / 11 u3 今フレームのハッシュ（M4） 12 u4 タイル /
    //                       13 u5 トレース / 14 u6 ワールドトレース / 15 u7 カウンタ / 16 u8 間接引数（M5）
    static bool CreateSamplePipelines(ID3D12Device* device)
    {
        CD3DX12_DESCRIPTOR_RANGE gbufferRange;
        gbufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 8);
        CD3DX12_ROOT_PARAMETER parameters[17];
        parameters[0].InitAsConstantBufferView(0);
        parameters[1].InitAsShaderResourceView(1);
        parameters[2].InitAsShaderResourceView(3);
        parameters[3].InitAsShaderResourceView(4);
        parameters[4].InitAsShaderResourceView(5);
        parameters[5].InitAsShaderResourceView(7);
        parameters[6].InitAsDescriptorTable(1, &gbufferRange);
        parameters[7].InitAsUnorderedAccessView(0);
        parameters[8].InitAsUnorderedAccessView(1);
        parameters[9].InitAsUnorderedAccessView(2);
        parameters[10].InitAsShaderResourceView(6);
        parameters[11].InitAsUnorderedAccessView(3);
        for (UINT uav = 0; uav < 5; ++uav)
        {
            parameters[12 + uav].InitAsUnorderedAccessView(4 + uav);
        }
        if (!CreateRootSignature(device, CD3DX12_ROOT_SIGNATURE_DESC(_countof(parameters), parameters, 0, nullptr),
                                 s_SampleRoot) ||
            !CompileEntries(device, L"shader/hlsl/MegaLightsSampleCS.hlsl", s_SampleRoot.Get(),
                            {{"SampleLights", &s_SampleLights},
                             {"ScreenTrace", &s_ScreenTrace},
                             {"WorldTrace", &s_WorldTrace},
                             {"ErrorReduce", &s_ErrorReduce},
                             {"ClearTraceCounters", &s_ClearTraceCounters},
                             {"ClassifyTiles", &s_ClassifyTiles},
                             {"ArgsTiles", &s_ArgsTiles},
                             {"ArgsTrace", &s_ArgsTrace},
                             {"ArgsWorld", &s_ArgsWorld}}) ||
            !CreateTraceResources(device))
        {
            ResetSamplePipelines();
            WriteGraphicsLog(
                "WARNING: MegaLights M2 unavailable (stochastic sampling disabled, M1 grid still works)\n");
            return false;
        }
        // M4 は失敗しても M2 は使える（ガイドなし）
        s_HashAvailable = CompileEntries(device, L"shader/hlsl/MegaLightsSampleCS.hlsl", s_SampleRoot.Get(),
                                         {{"BuildHash", &s_BuildHash}});
        if (!s_HashAvailable)
        {
            WriteGraphicsLog("WARNING: MegaLights M4 unavailable (visible light hash disabled)\n");
        }
        return true;
    }

    // M5：間接ディスパッチのコマンドシグネチャ・カウンタ・引数（解像度に依存しないもの）
    static bool CreateTraceResources(ID3D12Device* device)
    {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &argument;
        const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&s_DispatchSignature))) ||
            !CreateBuffer(s_TraceCounters, 16, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                          L"MegaLights Trace Counters") ||
            !CreateBuffer(s_DispatchArgs, 64, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                          L"MegaLights Dispatch Args") ||
            !CreateBuffer(s_TraceReadback, 16 * kFrames, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, L"MegaLights Trace Readback"))
        {
            WriteGraphicsLog("MegaLights: trace resources failed\n");
            return false;
        }
        s_TraceCountersState = s_DispatchArgsState = D3D12_RESOURCE_STATE_COMMON;
        return true;
    }

    static void ResetSamplePipelines()
    {
        s_SampleRoot.Reset();
        s_SampleLights.Reset();
        s_ScreenTrace.Reset();
        s_WorldTrace.Reset();
        s_ErrorReduce.Reset();
        s_ClearTraceCounters.Reset();
        s_ClassifyTiles.Reset();
        s_ArgsTiles.Reset();
        s_ArgsTrace.Reset();
        s_ArgsWorld.Reset();
        s_DispatchSignature.Reset();
        s_TraceCounters.Reset();
        s_DispatchArgs.Reset();
        s_TraceReadback.Reset();
    }

    // 8x8 タイルごとの 128bit（uint4）× 2 枚（前フレーム / 今フレーム）
    static bool EnsureHash(UINT tilesX, UINT tilesY)
    {
        if (tilesX == s_HashTilesX && tilesY == s_HashTilesY && s_Hash[0] && s_Hash[1])
        {
            return true;
        }
        const UINT64 bytes = static_cast<UINT64>(tilesX) * tilesY * 16ull;
        for (UINT index = 0; index < 2; ++index)
        {
            Retire(s_Hash[index]);
            if (!CreateBuffer(s_Hash[index], bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COMMON, L"MegaLights Visible Light Hash"))
            {
                s_Hash[0].Reset();
                s_Hash[1].Reset();
                s_HashTilesX = s_HashTilesY = 0;
                return false;
            }
            s_HashState[index] = D3D12_RESOURCE_STATE_COMMON;
        }
        s_HashTilesX = tilesX;
        s_HashTilesY = tilesY;
        s_HashValid = false; // 新しいバッファの中身は未定義
        s_HashDisplay = nullptr;
        return true;
    }

    static void BindSampleRoot(ID3D12GraphicsCommandList* commands, UINT slot, D3D12_GPU_VIRTUAL_ADDRESS scene,
                               D3D12_GPU_DESCRIPTOR_HANDLE gbufferTable, ID3D12Resource* hashPrevious = nullptr,
                               ID3D12Resource* hashCurrent = nullptr)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS lights = s_LightUpload->GetGPUVirtualAddress() + slot * kLightStride;
        const D3D12_GPU_VIRTUAL_ADDRESS dummy = s_Dummy->GetGPUVirtualAddress();
        commands->SetComputeRootSignature(s_SampleRoot.Get());
        commands->SetComputeRootConstantBufferView(0, s_Constants->GetGPUVirtualAddress() + slot * kConstantStride);
        commands->SetComputeRootShaderResourceView(1, lights);
        commands->SetComputeRootShaderResourceView(2, s_FineCells->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(3, s_FineIndices->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(4,
                                                   scene != 0 ? scene : lights); // TLAS なし（M1 の検証）では使われない
        commands->SetComputeRootShaderResourceView(5, s_Reference ? s_Reference->GetGPUVirtualAddress() : lights);
        commands->SetComputeRootDescriptorTable(6, gbufferTable);
        commands->SetComputeRootUnorderedAccessView(7, s_Samples ? s_Samples->GetGPUVirtualAddress() : dummy);
        commands->SetComputeRootUnorderedAccessView(8, s_Accumulation ? s_Accumulation->GetGPUVirtualAddress() : dummy);
        commands->SetComputeRootUnorderedAccessView(9, s_Partials ? s_Partials->GetGPUVirtualAddress() : dummy);
        commands->SetComputeRootShaderResourceView(10, hashPrevious ? hashPrevious->GetGPUVirtualAddress() : lights);
        commands->SetComputeRootUnorderedAccessView(11, hashCurrent ? hashCurrent->GetGPUVirtualAddress() : dummy);
        commands->SetComputeRootUnorderedAccessView(12, s_TileList ? s_TileList->GetGPUVirtualAddress() : dummy);
        commands->SetComputeRootUnorderedAccessView(13, s_TraceList ? s_TraceList->GetGPUVirtualAddress() : dummy);
        commands->SetComputeRootUnorderedAccessView(14, s_WorldList ? s_WorldList->GetGPUVirtualAddress() : dummy);
        commands->SetComputeRootUnorderedAccessView(15,
                                                    s_TraceCounters ? s_TraceCounters->GetGPUVirtualAddress() : dummy);
        commands->SetComputeRootUnorderedAccessView(16,
                                                    s_DispatchArgs ? s_DispatchArgs->GetGPUVirtualAddress() : dummy);
    }

    static void UavBarrier(ID3D12GraphicsCommandList* commands, std::initializer_list<ID3D12Resource*> resources)
    {
        D3D12_RESOURCE_BARRIER list[8];
        UINT count = 0;
        for (ID3D12Resource* resource : resources)
        {
            if (resource && count < 8)
            {
                list[count++] = CD3DX12_RESOURCE_BARRIER::UAV(resource);
            }
        }
        if (count > 0)
        {
            commands->ResourceBarrier(count, list);
        }
    }

    static void Transition(vector<D3D12_RESOURCE_BARRIER>& barriers, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES target)
    {
        if (!resource || current == target)
        {
            return;
        }
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource, current, target));
        current = target;
    }

    static UINT BuildFlags(Mode mode, bool accumulate, bool denoise, bool guide)
    {
        UINT flags = 0;
        if (guide)
        {
            flags |= kFlagGuide;
        }
        if (mode == Mode::Stochastic && s_Settings.ScreenTrace)
        {
            flags |= kFlagScreenTrace;
        }
        if (denoise)
        {
            flags |= kFlagDenoised;
        }
        if (mode == Mode::Stochastic)
        {
            flags |= kFlagStochastic;
        }
        if (mode == Mode::Reference)
        {
            flags |= kFlagReference;
        }
        if (accumulate)
        {
            flags |= kFlagAccumulate;
        }
        if (accumulate && s_HasReference)
        {
            flags |= kFlagHasReference;
        }
        return flags;
    }

    // 解像度とメモリ上限から N を決める（1 / 2 / 4）
    static UINT ChooseSamplesPerPixel(UINT width, UINT height)
    {
        UINT requested =
            static_cast<UINT>(std::clamp(s_Settings.SamplesPerPixel, 1, static_cast<int>(kMaxSamplesPerPixel)));
        requested = requested >= 4 ? 4u : requested >= 2 ? 2u : 1u;
        const UINT64 perSample = static_cast<UINT64>(width) * height * 16ull;
        while (requested > 1 && perSample * requested > kSampleBudgetBytes)
        {
            requested /= 2;
        }
        return perSample * requested > kSampleBudgetBytes * 2 ? 0u : requested;
    }

    // 解像度に合わせてグリッドを確保（大きくなったときだけ作り直す。古いものは GPU が使い終わるまで保持）
    static bool EnsureGrid(UINT tilesX, UINT tilesY, UINT coarseCells)
    {
        const UINT64 tiles = static_cast<UINT64>(tilesX) * tilesY;
        const UINT64 fineCells = tiles * kSlices;
        const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (tiles > s_TileCapacity)
        {
            Retire(s_TileMasks);
            if (!CreateBuffer(s_TileMasks, sizeof(UINT) * 2 * tiles, D3D12_HEAP_TYPE_DEFAULT, uav,
                              D3D12_RESOURCE_STATE_COMMON, L"MegaLights Tile Slice Masks"))
            {
                s_TileCapacity = 0;
                return false;
            }
            s_TileMasksState = D3D12_RESOURCE_STATE_COMMON;
            s_TileCapacity = tiles;
        }
        if (fineCells > s_FineCellCapacity)
        {
            Retire(s_FineCells);
            if (!CreateBuffer(s_FineCells, sizeof(UINT) * 2 * fineCells, D3D12_HEAP_TYPE_DEFAULT, uav,
                              D3D12_RESOURCE_STATE_COMMON, L"MegaLights Fine Cells"))
            {
                s_FineCellCapacity = 0;
                return false;
            }
            s_FineCellsState = D3D12_RESOURCE_STATE_COMMON;
            s_FineCellCapacity = fineCells;
        }
        if (coarseCells > s_CoarseCellCapacity)
        {
            Retire(s_CoarseCells);
            if (!CreateBuffer(s_CoarseCells, sizeof(UINT) * 2 * coarseCells, D3D12_HEAP_TYPE_DEFAULT, uav,
                              D3D12_RESOURCE_STATE_COMMON, L"MegaLights Coarse Cells"))
            {
                s_CoarseCellCapacity = 0;
                return false;
            }
            s_CoarseCellsState = D3D12_RESOURCE_STATE_COMMON;
            s_CoarseCellCapacity = coarseCells;
        }
        return true;
    }

    static bool EnsureSamples(UINT width, UINT height, UINT samplesPerPixel)
    {
        const UINT64 count = static_cast<UINT64>(width) * height * samplesPerPixel;
        if (count > s_SampleCapacity)
        {
            Retire(s_Samples);
            if (!CreateBuffer(s_Samples, count * 16ull, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
                              L"MegaLights Samples"))
            {
                s_SampleCapacity = 0;
                return false;
            }
            s_SamplesState = D3D12_RESOURCE_STATE_COMMON;
            // M5：トレースリスト（サンプルと同じ要素数）
            Retire(s_TraceList);
            Retire(s_WorldList);
            const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (!CreateBuffer(s_TraceList, count * 4ull, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                              L"MegaLights Trace List") ||
                !CreateBuffer(s_WorldList, count * 4ull, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                              L"MegaLights World Trace List"))
            {
                s_Samples.Reset();
                s_TraceList.Reset();
                s_WorldList.Reset();
                s_SampleCapacity = 0;
                return false;
            }
            s_TraceListState = s_WorldListState = D3D12_RESOURCE_STATE_COMMON;
            s_SampleCapacity = count;
        }
        const UINT64 tiles = static_cast<UINT64>((width + 7) / 8) * ((height + 7) / 8);
        if (tiles > s_TileListCapacity)
        {
            Retire(s_TileList);
            if (!CreateBuffer(s_TileList, tiles * 4ull, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
                              L"MegaLights Tile List"))
            {
                s_TileListCapacity = 0;
                return false;
            }
            s_TileListState = D3D12_RESOURCE_STATE_COMMON;
            s_TileListCapacity = tiles;
        }
        s_Stats.SampleBufferMB = static_cast<double>(s_SampleCapacity * 24ull) / (1024.0 * 1024.0);
        return true;
    }

    // 累積・リファレンス・誤差の部分和（検証を有効にしたときだけ確保）
    static bool EnsureAccumulation(UINT width, UINT height)
    {
        const UINT64 pixels = static_cast<UINT64>(width) * height;
        const UINT64 groups = (pixels + 255) / 256;
        if (pixels > s_AccumulationCapacity)
        {
            Retire(s_Accumulation);
            Retire(s_Reference);
            Retire(s_Partials);
            for (auto& readback : s_PartialReadback)
            {
                Retire(readback);
            }
            const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            bool ok = CreateBuffer(s_Accumulation, pixels * 16ull, D3D12_HEAP_TYPE_DEFAULT, uav,
                                   D3D12_RESOURCE_STATE_COMMON, L"MegaLights Accumulation") &&
                      CreateBuffer(s_Reference, pixels * 16ull, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_COMMON, L"MegaLights Reference") &&
                      CreateBuffer(s_Partials, groups * 8ull, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON,
                                   L"MegaLights Error Partials");
            for (UINT frame = 0; ok && frame < kFrames; ++frame)
            {
                ok = CreateBuffer(s_PartialReadback[frame], groups * 8ull, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                                  L"MegaLights Error Readback");
                s_PartialPending[frame] = false;
            }
            if (!ok)
            {
                s_Accumulation.Reset();
                s_Reference.Reset();
                s_Partials.Reset();
                for (auto& readback : s_PartialReadback)
                {
                    readback.Reset();
                }
                s_AccumulationCapacity = 0;
                return false;
            }
            s_AccumulationState = s_ReferenceState = s_PartialsState = D3D12_RESOURCE_STATE_COMMON;
            s_AccumulationCapacity = pixels;
            s_AccumulatedFrames = 0;
            ClearReference();
        }
        return true;
    }

    // 累積のリセット判定：カメラ（ジッターを除く）・ライト・評価方式・解像度が変わったら 0 から
    static void UpdateAccumulationState(UINT width, UINT height, Mode mode, UINT samplesPerPixel, bool accumulate)
    {
        if (!accumulate)
        {
            s_AccumulatedFrames = 0;
            s_PreviousAccumulationHash = 0;
            return;
        }
        XMMATRIX view = XMMatrixIdentity();
        XMMATRIX projection = XMMatrixIdentity();
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        XMFLOAT4X4 v, p;
        XMStoreFloat4x4(&v, view);
        XMStoreFloat4x4(&p, projection);
        p._31 = p._32 = 0.0f; // TAA のジッターは累積で平均される
        uint64_t hash = s_LightHash;
        HashBytes(hash, &v, sizeof(v));
        HashBytes(hash, &p, sizeof(p));
        const UINT settings[] = {width,
                                 height,
                                 static_cast<UINT>(mode),
                                 samplesPerPixel,
                                 static_cast<UINT>(s_Settings.ReferenceRays),
                                 static_cast<UINT>(s_Settings.UniformMix * 100000.0f),
                                 static_cast<UINT>(s_Settings.Guide),
                                 static_cast<UINT>(s_Settings.ScreenTrace),
                                 static_cast<UINT>(s_Settings.HalfResolution)};
        HashBytes(hash, settings, sizeof(settings));
        if (hash != s_PreviousAccumulationHash || s_ResetRequested)
        {
            s_AccumulatedFrames = 0;
            s_Stats.ErrorValid = false;
        }
        s_PreviousAccumulationHash = hash;
        s_ResetRequested = false;
    }

    static void RunVerification(ID3D12GraphicsCommandList* commands, UINT slot, D3D12_GPU_VIRTUAL_ADDRESS scene,
                                D3D12_GPU_DESCRIPTOR_HANDLE gbufferTable, UINT width, UINT height)
    {
        vector<D3D12_RESOURCE_BARRIER> barriers;
        const D3D12_RESOURCE_STATES bothRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        // 現在の累積をリファレンスとして保存（前フレームまでの平均）
        if (s_SaveReferenceRequested && s_AccumulatedFrames > 0)
        {
            Transition(barriers, s_Accumulation.Get(), s_AccumulationState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(barriers, s_Reference.Get(), s_ReferenceState, D3D12_RESOURCE_STATE_COPY_DEST);
            commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            commands->CopyBufferRegion(s_Reference.Get(), 0, s_Accumulation.Get(), 0,
                                       static_cast<UINT64>(width) * height * 16ull);
            barriers.clear();
            s_HasReference = true;
            s_ReferenceWidth = width;
            s_ReferenceHeight = height;
            s_Stats.HasReference = true;
            s_Stats.ErrorValid = false;
        }
        s_SaveReferenceRequested = false;
        Transition(barriers, s_Accumulation.Get(), s_AccumulationState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(barriers, s_Reference.Get(), s_ReferenceState, bothRead);
        if (!barriers.empty())
        {
            commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

        if (!s_HasReference || s_AccumulatedFrames == 0 || !s_ErrorReduce)
        {
            return;
        }
        const UINT pixels = width * height;
        const UINT groups = (pixels + 255) / 256;
        barriers.clear();
        Transition(barriers, s_Partials.Get(), s_PartialsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!barriers.empty())
        {
            commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
        BindSampleRoot(commands, slot, scene, gbufferTable);
        commands->SetPipelineState(s_ErrorReduce.Get());
        commands->Dispatch(groups, 1, 1);
        barriers.clear();
        Transition(barriers, s_Partials.Get(), s_PartialsState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(s_Accumulation.Get()));
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        commands->CopyBufferRegion(s_PartialReadback[slot].Get(), 0, s_Partials.Get(), 0,
                                   static_cast<UINT64>(groups) * 8ull);
        s_PartialPending[slot] = true;
        s_PartialGroups[slot] = groups;
        s_PartialFrames[slot] = s_AccumulatedFrames;
    }

    static void Retire(ComPtr<ID3D12Resource>& resource)
    {
        if (resource)
        {
            s_Retired.push_back({resource, kFrames + 1});
        }
        resource.Reset();
    }

    static void RetireTick()
    {
        for (auto& retired : s_RetiredNrd)
        {
            --retired.second;
        }
        s_RetiredNrd.erase(remove_if(s_RetiredNrd.begin(), s_RetiredNrd.end(),
                                     [](const pair<unique_ptr<NrdDenoiser>, UINT>& retired) {
                                         return retired.second == 0;
                                     }),
                           s_RetiredNrd.end());
        for (auto& retired : s_Retired)
        {
            --retired.second;
        }
        s_Retired.erase(remove_if(s_Retired.begin(), s_Retired.end(),
                                  [](const pair<ComPtr<ID3D12Resource>, UINT>& retired) {
                                      return retired.second == 0;
                                  }),
                        s_Retired.end());
    }

    // 同じスロットの前回分（kFrames フレーム前。GPU は完了済み）を読む
    static void ReadStats()
    {
        if (s_TraceReadback && s_TraceReadbackPending[s_Slot])
        {
            const CD3DX12_RANGE range(s_Slot * 16, s_Slot * 16 + 16);
            UINT* mapped = nullptr;
            if (SUCCEEDED(s_TraceReadback->Map(0, &range, reinterpret_cast<void**>(&mapped))))
            {
                const UINT* values = reinterpret_cast<const UINT*>(reinterpret_cast<const BYTE*>(mapped) + s_Slot * 16);
                s_Stats.ActiveTiles = values[0];
                s_Stats.TraceRequests = values[1];
                s_Stats.WorldTraces = values[2];
                s_Stats.ScreenHits = values[3];
                s_Stats.TotalTiles = s_TraceTotalTiles[s_Slot];
                s_Stats.TraceStatsValid = true;
                const CD3DX12_RANGE noWrite(0, 0);
                s_TraceReadback->Unmap(0, &noWrite);
            }
            s_TraceReadbackPending[s_Slot] = false;
        }
        if (s_Readback && s_ReadbackPending[s_Slot])
        {
            const CD3DX12_RANGE range(s_Slot * kReadbackStride, s_Slot * kReadbackStride + 16 + kLightVisibleBytes);
            UINT* mapped = nullptr;
            if (SUCCEEDED(s_Readback->Map(0, &range, reinterpret_cast<void**>(&mapped))))
            {
                const UINT* values =
                    reinterpret_cast<const UINT*>(reinterpret_cast<const BYTE*>(mapped) + s_Slot * kReadbackStride);
                // ライトのオクルージョン：見えなかったライトのカウンタを進め、見えたら 0 に
                const UINT* visible = values + 4;
                const auto& lights = s_SlotLights[s_Slot];
                s_OccludedCount = 0;
                for (size_t index = 0; index < lights.size() && index < kMaxLights; ++index)
                {
                    const EntityID entity = lights[index];
                    if (entity >= s_OccludedFrames.size())
                    {
                        s_OccludedFrames.resize(static_cast<size_t>(entity) + 1, 0);
                    }
                    const bool seen = ((visible[index >> 5] >> (index & 31)) & 1u) != 0u;
                    uint8_t& frames = s_OccludedFrames[entity];
                    frames = seen ? 0 : static_cast<uint8_t>((std::min)(frames + 1, 255));
                    if (frames >= kOcclusionFrames)
                    {
                        ++s_OccludedCount;
                    }
                }
                s_Stats.CoarseIndices = values[0];
                s_Stats.FineIndices = values[1];
                s_Stats.OverflowCells = values[2];
                s_Stats.MaxLightsPerCell = values[3];
                const CD3DX12_RANGE noWrite(0, 0);
                s_Readback->Unmap(0, &noWrite);
            }
            s_ReadbackPending[s_Slot] = false;
        }
        if (s_PartialReadback[s_Slot] && s_PartialPending[s_Slot])
        {
            const UINT groups = s_PartialGroups[s_Slot];
            const CD3DX12_RANGE range(0, static_cast<SIZE_T>(groups) * 8);
            float* mapped = nullptr;
            if (SUCCEEDED(s_PartialReadback[s_Slot]->Map(0, &range, reinterpret_cast<void**>(&mapped))))
            {
                double error = 0.0, reference = 0.0;
                for (UINT group = 0; group < groups; ++group)
                {
                    error += mapped[group * 2 + 0];
                    reference += mapped[group * 2 + 1];
                }
                const CD3DX12_RANGE noWrite(0, 0);
                s_PartialReadback[s_Slot]->Unmap(0, &noWrite);
                s_Stats.RelativeRmse = reference > 0.0 ? sqrt(error / reference) : 0.0;
                s_Stats.ErrorFrames = s_PartialFrames[s_Slot];
                s_Stats.ErrorValid = true;
            }
            s_PartialPending[s_Slot] = false;
        }
    }

    static void WriteConstants(bool enabled, const D3D12_RESOURCE_DESC& depthDesc, UINT tilesX, UINT tilesY,
                               UINT coarseX, UINT coarseY, UINT samplesPerPixel, UINT flags)
    {
        if (!s_MappedConstants)
        {
            return;
        }
        Constants constants{};
        if (enabled)
        {
            XMMATRIX view = XMMatrixIdentity();
            XMMATRIX projection = XMMatrixIdentity();
            Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
            XMFLOAT4X4 p;
            XMStoreFloat4x4(&p, projection);
            XMStoreFloat4x4(&constants.WorldToView, XMMatrixTranspose(view));
            XMStoreFloat4x4(&constants.ViewToWorld, XMMatrixTranspose(XMMatrixInverse(nullptr, view)));
            constants.Projection = {p._11, p._22, p._31, p._32};
            // z = _43 / (ndc - _33)。ndc = 0 と 1 の距離から手前 / 奥を求める（逆 Z でも成り立つ）
            const float zAtZero = fabsf(p._33) > 1.0e-6f ? p._43 / (0.0f - p._33) : 0.1f;
            const float zAtOne = fabsf(1.0f - p._33) > 1.0e-6f ? p._43 / (1.0f - p._33) : 1.0e6f;
            const bool reversed = zAtZero > zAtOne;
            const float nearZ = (std::max)(0.001f, reversed ? zAtOne : zAtZero);
            const float farZ =
                (std::max)(nearZ * 2.0f,
                           std::isfinite(reversed ? zAtZero : zAtOne) ? (reversed ? zAtZero : zAtOne) : 1.0e6f);
            const float sliceFar = std::clamp(s_Settings.SliceFar, nearZ * 4.0f, farZ);
            constants.Depth = {p._33, p._43, nearZ, static_cast<float>(kSlices) / logf(sliceFar / nearZ)};
            constants.Depth2 = {reversed ? 0.0f : 1.0f, farZ, static_cast<float>((std::max)(s_Settings.HeatmapMax, 1)),
                                std::clamp(s_Settings.GuideMinWeight, 0.001f, 1.0f)};
            constants.Grid = {tilesX, tilesY, kSlices, 1u};
            constants.Counts = {s_LightCount, static_cast<UINT>(std::clamp(s_Settings.DebugView, 0, 9)), kFineCapacity,
                                kCoarseCapacity};
            constants.Screen = {static_cast<UINT>(depthDesc.Width), depthDesc.Height, coarseX, coarseY};
            constants.Stochastic = {flags, samplesPerPixel, static_cast<UINT>(s_FrameCounter), s_AccumulatedFrames};
            // ハッシュのビットを残す確率（ML7）は Grid.w の 8〜15bit（0..255）。bit0 はグリッド有効
            constants.Grid.w |= static_cast<UINT>(std::clamp(s_Settings.GuideKeep, 0.0f, 1.0f) * 255.0f + 0.5f) << 8;
            constants.SampleGrid = {s_SampleWidth ? s_SampleWidth : static_cast<UINT>(depthDesc.Width),
                                    s_SampleHeight ? s_SampleHeight : depthDesc.Height, s_SampleScale,
                                    s_SampleScale > 1 ? static_cast<UINT>(s_FrameCounter & 3u) : 0u};
            constants.ScreenTrace = {static_cast<float>(std::clamp(s_Settings.ScreenTraceSteps, 1, 64)),
                                     std::clamp(s_Settings.ScreenTraceMaxPixels, 1.0f, 1024.0f),
                                     std::clamp(s_Settings.ScreenTraceThickness, 0.001f, 10.0f),
                                     std::clamp(s_Settings.ScreenTraceSelfPixels, 0.0f, 16.0f)};
            constants.StochasticParams = {std::clamp(s_Settings.UniformMix, 0.001f, 1.0f),
                                          std::clamp(Raytracing::GetSettings().NormalBias, 0.001f, 0.1f),
                                          (std::max)(s_Settings.InvPdfDisplayMax, 2.0f),
                                          static_cast<float>(std::clamp(s_Settings.ReferenceRays, 1, 64))};
        }
        memcpy(s_MappedConstants + s_Slot * kConstantStride, &constants, sizeof(constants));
    }

    static void ShutdownPipelines()
    {
        s_Available = false;
        s_SampleAvailable = false;
        s_ClearCounters.Reset();
        s_TileSliceMask.Reset();
        s_CoarseCull.Reset();
        s_FineCull.Reset();
        s_Root.Reset();
        ResetSamplePipelines();
        s_TileList.Reset();
        s_TraceList.Reset();
        s_WorldList.Reset();
        s_TileListCapacity = 0;
        s_NrdAvailable = false;
        s_NrdRoot.Reset();
        s_NrdPack.Reset();
        s_NrdUnpack.Reset();
        s_NrdHeap.Reset();
        s_Nrd.reset();
        s_RetiredNrd.clear();
        s_NrdNoisy.Reset();
        s_NrdWidth = s_NrdHeight = 0;
        s_HashAvailable = false;
        s_BuildHash.Reset();
        s_Hash[0].Reset();
        s_Hash[1].Reset();
        s_HashTilesX = s_HashTilesY = 0;
        s_HashValid = false;
        s_HashDisplay = nullptr;
        s_DenoiseAvailable = false;
        s_DenoiseRoot.Reset();
        s_Temporal.Reset();
        s_ATrous.Reset();
        s_Raw.Reset();
        s_FilterA.Reset();
        s_FilterB.Reset();
        for (UINT index = 0; index < 2; ++index)
        {
            s_HistoryColor[index].Reset();
            s_HistoryMoments[index].Reset();
        }
        s_DenoiseCapacity = 0;
        s_DenoiseWidth = s_DenoiseHeight = 0;
        s_DenoiseHistoryValid = false;
        s_DenoiseDone = false;
        s_TileMasks.Reset();
        s_CoarseCells.Reset();
        s_CoarseIndices.Reset();
        s_FineCells.Reset();
        s_FineIndices.Reset();
        s_Counters.Reset();
        s_LightVisible.Reset();
        s_LightVisibleState = D3D12_RESOURCE_STATE_COMMON;
        for (auto& lights : s_SlotLights)
        {
            lights.clear();
        }
        s_OccludedFrames.clear();
        s_OccludedCount = 0;
        s_Readback.Reset();
        s_Samples.Reset();
        s_Accumulation.Reset();
        s_Reference.Reset();
        s_Partials.Reset();
        for (auto& readback : s_PartialReadback)
        {
            readback.Reset();
        }
        s_Retired.clear();
        s_TileCapacity = s_FineCellCapacity = s_SampleCapacity = s_AccumulationCapacity = 0;
        s_CoarseCellCapacity = 0;
        s_HasReference = false;
        s_AccumulatedFrames = 0;
        for (UINT frame = 0; frame < kFrames; ++frame)
        {
            s_SlotBuilt[frame] = false;
            s_SlotSampled[frame] = false;
            s_SlotDenoise[frame] = false;
            s_SlotHash[frame] = false;
            s_TraceReadbackPending[frame] = false;
            s_SlotVolumetric[frame] = false;
            s_SlotForward[frame] = false;
            s_ForwardDescriptorsReady[frame] = false;
            s_ReadbackPending[frame] = false;
            s_PartialPending[frame] = false;
        }
        s_Stats = {};
    }

    static inline Settings s_Settings{};
    static inline Stats s_Stats{};
    static inline bool s_Available = false;
    static inline bool s_SampleAvailable = false;
    static inline ComPtr<ID3D12Device> s_Device;
    static inline ComPtr<ID3D12RootSignature> s_Root, s_SampleRoot;
    static inline ComPtr<ID3D12PipelineState> s_ClearCounters, s_TileSliceMask, s_CoarseCull, s_FineCull;
    static inline ComPtr<ID3D12PipelineState> s_SampleLights, s_ScreenTrace, s_WorldTrace, s_ErrorReduce;
    static inline ComPtr<ID3D12Resource> s_Constants, s_LightUpload, s_Dummy;
    static inline ComPtr<ID3D12Resource> s_TileMasks, s_CoarseCells, s_CoarseIndices, s_FineCells, s_FineIndices,
        s_Counters, s_Readback;
    // ---- ライトのオクルージョン
    static constexpr UINT kLightVisibleBytes = 4096 / 8;
    static constexpr UINT kReadbackStride = 1024;
    static constexpr uint8_t kOcclusionFrames = 3; // この回数続けて見えなければ「隠れている」
    static inline ComPtr<ID3D12Resource> s_LightVisible;
    static inline D3D12_RESOURCE_STATES s_LightVisibleState = D3D12_RESOURCE_STATE_COMMON;
    static inline vector<EntityID> s_SlotLights[kFrames];
    static inline vector<uint8_t> s_OccludedFrames;
    static inline UINT s_OccludedCount = 0;
    static inline ComPtr<ID3D12Resource> s_Samples, s_Accumulation, s_Reference, s_Partials;
    static inline ComPtr<ID3D12Resource> s_PartialReadback[kFrames];
    static inline D3D12_RESOURCE_STATES s_TileMasksState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_CoarseCellsState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_CoarseIndicesState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_FineCellsState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_FineIndicesState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_CountersState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_SamplesState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_AccumulationState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_ReferenceState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_PartialsState = D3D12_RESOURCE_STATE_COMMON;
    static inline UINT64 s_TileCapacity = 0, s_FineCellCapacity = 0, s_SampleCapacity = 0, s_AccumulationCapacity = 0;
    static inline UINT s_CoarseCellCapacity = 0;
    static inline vector<pair<ComPtr<ID3D12Resource>, UINT>> s_Retired;
    static inline BYTE* s_MappedConstants = nullptr;
    static inline BYTE* s_MappedLights = nullptr;
    static inline UINT s_Slot = 0;
    static inline UINT s_LightCount = 0;
    static inline uint64_t s_LightHash = 0;
    static inline uint64_t s_LastSerial = 0;
    static inline uint64_t s_FrameCounter = 0;
    static inline uint64_t s_PreviousAccumulationHash = 0;
    static inline bool s_HasSerial = false;
    static inline bool s_SlotBuilt[kFrames]{};
    static inline bool s_SlotSampled[kFrames]{};
    static inline bool s_ReadbackPending[kFrames]{};
    static inline bool s_PartialPending[kFrames]{};
    static inline UINT s_PartialGroups[kFrames]{};
    static inline UINT s_PartialFrames[kFrames]{};
    static inline UINT s_AccumulatedFrames = 0;
    static inline bool s_HasReference = false;
    static inline UINT s_ReferenceWidth = 0, s_ReferenceHeight = 0;
    static inline bool s_ResetRequested = false;
    static inline bool s_SaveReferenceRequested = false;
    // ---- M3
    static inline bool s_DenoiseAvailable = false;
    static inline bool s_ShadeFailed = false;
    static inline ComPtr<ID3D12RootSignature> s_DenoiseRoot;
    static inline ComPtr<ID3D12PipelineState> s_Temporal, s_ATrous, s_ShadePso;
    static inline DXGI_FORMAT s_ShadeFormat = DXGI_FORMAT_UNKNOWN;
    static inline ComPtr<ID3D12Resource> s_Raw, s_FilterA, s_FilterB, s_HistoryColor[2], s_HistoryMoments[2];
    static inline D3D12_RESOURCE_STATES s_RawState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_FilterAState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_FilterBState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_HistoryColorState[2] = {D3D12_RESOURCE_STATE_COMMON,
                                                                  D3D12_RESOURCE_STATE_COMMON};
    static inline D3D12_RESOURCE_STATES s_HistoryMomentsState[2] = {D3D12_RESOURCE_STATE_COMMON,
                                                                    D3D12_RESOURCE_STATE_COMMON};
    static inline UINT64 s_DenoiseCapacity = 0;
    static inline UINT s_DenoiseWidth = 0, s_DenoiseHeight = 0;
    static inline UINT s_Width = 0, s_Height = 0;
    static inline UINT s_HistoryIndex = 0;
    static inline bool s_DenoiseHistoryValid = false;
    static inline bool s_DenoiseResetRequested = false;
    static inline bool s_DenoiseDone = false;
    static inline bool s_FinalIsA = true;
    static inline EntityID s_DenoiseCamera = g_kINVALID_ENTITY;
    static inline uint64_t s_LastDenoiseFrame = 0;
    static inline uint64_t s_PreviousLightHash = 0;
    static inline XMFLOAT4 s_PreviousViewZRow = {0.0f, 0.0f, 1.0f, 0.0f};
    static inline bool s_SlotDenoise[kFrames]{};
    // ---- M4
    static inline bool s_HashAvailable = false;
    static inline ComPtr<ID3D12PipelineState> s_BuildHash;
    static inline ComPtr<ID3D12Resource> s_Hash[2];
    static inline D3D12_RESOURCE_STATES s_HashState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    static inline ID3D12Resource* s_HashDisplay = nullptr;
    static inline UINT s_HashIndex = 0;
    static inline UINT s_HashTilesX = 0, s_HashTilesY = 0;
    static inline bool s_HashValid = false;
    static inline bool s_HashResetRequested = false;
    static inline uint64_t s_HashLastFrame = 0;
    static inline EntityID s_HashCamera = g_kINVALID_ENTITY;
    static inline bool s_SlotHash[kFrames]{};
    // ---- M5
    static inline ComPtr<ID3D12PipelineState> s_ClearTraceCounters, s_ClassifyTiles, s_ArgsTiles, s_ArgsTrace,
        s_ArgsWorld;
    static inline ComPtr<ID3D12CommandSignature> s_DispatchSignature;
    static inline ComPtr<ID3D12Resource> s_TileList, s_TraceList, s_WorldList, s_TraceCounters, s_DispatchArgs,
        s_TraceReadback;
    static inline D3D12_RESOURCE_STATES s_TileListState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_TraceListState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_WorldListState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_TraceCountersState = D3D12_RESOURCE_STATE_COMMON;
    static inline D3D12_RESOURCE_STATES s_DispatchArgsState = D3D12_RESOURCE_STATE_COMMON;
    static inline UINT64 s_TileListCapacity = 0;
    static inline bool s_TraceReadbackPending[kFrames]{};
    static inline UINT s_TraceTotalTiles[kFrames]{};
    // ---- M6
    static constexpr UINT kNrdDescriptorsPerFrame = 3;
    static inline bool s_NrdAvailable = false;
    static inline ComPtr<ID3D12RootSignature> s_NrdRoot;
    static inline ComPtr<ID3D12PipelineState> s_NrdPack, s_NrdUnpack;
    static inline ComPtr<ID3D12DescriptorHeap> s_NrdHeap;
    static inline UINT s_NrdIncrement = 0;
    static inline unique_ptr<NrdDenoiser> s_Nrd;
    static inline vector<pair<unique_ptr<NrdDenoiser>, UINT>> s_RetiredNrd;
    static inline ComPtr<ID3D12Resource> s_NrdNoisy;
    static inline D3D12_RESOURCE_STATES s_NrdNoisyState = D3D12_RESOURCE_STATE_COMMON;
    static inline UINT s_NrdWidth = 0, s_NrdHeight = 0;
    static inline uint64_t s_NrdGuideSerial = 0;
    static inline uint64_t s_NrdLastFrame = 0;
    static inline bool s_SlotVolumetric[kFrames]{};
    static inline vector<uint8_t> s_EntityInGrid;
    static inline vector<EntityID> s_EntityMarked;
    // ---- M7 / 半解像度
    static inline bool s_SlotForward[kFrames]{};
    static inline bool s_ForwardDescriptorsReady[kFrames]{};
    static inline ID3D12DescriptorHeap* s_EngineHeap = nullptr;
    static inline UINT s_EngineHeapStart = 0, s_EngineIncrement = 0;
    static inline UINT s_SampleWidth = 0, s_SampleHeight = 0, s_SampleScale = 1;
};
