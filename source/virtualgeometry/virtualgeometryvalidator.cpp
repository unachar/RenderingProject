#include "virtualgeometryvalidator.h"
#include "virtualgeometryinternal.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>

namespace
{
using namespace VirtualGeometryInternal;

bool Finite(const std::array<float, 3>& values)
{
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value);
    });
}

bool ValidSphere(const VirtualGeometrySphere& sphere)
{
    return Finite(sphere.Center) && std::isfinite(sphere.Radius) && sphere.Radius >= 0;
}

bool Contains(const VirtualGeometrySphere& outer, const VirtualGeometrySphere& inner, float epsilon)
{
    double distanceSquared = 0;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        double difference = double(outer.Center[axis]) - inner.Center[axis];
        distanceSquared += difference * difference;
    }
    double tolerance = std::max(double(epsilon), double(outer.Radius) * 1e-5);
    return std::sqrt(distanceSquared) + inner.Radius <= outer.Radius + tolerance;
}

struct PositionKey
{
    std::array<uint32_t, 3> Bits{};

    explicit PositionKey(const VirtualGeometryVertex& vertex)
    {
        for (size_t axis = 0; axis < 3; ++axis)
        {
            Bits[axis] = vertex.Position[axis] == 0 ? 0 : std::bit_cast<uint32_t>(vertex.Position[axis]);
        }
    }

    bool operator==(const PositionKey&) const = default;
};

struct PositionHash
{
    size_t operator()(const PositionKey& position) const
    {
        size_t hash = 0;
        for (uint32_t bits : position.Bits)
        {
            hash = hash * 16777619u ^ bits;
        }
        return hash;
    }
};

using EdgeCounts = std::unordered_map<uint64_t, uint32_t>;

void AddEdges(const VirtualGeometryAsset& asset, uint32_t clusterIndex, const std::vector<uint32_t>& positionIds,
              EdgeCounts& counts)
{
    const auto& cluster = asset.Clusters[clusterIndex];
    for (uint32_t triangle = 0; triangle < cluster.TriangleCount; ++triangle)
    {
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            uint32_t first = positionIds[cluster.VertexOffset +
                                         asset.TriangleIndices[cluster.TriangleOffset + triangle * 3 + corner]];
            uint32_t second =
                positionIds[cluster.VertexOffset +
                            asset.TriangleIndices[cluster.TriangleOffset + triangle * 3 + (corner + 1) % 3]];
            ++counts[uint64_t(std::min(first, second)) << 32 | std::max(first, second)];
        }
    }
}

bool ValidateGroupBoundaries(const VirtualGeometryAsset& asset, std::string& error)
{
    std::unordered_map<PositionKey, uint32_t, PositionHash> positions;
    std::vector<uint32_t> positionIds;
    positionIds.reserve(asset.Vertices.size());
    for (const auto& vertex : asset.Vertices)
    {
        auto [entry, inserted] = positions.emplace(PositionKey(vertex), CheckedIndex(positions.size()));
        positionIds.push_back(entry->second);
    }

    // Build positional edge incidence once per level. No pairwise cluster comparisons.
    struct Incidence
    {
        uint32_t Count = 0;
        uint32_t Group = VirtualGeometryConfig::InvalidIndex;
        bool Shared = false;
    };

    std::vector<std::unordered_map<uint64_t, Incidence>> levelEdges(asset.Levels.size());
    for (uint32_t index = 0; index < asset.Clusters.size(); ++index)
    {
        const auto& cluster = asset.Clusters[index];
        EdgeCounts edges;
        AddEdges(asset, index, positionIds, edges);
        for (const auto& [key, count] : edges)
        {
            auto& incidence = levelEdges[cluster.Level][key];
            if (incidence.Count == 0)
            {
                incidence.Group = cluster.GroupIndex;
            }
            incidence.Shared |= incidence.Group != cluster.GroupIndex;
            incidence.Count += count;
        }
    }
    for (const auto& group : asset.Groups)
    {
        EdgeCounts before;
        EdgeCounts after;
        for (uint32_t child = 0; child < group.ChildCount; ++child)
        {
            AddEdges(asset, asset.HierarchyIndices[group.FirstChild + child], positionIds, before);
        }
        for (uint32_t parent = 0; parent < group.ParentCount; ++parent)
        {
            AddEdges(asset, asset.HierarchyIndices[group.FirstParent + parent], positionIds, after);
        }
        for (const auto& [key, count] : before)
        {
            const auto& incidence = levelEdges[group.Level].at(key);
            bool protectedEdge = count != 2 || incidence.Shared || incidence.Count != 2;
            if (protectedEdge && (!after.contains(key) || after.at(key) != count))
            {
                error = "Group boundary geometry changed across LOD";
                return false;
            }
        }
        for (const auto& [key, count] : after)
        {
            if (count != 2 && (!before.contains(key) || before.at(key) != count))
            {
                error = "Parent geometry introduces a boundary/non-manifold edge";
                return false;
            }
        }
    }
    return true;
}

