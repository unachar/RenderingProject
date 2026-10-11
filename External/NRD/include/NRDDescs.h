// NRD Descs Stub - Replace with actual NRD from https://github.com/NVIDIA/Real-Time-Denoiser

#pragma once

#ifndef NRD_DESCS_H
#define NRD_DESCS_H

#include "NRD.h"

// Resource description helpers
inline NRD_ResourceDesc NRD_ResourceDesc_Texture2D(NRD_Format format, uint32_t width, uint32_t height, void* nativeResource = nullptr)
{
    NRD_ResourceDesc desc = {};
    desc.type = NRD_ResourceType_Texture2D;
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.depthOrArraySize = 1;
    desc.mipLevels = 1;
    desc.sampleCount = 1;
    desc.flags = 0;
    desc.nativeObject = nativeResource;
    return desc;
}

inline NRD_ResourceDesc NRD_ResourceDesc_Buffer(NRD_Format format, uint32_t width, void* nativeResource = nullptr)
{
    NRD_ResourceDesc desc = {};
    desc.type = NRD_ResourceType_Buffer;
    desc.format = format;
    desc.width = width;
    desc.height = 1;
    desc.depthOrArraySize = 1;
    desc.mipLevels = 1;
    desc.sampleCount = 1;
    desc.flags = 0;
    desc.nativeObject = nativeResource;
    return desc;
}

// Common settings defaults
inline NRD_CommonSettings NRD_CommonSettings_Default()
{
    NRD_CommonSettings settings = {};
    settings.enableReferenceAccumulation = true;
    settings.enableVarianceEstimation = true;
    settings.minDiffuseRadianceFiltering = 0.0f;
    settings.minSpecularRadianceFiltering = 0.0f;
    settings.minShadowTranslucencyFiltering = 0.0f;
    return settings;
}

inline NRD_ReblurSettings NRD_ReblurSettings_Default()
{
    NRD_ReblurSettings settings = {};
    settings.sigma = 16.0f;
    settings.minLuminanceWeight = 0.01f;
    settings.temporalStability = 0.75f;
    settings.depthThreshold = 0.05f;
    settings.normalThreshold = 0.99f;
    return settings;
}

inline NRD_SigmaSettings NRD_SigmaSettings_Default()
{
    NRD_SigmaSettings settings = {};
    settings.sigma = 16.0f;
    settings.minLuminanceWeight = 0.01f;
    settings.temporalStability = 0.75f;
    settings.depthThreshold = 0.05f;
    settings.normalThreshold = 0.99f;
    return settings;
}

inline NRD_RelaxSettings NRD_RelaxSettings_Default()
{
    NRD_RelaxSettings settings = {};
    settings.sigma = 16.0f;
    settings.minLuminanceWeight = 0.01f;
    settings.temporalStability = 0.75f;
    settings.depthThreshold = 0.05f;
    settings.normalThreshold = 0.99f;
    settings.roughnessThreshold = 0.1f;
    return settings;
}

// Dispatch desc builder helper
struct NRD_DispatchDescBuilder
{
    NRD_DispatchDesc desc = {};
    
    NRD_DispatchDescBuilder(uint32_t width, uint32_t height)
    {
        desc.width = width;
        desc.height = height;
        desc.frameTimeDelta = 1.0f / 60.0f;
    }
    
    NRD_DispatchDescBuilder& InColor(const NRD_ResourceDesc* v) { desc.inColor = v; return *this; }
    NRD_DispatchDescBuilder& InNormal(const NRD_ResourceDesc* v) { desc.inNormal = v; return *this; }
    NRD_DispatchDescBuilder& InDepth(const NRD_ResourceDesc* v) { desc.inDepth = v; return *this; }
    NRD_DispatchDescBuilder& InMotionVectors(const NRD_ResourceDesc* v) { desc.inMotionVectors = v; return *this; }
    NRD_DispatchDescBuilder& InRoughness(const NRD_ResourceDesc* v) { desc.inRoughness = v; return *this; }
    NRD_DispatchDescBuilder& InSpecularHitDistance(const NRD_ResourceDesc* v) { desc.inSpecularHitDistance = v; return *this; }
    NRD_DispatchDescBuilder& InDiffuseHitDistance(const NRD_ResourceDesc* v) { desc.inDiffuseHitDistance = v; return *this; }
    NRD_DispatchDescBuilder& InViewZ(const NRD_ResourceDesc* v) { desc.inViewZ = v; return *this; }
    NRD_DispatchDescBuilder& InShadowTranslucency(const NRD_ResourceDesc* v) { desc.inShadowTranslucency = v; return *this; }
    NRD_DispatchDescBuilder& InShadowData(const NRD_ResourceDesc* v) { desc.inShadowData = v; return *this; }
    NRD_DispatchDescBuilder& InShadowNormal(const NRD_ResourceDesc* v) { desc.inShadowNormal = v; return *this; }
    NRD_DispatchDescBuilder& InPrevDenoisedColor(const NRD_ResourceDesc* v) { desc.inPrevDenoisedColor = v; return *this; }
    NRD_DispatchDescBuilder& OutDenoisedColor(const NRD_ResourceDesc* v) { desc.outDenoisedColor = v; return *this; }
    NRD_DispatchDescBuilder& OutDiffuseRadiance(const NRD_ResourceDesc* v) { desc.outDiffuseRadiance = v; return *this; }
    NRD_DispatchDescBuilder& OutSpecularRadiance(const NRD_ResourceDesc* v) { desc.outSpecularRadiance = v; return *this; }
    NRD_DispatchDescBuilder& OutShadowTranslucency(const NRD_ResourceDesc* v) { desc.outShadowTranslucency = v; return *this; }
    NRD_DispatchDescBuilder& FrameTimeDelta(float v) { desc.frameTimeDelta = v; return *this; }
    NRD_DispatchDescBuilder& CameraJitter(const float* v) { desc.cameraJitter = v; return *this; }
    NRD_DispatchDescBuilder& CameraMotion(const float* v) { desc.cameraMotion = v; return *this; }
    NRD_DispatchDescBuilder& CameraViewToWorld(const float* v) { desc.cameraViewToWorld = v; return *this; }
    NRD_DispatchDescBuilder& CameraWorldToView(const float* v) { desc.cameraWorldToView = v; return *this; }
    NRD_DispatchDescBuilder& CameraProj(const float* v) { desc.cameraProj = v; return *this; }
    NRD_DispatchDescBuilder& CameraProjInv(const float* v) { desc.cameraProjInv = v; return *this; }
    NRD_DispatchDescBuilder& CommonSettings(const NRD_CommonSettings* v) { desc.commonSettings = v; return *this; }
    NRD_DispatchDescBuilder& ReblurSettings(const NRD_ReblurSettings* v) { desc.reblurSettings = v; return *this; }
    NRD_DispatchDescBuilder& SigmaSettings(const NRD_SigmaSettings* v) { desc.sigmaSettings = v; return *this; }
    NRD_DispatchDescBuilder& RelaxSettings(const NRD_RelaxSettings* v) { desc.relaxSettings = v; return *this; }
    
    NRD_DispatchDesc Build() { return desc; }
};

#endif // NRD_DESCS_H