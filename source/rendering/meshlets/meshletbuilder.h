#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Meshlets
{
struct Position
{
    float X = 0, Y = 0, Z = 0;
};
struct BuildConfig
{
    uint32_t MaxVertices = 64, MaxPrimitives = 126, MeshletsPerCluster = 32;
};
struct Meshlet
{
    uint32_t VertexOffset = 0, VertexCount = 0, PrimitiveOffset = 0, PrimitiveCount = 0;
};
struct Bounds
{
    Position Center;
    float Radius = 0;
    Position ConeAxis;
    float ConeCutoff = 1; // 1 disables cone. Otherwise sin(max normal angle).
};
struct Cluster
{
    uint32_t MeshletOffset = 0, MeshletCount = 0, Padding[2]{};
    Bounds Bounds;
};
struct Range
{
    uint32_t MeshletOffset = 0, MeshletCount = 0, ClusterOffset = 0, ClusterCount = 0;
};
struct Asset
{
    BuildConfig Config;
    std::vector<Meshlet> Meshlets;
    std::vector<uint32_t> Vertices, Primitives;
    std::vector<Bounds> Bounds;
    std::vector<Cluster> Clusters;
};
static_assert(sizeof(Meshlet) == 16 && sizeof(Bounds) == 32 && sizeof(Cluster) == 48 && sizeof(Range) == 16);
Asset Build(std::span<const Position> positions, std::span<const uint32_t> indices, BuildConfig config = {});
void RefreshMeshletBounds(Asset &asset, std::span<const Position> positions);
bool Validate(const Asset &asset, std::span<const Position> positions, std::string &error);
uint64_t GeometryKey(std::span<const Position> positions, std::span<const uint32_t> indices,
                     BuildConfig config);
bool Save(const std::filesystem::path &path, const Asset &asset, uint64_t key);
bool Load(const std::filesystem::path &path, Asset &asset, uint64_t key, std::span<const Position> positions);
std::array<uint32_t, 3> DispatchDimensions(uint32_t count);
} // namespace Meshlets

