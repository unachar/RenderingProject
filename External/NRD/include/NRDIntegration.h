// NRD Integration Stub - Replace with actual NRD from https://github.com/NVIDIA/Real-Time-Denoiser

#pragma once

#ifndef NRD_INTEGRATION_H
#define NRD_INTEGRATION_H

#include "NRD.h"

typedef enum NRD_BackendAPI
{
    NRD_BackendAPI_D3D11 = 0,
    NRD_BackendAPI_D3D12 = 1,
    NRD_BackendAPI_Vulkan = 2,
} NRD_BackendAPI;

typedef struct NRD_D3D12_DeviceDesc
{
    void* device;           // ID3D12Device*
    void* commandQueue;     // ID3D12CommandQueue*
    void* commandList;      // ID3D12GraphicsCommandList*
    void* descriptorHeap;   // ID3D12DescriptorHeap* (CBV_SRV_UAV)
    uint32_t descriptorHeapStartIndex;
    uint32_t descriptorHeapSize;
} NRD_D3D12_DeviceDesc;

typedef struct NRD_D3D12_DispatchDesc
{
    NRD_DispatchDesc base;
    void* commandList;      // ID3D12GraphicsCommandList*
    uint32_t descriptorHeapStartIndex;
} NRD_D3D12_DispatchDesc;

NRD_Result NRD_D3D12_CreateInstance(NRD_Denoiser denoiser, const NRD_D3D12_DeviceDesc* deviceDesc, NRD_Instance* instance);
void NRD_D3D12_DestroyInstance(NRD_Instance instance);
NRD_Result NRD_D3D12_SetMethodSettings(NRD_Instance instance, NRD_Denoiser denoiser, const void* settings);
NRD_Result NRD_D3D12_Dispatch(NRD_Instance instance, const NRD_D3D12_DispatchDesc* dispatchDesc);
NRD_Result NRD_D3D12_GetComputeResourceUsage(NRD_Instance instance, NRD_Denoiser denoiser, uint32_t width, uint32_t height, NRD_ResourceDesc* tempResources, uint32_t* tempResourceCount);
NRD_Result NRD_D3D12_GetScratchResourceUsage(NRD_Instance instance, NRD_Denoiser denoiser, uint32_t width, uint32_t height, uint64_t* scratchSize);

// Helper macros for D3D12 integration
#define NRD_D3D12_RESOURCE_STATE_COMMON D3D12_RESOURCE_STATE_COMMON
#define NRD_D3D12_RESOURCE_STATE_UNORDERED_ACCESS D3D12_RESOURCE_STATE_UNORDERED_ACCESS
#define NRD_D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
#define NRD_D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE

#endif // NRD_INTEGRATION_H