// DLSS SDK Params Stub - Replace with actual SDK

#pragma once

#include "nvsdk_ngx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parameter keys for DLSS Super Resolution
#define NVSDK_NGX_PARAMETER_SUPER_RESOLUTION_MODE "SuperResolution.Mode"
#define NVSDK_NGX_PARAMETER_SUPER_RESOLUTION_SHARPNESS "SuperResolution.Sharpness"
#define NVSDK_NGX_PARAMETER_SUPER_RESOLUTION_MV_SCALE_X "SuperResolution.MVScaleX"
#define NVSDK_NGX_PARAMETER_SUPER_RESOLUTION_MV_SCALE_Y "SuperResolution.MVScaleY"
#define NVSDK_NGX_PARAMETER_SUPER_RESOLUTION_JITTER_OFFSET_X "SuperResolution.JitterOffsetX"
#define NVSDK_NGX_PARAMETER_SUPER_RESOLUTION_JITTER_OFFSET_Y "SuperResolution.JitterOffsetY"
#define NVSDK_NGX_PARAMETER_SUPER_RESOLUTION_PREV_FRAME_RESET "SuperResolution.PrevFrameReset"
#define NVSDK_NGX_PARAMETER_SUPER_RESOLUTION_AUTO_EXPOSURE "SuperResolution.AutoExposure"

// Feature creation flags
#define NVSDK_NGX_FEATURE_FLAG_NONE 0x0
#define NVSDK_NGX_FEATURE_FLAG_D3D12 0x1
#define NVSDK_NGX_FEATURE_FLAG_VULKAN 0x2

#ifdef __cplusplus
}
#endif