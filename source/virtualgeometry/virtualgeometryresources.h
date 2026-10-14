#pragma once
#include "virtualgeometrygpu.h"
#include "rendering/gpuscene/gpuscenebuffer.h"
#include "rendering/rendergraph.h"

namespace VirtualGeometryResources
{
uint32_t Register(std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset> asset);
void Prepare(RenderGraph& graph);
bool Ready(uint32_t asset);
uint32_t AssetCount();
const std::vector<VirtualGeometryGPUAsset>& AssetTable();
GpuSceneBuffer& ClusterBuffer();
GpuSceneBuffer& GroupBuffer();
GpuSceneBuffer& HierarchyIndexBuffer();
GpuSceneBuffer& RootBuffer();
GpuSceneBuffer& VertexBuffer();
GpuSceneBuffer& TriangleBuffer();
GpuSceneBuffer& AssetBuffer();
uint64_t MemoryBytes();
void Shutdown();
}
