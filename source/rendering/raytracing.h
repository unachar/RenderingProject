#pragma once

#include "pch.h"

class Raytracing
{
public:
    enum class Mode { ReflectionsOnly, Full };
    struct Settings
    {
        Mode RenderMode = Mode::ReflectionsOnly;
        bool Enabled = true;
        bool Shadows = true;
        bool AmbientOcclusion = true;
        bool Reflections = true;
        bool Denoise = true;
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
    static D3D12_GPU_DESCRIPTOR_HANDLE GetReflectionSrv();
    static D3D12_GPU_VIRTUAL_ADDRESS GetLightingConstantsAddress(UINT frameIndex);
    static void BeginLightList();
    static void SetLightShadowFlag(UINT index, bool castShadow);
    static void BindLighting(ID3D12GraphicsCommandList* commandList, UINT sceneRootIndex, UINT constantsRootIndex);
};
