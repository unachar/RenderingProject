#pragma once
#include <cstdint>
#include <filesystem>
struct ImDrawList;
struct ImVec2;
// CPU snapshot; GPU visibility fields are valid only when HasVisibilitySample is true.
struct VGStatistics
{
    uint64_t VisibilitySampleSerial = 0;
    uint64_t RegisteredAssets = 0, RegisteredClusters = 0;
    uint64_t ResidentPages = 0, RequestedPages = 0, Evictions = 0;
    uint64_t ResidentAllocationBytes = 0, BudgetBytes = 0, MetadataBytes = 0;
    uint64_t SubmittedBytes = 0, CompletedBytes = 0, IoErrors = 0, RequestOverflow = 0;
    uint64_t RawGeometryBytes = 0, EncodedGeometryBytes = 0;
    uint64_t SourceTriangles = 0, InstancedSourceTriangles = 0, VisibleTriangles = 0;
    uint64_t MaterialBinOverflow = 0, InvalidMaterialRecords = 0;
    uint32_t Instances = 0;
    uint32_t SceneInstances = 0;
    uint32_t VisibleInstances = 0;
    bool HasInstanceSample = false;
    uint64_t VisibleInstanceOverflow = 0, InvalidIndirectCommands = 0, BinningRejectedClusters = 0;
    uint32_t FirstVisibleClusters = 0, SecondVisibleClusters = 0, CapacityFallbacks = 0;
    bool HasVisibilitySample = false;
};
VGStatistics GetVGStatistics();
namespace VirtualGeometryDebug
{
void DrawControls();
void DrawAssetInspector(const std::filesystem::path& path);
void DrawViewport(ImDrawList *draw, const ImVec2 &origin, const ImVec2 &size, uint32_t selectedEntity);
} // namespace VirtualGeometryDebug
