#pragma once

#include "pch.h"
#include "nrddenoiser.h"

class Raytracing
{
  public:
    enum class Mode
    {
        ReflectionsOnly,
        Full
    };
    enum class DenoiserType
    {
        Nrd,
        Optix
    };

    struct Settings
    {
        Mode RenderMode = Mode::ReflectionsOnly;
        bool Enabled = true;
        bool Shadows = true;
        bool AmbientOcclusion = true;
        bool Reflections = true;
        bool Denoise = true;
        bool StableNrdProfile = true;
        bool StableSampling = true;
        DenoiserType RadianceDenoiser = DenoiserType::Nrd;
        int Samples = 2;
        float ShadowDistance = 500.0f;
        float AmbientRadius = 1.5f;
        float NormalBias = 0.015f;
        float SunAngularRadius = 0.00465f;
        // BLAS: 静的 BLAS をビルド後に圧縮（コンパクション）し、VRAM を削減する。
        bool CompactStaticBlas = true;
        // BLAS: 変形 BLAS は通常リフィット（更新）し、この回数ごとにフルビルドで BVH 品質を回復する。0 で無効。
        int DeformingBlasRebuildInterval = 32;
        // 1 フレームあたりの変形 BLAS フルビルド数の上限（スパイク分散）。
        int DeformingBlasRebuildsPerFrame = 2;
        // BLAS 入力頂点を R16G16B16A16_SNORM へ量子化する（バウンディングボックス正規化 + Transform3x4 で復元）。
        bool QuantizeBlasVertices = true;
        // 量子化誤差（ワールド空間）の許容上限。これを超えるメッシュは 32bit 頂点のまま構築する。
        float BlasQuantizationMaxError = 0.0005f;
    };

    struct BlasStatistics
    {
        UINT BottomLevels = 0;
        UINT Compacted = 0;
        UINT Quantized = 0;
        UINT RefitsThisFrame = 0;
        UINT RebuildsThisFrame = 0;
        UINT64 ResultBytes = 0;
        UINT64 CompactionSavedBytes = 0;
    };

    static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT increment);
    static void Shutdown();
    static void Execute(ID3D12GraphicsCommandList* commandList, ID3D12Resource* depth, ID3D12Resource* normal,
                        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv, D3D12_GPU_DESCRIPTOR_HANDLE normalSrv, UINT frameIndex);
    static bool IsAvailable();
    static bool UsesRaytracedShadows();
    static bool UsesRaytracedAmbientOcclusion();
    static bool IsActive();
    static const char* GetStatus();
    static Settings& GetSettings();
    static D3D12_GPU_DESCRIPTOR_HANDLE GetResultSrv();
    static ID3D12Resource* GetResultResource();
    static XMFLOAT4 GetSunDirection();
    static UINT GetInstanceCount();
    static BlasStatistics GetBlasStatistics();
    static UINT GetDenoiserDispatchCount();
    static UINT GetAmbientDenoiserDispatchCount();
    static bool AreReflectionsActive();
    static bool IsGiActive();
    static D3D12_GPU_DESCRIPTOR_HANDLE GetGiSrv();
    static D3D12_GPU_DESCRIPTOR_HANDLE GetReflectionSrv();
    static D3D12_GPU_VIRTUAL_ADDRESS GetLightingConstantsAddress(UINT frameIndex);
    // このフレームの Execute() が構築した TLAS（レイトレーシング未実行時は0）。
    static D3D12_GPU_VIRTUAL_ADDRESS GetSceneAddress();
    // NRD の共有ガイド（Motion / NormalRoughness / ViewZ。PIXEL_SHADER_RESOURCE）とカメラ情報。
    // serial は最後にガイドを書いた Execute の番号（同じフレームのものかを呼び出し側で判定する）
    static bool GetNrdGuides(ID3D12Resource* guides[3], NrdFrame& frame, uint64_t& serial);
    static void BeginLightList();
    static void SetLightShadowFlag(UINT index, bool castShadow);
    static void BindGlass(ID3D12GraphicsCommandList* commandList);
    static void BeginGlassRead(ID3D12GraphicsCommandList* commandList);
    static bool IsReadingSkinnedVerticesForGlass(ID3D12Resource* resource);
    static void EndGlassRead(ID3D12GraphicsCommandList* commandList);
    static void BindLighting(ID3D12GraphicsCommandList* commandList, UINT sceneRootIndex, UINT constantsRootIndex);
};
