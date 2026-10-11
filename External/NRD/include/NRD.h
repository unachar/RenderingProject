// NRD Stub Header - Replace with actual NRD from https://github.com/NVIDIA/Real-Time-Denoiser

#pragma once

#ifndef NRD_H
#define NRD_H

#include <stdint.h>
#include <stddef.h>

typedef enum NRD_Result
{
    NRD_Success = 0,
    NRD_Fail = -1,
    NRD_InvalidArgument = -2,
    NRD_Unsupported = -3,
    NRD_NotInitialized = -4,
} NRD_Result;

typedef enum NRD_Denoiser
{
    NRD_Denoiser_REBLUR = 0,      // Diffuse/Specular denoising
    NRD_Denoiser_SIGMA = 1,       // Shadow denoising
    NRD_Denoiser_RELAX = 2,       // Specular reflection denoising
    NRD_Denoiser_COUNT,
} NRD_Denoiser;

typedef enum NRD_ResourceType
{
    NRD_ResourceType_Texture2D = 0,
    NRD_ResourceType_Texture2DArray = 1,
    NRD_ResourceType_Buffer = 2,
    NRD_ResourceType_AccelerationStructure = 3,
} NRD_ResourceType;

typedef enum NRD_Format
{
    NRD_Format_R8_Unorm = 0,
    NRD_Format_R16_Float = 1,
    NRD_Format_R16G16_Float = 2,
    NRD_Format_R16G16B16A16_Float = 3,
    NRD_Format_R32_Float = 4,
    NRD_Format_R32G32_Float = 5,
    NRD_Format_R32G32B32A32_Float = 6,
    NRD_Format_R8G8B8A8_Unorm = 7,
    NRD_Format_R10G10B10A2_Unorm = 8,
    NRD_Format_D32_Float = 9,
    NRD_Format_D24_Unorm_S8_Uint = 10,
} NRD_Format;

typedef struct NRD_ResourceDesc
{
    NRD_ResourceType type;
    NRD_Format format;
    uint32_t width;
    uint32_t height;
    uint32_t depthOrArraySize;
    uint32_t mipLevels;
    uint32_t sampleCount;
    uint32_t flags;
    const void* nativeObject; // ID3D12Resource* for D3D12
} NRD_ResourceDesc;

typedef struct NRD_CommonSettings
{
    bool enableReferenceAccumulation;
    bool enableVarianceEstimation;
    float minDiffuseRadianceFiltering;
    float minSpecularRadianceFiltering;
    float minShadowTranslucencyFiltering;
} NRD_CommonSettings;

typedef struct NRD_ReblurSettings
{
    float sigma;
    float minLuminanceWeight;
    float temporalStability;
    float depthThreshold;
    float normalThreshold;
} NRD_ReblurSettings;

typedef struct NRD_SigmaSettings
{
    float sigma;
    float minLuminanceWeight;
    float temporalStability;
    float depthThreshold;
    float normalThreshold;
} NRD_SigmaSettings;

typedef struct NRD_RelaxSettings
{
    float sigma;
    float minLuminanceWeight;
    float temporalStability;
    float depthThreshold;
    float normalThreshold;
    float roughnessThreshold;
} NRD_RelaxSettings;

typedef struct NRD_DispatchDesc
{
    uint32_t width;
    uint32_t height;
    const NRD_ResourceDesc* inColor;
    const NRD_ResourceDesc* inNormal;
    const NRD_ResourceDesc* inDepth;
    const NRD_ResourceDesc* inMotionVectors;
    const NRD_ResourceDesc* inRoughness;
    const NRD_ResourceDesc* inSpecularHitDistance;
    const NRD_ResourceDesc* inDiffuseHitDistance;
    const NRD_ResourceDesc* inViewZ;
    const NRD_ResourceDesc* inShadowTranslucency;
    const NRD_ResourceDesc* inShadowData;
    const NRD_ResourceDesc* inShadowNormal;
    const NRD_ResourceDesc* inPrevDenoisedColor;
    const NRD_ResourceDesc* outDenoisedColor;
    const NRD_ResourceDesc* outDiffuseRadiance;
    const NRD_ResourceDesc* outSpecularRadiance;
    const NRD_ResourceDesc* outShadowTranslucency;
    float frameTimeDelta;
    const float* cameraJitter;
    const float* cameraMotion;
    const float* cameraViewToWorld;
    const float* cameraWorldToView;
    const float* cameraProj;
    const float* cameraProjInv;
    const NRD_CommonSettings* commonSettings;
    const NRD_ReblurSettings* reblurSettings;
    const NRD_SigmaSettings* sigmaSettings;
    const NRD_RelaxSettings* relaxSettings;
} NRD_DispatchDesc;

typedef void* NRD_Instance;

NRD_Result NRD_CreateInstance(NRD_Denoiser denoiser, const NRD_ResourceDesc* device, NRD_Instance* instance);
void NRD_DestroyInstance(NRD_Instance instance);
NRD_Result NRD_SetMethodSettings(NRD_Instance instance, NRD_Denoiser denoiser, const void* settings);
NRD_Result NRD_Dispatch(NRD_Instance instance, const NRD_DispatchDesc* dispatchDesc, void* commandList);
NRD_Result NRD_GetComputeResourceUsage(NRD_Instance instance, NRD_Denoiser denoiser, uint32_t width, uint32_t height, NRD_ResourceDesc* tempResources, uint32_t* tempResourceCount);
NRD_Result NRD_GetScratchResourceUsage(NRD_Instance instance, NRD_Denoiser denoiser, uint32_t width, uint32_t height, uint64_t* scratchSize);

#endif // NRD_H