#pragma once
#include "virtualgeometrygpu.h"
#include "rendering/gpuscene/gpuscenebuffer.h"
#include "rendering/rendergraph.h"
#include "virtualgeometrypagecache.h"

namespace VirtualGeometryResources
{
uint32_t Register(std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset> asset);
uint32_t RegisterManifest(std::shared_ptr<const VirtualGeometryPageCache::PageManifest> manifest);
void Prepare(RenderGraph& graph);
bool Ready(uint32_t asset);
uint32_t AssetCount();
uint64_t RegisteredClusterCount();
const std::vector<uint64_t>& SourceTriangleCounts();
std::shared_ptr<const VirtualGeometryPageCache::PageManifest> ManifestFor(const VirtualGeometryRuntime::RuntimeAsset* asset);
const std::vector<VirtualGeometryGPUAsset>& AssetTable();
const VirtualGeometryGPU::PackedAsset& Metadata();
VirtualGeometryCompression::Metrics CompressionMetrics();
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
