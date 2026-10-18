#pragma once

#include "pch.h"
#include "nrddenoiser.h"

class Raytracing
{
public:
    enum class Mode { ReflectionsOnly, Full };
    enum class DenoiserType { Nrd, Optix };
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
    };

    static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT increment);
    static void Shutdown();
    static void Execute(ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* depth, ID3D12Resource* normal,
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv, D3D12_GPU_DESCRIPTOR_HANDLE normalSrv,
        UINT frameIndex);
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
    static UINT GetDenoiserDispatchCount();
    static UINT GetAmbientDenoiserDispatchCount();
    static bool AreReflectionsActive();
    static bool IsGiActive();
    static D3D12_GPU_DESCRIPTOR_HANDLE GetGiSrv();
    static D3D12_GPU_DESCRIPTOR_HANDLE GetReflectionSrv();
    static D3D12_GPU_VIRTUAL_ADDRESS GetLightingConstantsAddress(UINT frameIndex);
    // TLAS built by Execute() this frame (0 when ray tracing did not run).
    static D3D12_GPU_VIRTUAL_ADDRESS GetSceneAddress();
    // NRD の共有ガイド（Motion / NormalRoughness / ViewZ。PIXEL_SHADER_RESOURCE）とカメラ情報。
    // serial は最後にガイドを書いた Execute の番号（同じフレームのものかを呼び出し側で判定する）
    static bool GetNrdGuides(ID3D12Resource* guides[3], NrdFrame& frame, uint64_t& serial);
    static void BeginLightList();
    static void SetLightShadowFlag(UINT index, bool castShadow);
    static void BindGlass(ID3D12GraphicsCommandList* commandList);
    static void BeginGlassRead(ID3D12GraphicsCommandList* commandList);
    static void EndGlassRead(ID3D12GraphicsCommandList* commandList);
    static void BindLighting(ID3D12GraphicsCommandList* commandList, UINT sceneRootIndex, UINT constantsRootIndex);
};
