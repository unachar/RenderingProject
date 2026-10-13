#ifndef MESHLET_COMMON
#define MESHLET_COMMON
#include "GpuSceneCommon.hlsli"
struct MeshletRecord { uint VertexOffset, VertexCount, PrimitiveOffset, PrimitiveCount; };
struct MeshletBounds { float3 Center; float Radius; float3 ConeAxis; float ConeCutoff; };
struct ClusterRecord { uint Offset, Count; uint2 Padding; MeshletBounds Bounds; };
#endif
