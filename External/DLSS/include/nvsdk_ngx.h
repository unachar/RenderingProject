// DLSS SDK Stub Header - Replace with actual SDK from NVIDIA Developer Portal
// Download: https://developer.nvidia.com/nvidia-dlss-sdk
// RTX 3050 Laptop supports DLSS 2.x (Super Resolution) ONLY
// DLSS 3 Frame Generation requires RTX 40 series (Ada Lovelace) with Optical Flow Accelerator

#pragma once

#ifndef NVSDK_NGX_H
#define NVSDK_NGX_H

#include <stdint.h>
#include <stddef.h>

// Basic type definitions
typedef enum NVSDK_NGX_Result
{
    NVSDK_NGX_Result_Success = 0,
    NVSDK_NGX_Result_Fail = -1,
    NVSDK_NGX_Result_InvalidParameter = -2,
    NVSDK_NGX_Result_NotSupported = -3,
    NVSDK_NGX_Result_NotInitialized = -4,
} NVSDK_NGX_Result;

typedef enum NVSDK_NGX_Feature
{
    NVSDK_NGX_Feature_SuperResolution = 1,
    NVSDK_NGX_Feature_RayReconstruction = 2,
    NVSDK_NGX_Feature_DLAA = 3,
} NVSDK_NGX_Feature;

typedef enum NVSDK_NGX_PerfQuality_Value
{
    NVSDK_NGX_PerfQuality_Value_MaxPerf = 0,
    NVSDK_NGX_PerfQuality_Value_Balanced = 1,
    NVSDK_NGX_PerfQuality_Value_Quality = 2,
    NVSDK_NGX_PerfQuality_Value_MaxQuality = 3,
    NVSDK_NGX_PerfQuality_Value_DLAA = 4,
} NVSDK_NGX_PerfQuality_Value;

typedef struct NVSDK_NGX_Parameters* NVSDK_NGX_Parameters_Handle;

NVSDK_NGX_Result NVSDK_NGX_V1_Init(uint32_t sdkVersion, const wchar_t* logDirectory, void* optionalParameters);
NVSDK_NGX_Result NVSDK_NGX_V1_Shutdown();
NVSDK_NGX_Result NVSDK_NGX_V1_CreateFeature(NVSDK_NGX_Feature feature, NVSDK_NGX_Parameters_Handle params, void** featureHandle);
NVSDK_NGX_Result NVSDK_NGX_V1_ReleaseFeature(void* featureHandle);
NVSDK_NGX_Result NVSDK_NGX_V1_EvaluateFeature(void* featureHandle, NVSDK_NGX_Parameters_Handle params, void* commandList);
NVSDK_NGX_Result NVSDK_NGX_V1_GetOptimalSettings(void* featureHandle, NVSDK_NGX_Parameters_Handle params);

NVSDK_NGX_Parameters_Handle NVSDK_NGX_V1_CreateParameters();
void NVSDK_NGX_V1_DestroyParameters(NVSDK_NGX_Parameters_Handle params);

#define NVSDK_NGX_VERSION_MAJOR 2
#define NVSDK_NGX_VERSION_MINOR 0
#define NVSDK_NGX_VERSION_PATCH 0

#endif // NVSDK_NGX_H