bool ValidateData(const VirtualGeometryAsset& asset, std::string& error)
{
    auto fail = [&error](const char* message) {
        error = message;
        return false;
    };
    if (asset.Version != VirtualGeometryConfig::AssetVersion || !ValidateVirtualGeometryConfig(asset.Config, error))
    {
        return fail("Unsupported asset version or invalid build config");
    }
    if (asset.Clusters.empty() || asset.RootClusters.empty() || asset.Levels.empty() ||
        asset.Levels.size() > asset.Config.MaxHierarchyLevels || !std::isfinite(asset.SourceMaterialPartId))
    {
        return fail("Empty or invalid virtual geometry asset");
    }
    if (asset.Vertices.size() >= VirtualGeometryConfig::InvalidIndex ||
        asset.TriangleIndices.size() >= VirtualGeometryConfig::InvalidIndex ||
        asset.Clusters.size() >= VirtualGeometryConfig::InvalidIndex ||
        asset.Groups.size() >= VirtualGeometryConfig::InvalidIndex ||
        asset.HierarchyIndices.size() >= VirtualGeometryConfig::InvalidIndex)
    {
        return fail("Asset exceeds 32-bit addressing");
    }
    for (const auto& vertex : asset.Vertices)
    {
        if (!IsFiniteVertex(vertex))
        {
            return fail("Non-finite asset vertex");
        }
    }
    std::vector<VirtualGeometryLevelStats> measured(asset.Levels.size());
    uint64_t vertexEnd = 0;
    uint64_t triangleEnd = 0;
    uint32_t maximumTriangles = 0;
    uint32_t maximumVertices = 0;
    for (const auto& cluster : asset.Clusters)
    {
        if (cluster.VertexCount == 0 || cluster.VertexCount > asset.Config.MaxClusterVertices ||
            cluster.TriangleCount == 0 || cluster.TriangleCount > asset.Config.MaxClusterTriangles ||
            cluster.Level >= asset.Levels.size() || cluster.MaterialId != asset.SourceMaterialId ||
            !RangeFits(cluster.VertexOffset, cluster.VertexCount, asset.Vertices.size()) ||
            !RangeFits(cluster.TriangleOffset, uint64_t(cluster.TriangleCount) * 3, asset.TriangleIndices.size()) ||
            cluster.VertexOffset != vertexEnd || cluster.TriangleOffset != triangleEnd)
        {
            return fail("Invalid cluster ranges, limits, level, or material");
        }
        vertexEnd += cluster.VertexCount;
        triangleEnd += uint64_t(cluster.TriangleCount) * 3;
        if (!ValidSphere(cluster.Bounds) || !Finite(cluster.Aabb.Min) || !Finite(cluster.Aabb.Max) ||
            !Finite(cluster.Cone.Axis) || !Finite(cluster.Cone.Apex) || !std::isfinite(cluster.Cone.Cutoff) ||
            cluster.Cone.Cutoff < 0 || cluster.Cone.Cutoff > 1 || !std::isfinite(cluster.LodError) ||
            cluster.LodError < 0 || (cluster.Level == 0 && cluster.LodError != 0))
        {
            return fail("Invalid cluster bounds/cone/error");
        }
        for (size_t axis = 0; axis < 3; ++axis)
        {
            if (cluster.Aabb.Min[axis] > cluster.Aabb.Max[axis])
            {
                return fail("Inverted AABB");
            }
        }
        for (uint32_t vertexIndex = 0; vertexIndex < cluster.VertexCount; ++vertexIndex)
        {
            const auto& vertex = asset.Vertices[cluster.VertexOffset + vertexIndex];
            VirtualGeometrySphere point;
            std::copy_n(vertex.Position, 3, point.Center.begin());
            if (!Contains(cluster.Bounds, point, asset.Config.BoundaryEpsilon))
            {
                return fail("Cluster sphere does not cover geometry");
            }
            for (size_t axis = 0; axis < 3; ++axis)
            {
                if (vertex.Position[axis] < cluster.Aabb.Min[axis] || vertex.Position[axis] > cluster.Aabb.Max[axis])
                {
                    return fail("Cluster AABB does not cover geometry");
                }
            }
        }
        for (uint32_t index = 0; index < cluster.TriangleCount * 3; ++index)
        {
            if (asset.TriangleIndices[cluster.TriangleOffset + index] >= cluster.VertexCount)
            {
                return fail("Cluster-local triangle index out of bounds");
            }
        }
        auto& level = measured[cluster.Level];
        ++level.Clusters;
        level.Triangles += cluster.TriangleCount;
        level.Vertices += cluster.VertexCount;
        maximumTriangles = std::max(maximumTriangles, cluster.TriangleCount);
        maximumVertices = std::max(maximumVertices, cluster.VertexCount);
    }
    if (vertexEnd != asset.Vertices.size() || triangleEnd != asset.TriangleIndices.size())
    {
        return fail("Unreferenced geometry payload");
    }
    std::vector<uint32_t> consumers(asset.Clusters.size(), VirtualGeometryConfig::InvalidIndex);
    std::vector<uint32_t> producers(asset.Clusters.size(), VirtualGeometryConfig::InvalidIndex);
    uint64_t hierarchyEnd = 0;
    for (uint32_t index = 0; index < asset.Groups.size(); ++index)
    {
        const auto& group = asset.Groups[index];
        if (group.ChildCount == 0 || group.ParentCount == 0 || group.ParentCount >= group.ChildCount ||
            !RangeFits(group.FirstChild, group.ChildCount, asset.HierarchyIndices.size()) ||
            !RangeFits(group.FirstParent, group.ParentCount, asset.HierarchyIndices.size()) ||
            group.FirstChild != hierarchyEnd || uint64_t(group.FirstParent) != hierarchyEnd + group.ChildCount ||
            !ValidSphere(group.Bounds) || !std::isfinite(group.MinError) || !std::isfinite(group.MaxError) ||
            group.MinError < 0 || group.MaxError < group.MinError)
        {
            return fail("Invalid cluster group ranges/bounds/error");
        }
        hierarchyEnd += uint64_t(group.ChildCount) + group.ParentCount;
        float maximumChildError = 0;
        uint64_t childTriangles = 0;
        uint64_t parentTriangles = 0;
        for (uint32_t child = 0; child < group.ChildCount; ++child)
        {
            uint32_t childIndex = asset.HierarchyIndices[group.FirstChild + child];
            if (childIndex >= asset.Clusters.size() || consumers[childIndex] != VirtualGeometryConfig::InvalidIndex)
            {
                return fail("Invalid or duplicate group child");
            }
            consumers[childIndex] = index;
            const auto& cluster = asset.Clusters[childIndex];
            if (cluster.Level != group.Level || cluster.GroupIndex != index)
            {
                return fail("Child level/group mismatch");
            }
            const auto& descendantBounds = cluster.GeneratingGroupIndex == VirtualGeometryConfig::InvalidIndex
                                               ? cluster.Bounds
                                               : asset.Groups.at(cluster.GeneratingGroupIndex).Bounds;
            if (!Contains(group.Bounds, descendantBounds, asset.Config.BoundaryEpsilon))
            {
                return fail("Group sphere does not cover descendants");
            }
            maximumChildError = std::max(maximumChildError, cluster.LodError);
            childTriangles += cluster.TriangleCount;
        }
        if (group.MinError != maximumChildError)
        {
            return fail("Group minimum error does not match its children");
        }
        for (uint32_t parent = 0; parent < group.ParentCount; ++parent)
        {
            uint32_t parentIndex = asset.HierarchyIndices[group.FirstParent + parent];
            if (parentIndex >= asset.Clusters.size() || producers[parentIndex] != VirtualGeometryConfig::InvalidIndex)
            {
                return fail("Invalid or duplicate group parent");
            }
            producers[parentIndex] = index;
            const auto& cluster = asset.Clusters[parentIndex];
            // Strict level increase proves acyclicity without recursive stack traversal.
            if (uint64_t(cluster.Level) != uint64_t(group.Level) + 1 || cluster.GeneratingGroupIndex != index ||
                cluster.LodError < maximumChildError || cluster.LodError != group.MaxError)
            {
                return fail("Hierarchy cycle/level/parent error mismatch");
            }
            parentTriangles += cluster.TriangleCount;
        }
        if (parentTriangles >= childTriangles)
        {
            return fail("Non-reducing hierarchy group");
        }
    }
    if (hierarchyEnd != asset.HierarchyIndices.size())
    {
        return fail("Unreferenced hierarchy payload");
    }
    std::vector<bool> roots(asset.Clusters.size());
    for (uint32_t root : asset.RootClusters)
    {
        if (root >= asset.Clusters.size() || roots[root] || consumers[root] != VirtualGeometryConfig::InvalidIndex)
        {
            return fail("Invalid, duplicate, or consumed root");
        }
        roots[root] = true;
    }
    for (size_t index = 0; index < asset.Clusters.size(); ++index)
    {
        const auto& cluster = asset.Clusters[index];
        bool isRoot = consumers[index] == VirtualGeometryConfig::InvalidIndex;
        bool isLeaf = cluster.Level == 0;
        if (roots[index] != isRoot || cluster.GroupIndex != consumers[index] ||
            cluster.GeneratingGroupIndex != producers[index] ||
            isLeaf != (producers[index] == VirtualGeometryConfig::InvalidIndex))
        {
            return fail("Orphan cluster or inconsistent producer/consumer/root metadata");
        }
    }
    const auto& stats = asset.Stats;
    if (stats.SourceVertices == 0 || stats.SourceVertices >= VirtualGeometryConfig::InvalidIndex ||
        stats.SourceTriangles == 0 || stats.SourceTriangles >= VirtualGeometryConfig::InvalidIndex / 3 ||
        stats.RemovedDegenerateTriangles > stats.SourceTriangles ||
        stats.RemovedDuplicateTriangles > stats.SourceTriangles - stats.RemovedDegenerateTriangles ||
        stats.FinalLeafTriangles !=
            stats.SourceTriangles - stats.RemovedDegenerateTriangles - stats.RemovedDuplicateTriangles ||
        stats.FinalLeafTriangles != measured[0].Triangles || stats.LeafClusters != measured[0].Clusters ||
        stats.TotalClusters != asset.Clusters.size() || stats.TotalGroups != asset.Groups.size() ||
        stats.HierarchyLevels != asset.Levels.size() || stats.MaximumTrianglesPerCluster != maximumTriangles ||
        stats.MaximumVerticesPerCluster != maximumVertices || !std::isfinite(stats.BuildMilliseconds) ||
        stats.BuildMilliseconds < 0)
    {
        return fail("Invalid source/leaf/asset statistics");
    }
    uint64_t allTriangles = 0;
    uint64_t allVertices = 0;
    for (size_t level = 0; level < measured.size(); ++level)
    {
        const auto& expected = asset.Levels[level];
        const auto& actual = measured[level];
        if (actual.Clusters == 0 || expected.Clusters != actual.Clusters || expected.Triangles != actual.Triangles ||
            expected.Vertices != actual.Vertices)
        {
            return fail("Invalid per-level statistics");
        }
        allTriangles += actual.Triangles;
        allVertices += actual.Vertices;
    }
    uint64_t rawBytes = uint64_t(asset.Vertices.size()) * sizeof(VirtualGeometryVertex) + asset.TriangleIndices.size() +
                        uint64_t(asset.Clusters.size()) * sizeof(VirtualGeometryCluster) +
                        uint64_t(asset.Groups.size()) * sizeof(VirtualGeometryClusterGroup) +
                        uint64_t(asset.HierarchyIndices.size() + asset.RootClusters.size()) * sizeof(uint32_t) +
                        uint64_t(asset.Levels.size()) * sizeof(VirtualGeometryLevelStats);
    if (stats.AverageTrianglesPerCluster != double(allTriangles) / asset.Clusters.size() ||
        stats.AverageVerticesPerCluster != double(allVertices) / asset.Clusters.size() ||
        stats.RawMemoryBytes != rawBytes ||
        stats.SourceMemoryBytes !=
            stats.SourceVertices * sizeof(VirtualGeometryVertex) + stats.SourceTriangles * 3 * sizeof(uint32_t))
    {
        return fail("Invalid memory/average statistics");
    }
    return ValidateGroupBoundaries(asset, error);
}
} // namespace

bool VirtualGeometryValidator::Validate(const VirtualGeometryAsset& asset, std::string& error)
{
    error.clear();
    try
    {
        return ValidateData(asset, error);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
