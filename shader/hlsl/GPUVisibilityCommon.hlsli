#ifndef GPU_VISIBILITY_INCLUDED
#define GPU_VISIBILITY_INCLUDED
#include "GpuSceneCommon.hlsli"
struct GPULODGroup { uint Offset; uint Count; uint2 Padding; };
struct GPULODLevel { uint MeshIndex; float Threshold; float Hysteresis; uint Padding; };
StructuredBuffer<GPUInstanceData> gInstances : register(t0);
StructuredBuffer<uint> gFrustum : register(t1);
StructuredBuffer<uint4> gGeometry : register(t2);
StructuredBuffer<GPULODGroup> gGroups : register(t3);
StructuredBuffer<GPULODLevel> gLevels : register(t4);
Texture2D<float> gHiZ : register(t5);
RWStructuredBuffer<uint4> gResult : register(u0);
RWStructuredBuffer<uint4> gHistory : register(u1);
RWByteAddressBuffer gStatistics : register(u2);
cbuffer VisibilityConstants : register(b0)
{
    row_major float4x4 View;
    row_major float4x4 Projection;
    row_major float4x4 PreviousViewProjection;
    uint Slots; uint MeshCount; uint GroupCount; uint LevelCount;
    uint EnableOcclusion; uint EnableLOD; uint ResetHistory; uint ForceLOD;
    float DepthBias; float LODBias; float2 HiZSize;
    uint HiZMips; uint DebugMode; uint NativeHierarchy; uint Padding;
};
#endif
