#pragma once
#include "meshletbuilder.h"
#include "gpuscene/gpuscenebuffer.h"
#include "rendergraph.h"
struct StaticMeshData;

namespace MeshletManager
{
void BuildImportedMesh(StaticMeshData& mesh);
bool PollImportedMesh(StaticMeshData& mesh);
void Register(uint32_t meshID, const Meshlets::Asset& asset, uint32_t baseVertex);
void Replace(uint32_t meshID, const Meshlets::Asset& asset, uint32_t baseVertex);
void Invalidate(uint32_t meshID);
void RestoreForValidation(uint32_t meshID);
void Prepare(RenderGraph& graph, uint32_t meshes);
const std::vector<Meshlets::Range>& Ranges();
const Meshlets::Asset& AssetData();
GpuSceneBuffer& MeshletBuffer();
GpuSceneBuffer& VertexBuffer();
GpuSceneBuffer& PrimitiveBuffer();
GpuSceneBuffer& BoundsBuffer();
GpuSceneBuffer& ClusterBuffer();
GpuSceneBuffer& RangeBuffer();
uint64_t MemoryBytes();
void Shutdown();
} // namespace MeshletManager
