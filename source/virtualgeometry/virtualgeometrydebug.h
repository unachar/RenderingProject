#pragma once
#include <cstdint>
#include <filesystem>
struct ImDrawList;
struct ImVec2;

// CPU のスナップショット。GPU 可視性フィールドは HasVisibilitySample が true の場合だけ有効。
struct VGStatistics
{
    uint64_t VisibilitySampleSerial = 0;
    uint64_t RegisteredAssets = 0;
    uint64_t RegisteredClusters = 0;
    uint64_t ResidentPages = 0;
    uint64_t RequestedPages = 0;
    uint64_t Evictions = 0;
    uint64_t ResidentAllocationBytes = 0;
    uint64_t BudgetBytes = 0;
    uint64_t MetadataBytes = 0;
    uint64_t SubmittedBytes = 0;
    uint64_t CompletedBytes = 0;
    uint64_t IoErrors = 0;
    uint64_t RequestOverflow = 0;
    uint64_t RawGeometryBytes = 0;
    uint64_t EncodedGeometryBytes = 0;
    uint64_t SourceTriangles = 0;
    uint64_t InstancedSourceTriangles = 0;
    uint64_t VisibleTriangles = 0;
    uint64_t MaterialBinOverflow = 0;
    uint64_t InvalidMaterialRecords = 0;
    uint32_t Instances = 0;
    uint32_t SceneInstances = 0;
    uint32_t VisibleInstances = 0;
    bool HasInstanceSample = false;
    uint64_t VisibleInstanceOverflow = 0;
    uint64_t InvalidIndirectCommands = 0;
    uint64_t BinningRejectedClusters = 0;
    uint32_t FirstVisibleClusters = 0;
    uint32_t SecondVisibleClusters = 0;
    uint32_t CapacityFallbacks = 0;
    bool HasVisibilitySample = false;
};

VGStatistics GetVGStatistics();

namespace VirtualGeometryDebug
{
    struct Preferences
    {
        bool HierarchyBounds = false, Wireframe = false, SelectedOnly = true;
        int HierarchyLevel = 0;
        int SourceLOD = 0;
        int Limit = 512;
    };

    Preferences& GetPreferences();
    void DrawControls();
    void DrawAssetInspector(const std::filesystem::path& path);
    void DrawViewport(ImDrawList* draw, const ImVec2& origin, const ImVec2& size, uint32_t selectedEntity);
} // 名前空間 VirtualGeometryDebug の終わり。
