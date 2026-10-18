#pragma once

#include "pch.h"
#include "renderersettings.h"

class SpatialUpscaler
{
  public:
    static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* descriptorHeap, UINT descriptorIncrement);
    static bool Resize(UINT outputWidth, UINT outputHeight, DXGI_FORMAT format);
    static void Shutdown();

    static bool IsAvailable(UpscaleMode mode);
    static bool IsScaleSupported(UpscaleMode mode, UINT inputWidth, UINT inputHeight, UINT outputWidth,
                                 UINT outputHeight);
    static bool Execute(ID3D12GraphicsCommandList* commandList, D3D12_GPU_DESCRIPTOR_HANDLE inputSrv,
                        ID3D12Resource* output, D3D12_GPU_DESCRIPTOR_HANDLE outputUav, UINT inputWidth,
                        UINT inputHeight, UINT outputWidth, UINT outputHeight, UpscaleMode mode,
                        ID3D12Resource* input = nullptr, // 入力テクスチャ（実サイズを NIS / FSR に渡すため）
                        D3D12_GPU_DESCRIPTOR_HANDLE outputSrv =
                            D3D12_GPU_DESCRIPTOR_HANDLE{}); // 出力の SRV（NIS の 3 段以上で中間に使う）

    // NIS の段数（1 段は最大 2 倍まで）
    static UINT NisPassCount(UINT inputWidth, UINT inputHeight, UINT outputWidth, UINT outputHeight);

  private:
    static bool CreateRootSignature();
    static bool CreatePipeline(const wchar_t* path, ComPtr<ID3D12PipelineState>& output);
    static bool CreateNisCoefficientBuffers();
    static D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index);
    static D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT index);
};
