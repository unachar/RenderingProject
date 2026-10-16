#pragma once

#include "pch.h"
#include "ecs.h"

class DlssUpscaler
{
public:
    static void Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT descriptorIncrement);
    static void Shutdown();
    static bool IsAvailable();
    static bool IsActive();
    static const char* GetStatus();
    static bool IsScaleSupported(UINT inputWidth, UINT inputHeight, UINT outputWidth, UINT outputHeight);
    static void PrepareFrame(EntityID camera);
    static bool IsRequested();
    static XMFLOAT2 GetJitter();
    static bool Execute(ID3D12GraphicsCommandList* commands, UINT frameIndex,
        ID3D12Resource* color, D3D12_GPU_DESCRIPTOR_HANDLE colorSrv,
        ID3D12Resource* depth, ID3D12Resource* velocity, D3D12_GPU_DESCRIPTOR_HANDLE velocitySrv,
        ID3D12Resource* output, D3D12_GPU_DESCRIPTOR_HANDLE outputUav);
};

