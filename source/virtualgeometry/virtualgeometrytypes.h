#pragma once
#include "virtualgeometryconfig.h"
#include <array>
#include <cstddef>
#include <type_traits>

// Portable CPU layout mirrors StaticModelVertex; no D3D resource ownership.
struct VirtualGeometryVertex
{
    float Position[3]{};
    float Normal[3]{};
    float TexCoord[2]{};
    float Diffuse[4]{};
};
static_assert(std::is_standard_layout_v<VirtualGeometryVertex>);
static_assert(sizeof(VirtualGeometryVertex) == 12 * sizeof(float));
static_assert(offsetof(VirtualGeometryVertex, Normal) == 3 * sizeof(float));

struct VirtualGeometrySphere
{
    std::array<float, 3> Center{};
    float Radius = 0;
};
struct VirtualGeometryAabb
{
    std::array<float, 3> Min{};
    std::array<float, 3> Max{};
};
struct VirtualGeometryNormalCone
{
    std::array<float, 3> Axis{};
    std::array<float, 3> Apex{};
    float Cutoff = 1;
};
struct VirtualGeometryCluster
{
    uint32_t VertexOffset = 0;
    uint32_t VertexCount = 0;
    uint32_t TriangleOffset = 0; // Byte offset: three uint8_t indices per triangle.
    uint32_t TriangleCount = 0;
    uint32_t Level = 0;
    uint32_t GroupIndex = VirtualGeometryConfig::InvalidIndex; // Consuming group; invalid for roots.
    uint32_t GeneratingGroupIndex = VirtualGeometryConfig::InvalidIndex;
    uint32_t MaterialId = 0; // Stable source section ID, distinct from shader MaterialPartId.
    VirtualGeometrySphere Bounds;
    VirtualGeometryAabb Aabb;
    VirtualGeometryNormalCone Cone;
    float LodError = 0;
};
struct VirtualGeometryClusterGroup
{
    uint32_t Level = 0; // Child level.
    uint32_t FirstChild = 0;
    uint32_t ChildCount = 0;
    uint32_t FirstParent = 0;
    uint32_t ParentCount = 0;
    VirtualGeometrySphere Bounds; // Covers descendant geometry, not just simplified parents.
    float MinError = 0;
    float MaxError = 0;
};
struct VirtualGeometryLevelStats
{
    uint32_t Clusters = 0;
    uint64_t Triangles = 0;
    uint64_t Vertices = 0;
};
struct VirtualGeometryBuildStats
{
    uint64_t SourceVertices = 0;
    uint64_t SourceTriangles = 0;
    uint64_t RemovedDegenerateTriangles = 0;
    uint64_t RemovedDuplicateTriangles = 0;
    uint64_t FinalLeafTriangles = 0;
    uint32_t LeafClusters = 0;
    uint32_t TotalClusters = 0;
    uint32_t TotalGroups = 0;
    uint32_t HierarchyLevels = 0;
    uint32_t MaximumTrianglesPerCluster = 0;
    uint32_t MaximumVerticesPerCluster = 0;
    double AverageTrianglesPerCluster = 0;
    double AverageVerticesPerCluster = 0;
    uint64_t SourceMemoryBytes = 0;
    uint64_t RawMemoryBytes = 0;
    double BuildMilliseconds = 0; // Telemetry; excluded from deterministic serialization/hash.
};
