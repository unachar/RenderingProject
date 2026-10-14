#include "virtualgeometryinternal.h"
#include "../../External/meshoptimizer/src/meshoptimizer.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace VirtualGeometryInternal
{
    namespace
    {
        uint64_t EdgeKey(uint32_t first, uint32_t second)
        {
            return uint64_t(std::min(first, second)) << 32 | std::max(first, second);
        }

        struct EdgeUse
        {
            uint32_t Count = 0;
            uint32_t Partition = 0;
            bool Shared = false;
        };
        using EdgeTable = std::unordered_map<uint64_t, EdgeUse>;
        using EdgeCounts = std::unordered_map<uint64_t, uint32_t>;

        struct VertexKey
        {
            std::array<uint32_t, 12> Components{};
            explicit VertexKey(const VirtualGeometryVertex& vertex)
            {
                auto values = VertexComponents(vertex);
                for (size_t component = 0; component < values.size(); ++component)
                {
                    Components[component] = values[component] == 0 ? 0 : std::bit_cast<uint32_t>(values[component]);
                }
            }
            bool operator==(const VertexKey&) const = default;
        };
        struct VertexHash
        {
            size_t operator()(const VertexKey& vertex) const
            {
                size_t hash = 0;
                for (uint32_t component : vertex.Components)
                {
                    hash = hash * 16777619u ^ component;
                }
                return hash;
            }
        };

        struct LevelGeometry
        {
            std::vector<std::array<float, 3>> Positions;
            std::vector<uint32_t> PositionRemap;
            std::vector<uint32_t> VertexOffsets;
            std::vector<std::vector<uint32_t>> Partitions; // Indices into current clusters.
            EdgeTable Edges;
        };

        LevelGeometry PartitionLevel(const VirtualGeometryAsset& asset, const std::vector<uint32_t>& current)
        {
            LevelGeometry geometry;
            std::vector<uint32_t> counts;
            for (uint32_t clusterIndex : current)
            {
                const auto& cluster = asset.Clusters[clusterIndex];
                geometry.VertexOffsets.push_back(CheckedIndex(geometry.Positions.size()));
                counts.push_back(cluster.VertexCount);
                for (uint32_t vertex = 0; vertex < cluster.VertexCount; ++vertex)
                {
                    const auto& position = asset.Vertices[cluster.VertexOffset + vertex].Position;
                    geometry.Positions.push_back({position[0] == 0 ? 0 : position[0],
                        position[1] == 0 ? 0 : position[1], position[2] == 0 ? 0 : position[2]});
                }
            }
            CheckedIndex(geometry.Positions.size());
            geometry.PositionRemap.resize(geometry.Positions.size());
            meshopt_generatePositionRemap(geometry.PositionRemap.data(), geometry.Positions[0].data(),
                geometry.Positions.size(), sizeof(geometry.Positions[0]));
            std::vector<uint32_t> partitionIds(current.size());
            size_t count = meshopt_partitionClusters(partitionIds.data(), geometry.PositionRemap.data(),
                geometry.PositionRemap.size(), counts.data(), current.size(), geometry.Positions[0].data(),
                geometry.Positions.size(), sizeof(geometry.Positions[0]), asset.Config.TargetClustersPerGroup);
            geometry.Partitions.resize(count);
            for (uint32_t currentIndex = 0; currentIndex < current.size(); ++currentIndex)
            {
                uint32_t partition = partitionIds[currentIndex];
                geometry.Partitions[partition].push_back(currentIndex);
                const auto& cluster = asset.Clusters[current[currentIndex]];
                uint32_t offset = geometry.VertexOffsets[currentIndex];
                for (uint32_t triangle = 0; triangle < cluster.TriangleCount; ++triangle)
                {
                    for (uint32_t corner = 0; corner < 3; ++corner)
                    {
                        uint32_t first = asset.TriangleIndices[cluster.TriangleOffset + triangle * 3 + corner];
                        uint32_t second = asset.TriangleIndices[cluster.TriangleOffset + triangle * 3 + (corner + 1) % 3];
                        auto& edge = geometry.Edges[EdgeKey(geometry.PositionRemap[offset + first],
                            geometry.PositionRemap[offset + second])];
                        if (edge.Count == 0)
                        {
                            edge.Partition = partition;
                        }
                        edge.Shared |= edge.Partition != partition;
                        ++edge.Count;
                    }
                }
            }
            return geometry;
        }

        struct GroupGeometry
        {
            std::vector<VirtualGeometryVertex> Vertices;
            std::vector<uint32_t> Indices;
            std::vector<uint32_t> PositionIds;
        };

        GroupGeometry MergeGroup(const VirtualGeometryAsset& asset, const std::vector<uint32_t>& current,
            const LevelGeometry& level, const std::vector<uint32_t>& partition)
        {
            GroupGeometry mesh;
            std::unordered_map<VertexKey, uint32_t, VertexHash> vertices;
            for (uint32_t currentIndex : partition)
            {
                const auto& cluster = asset.Clusters[current[currentIndex]];
                std::vector<uint32_t> localRemap(cluster.VertexCount);
                for (uint32_t vertexIndex = 0; vertexIndex < cluster.VertexCount; ++vertexIndex)
                {
                    const auto& vertex = asset.Vertices[cluster.VertexOffset + vertexIndex];
                    uint32_t destination = CheckedIndex(mesh.Vertices.size());
                    auto [entry, inserted] = vertices.emplace(VertexKey(vertex), destination);
                    if (inserted)
                    {
                        mesh.Vertices.push_back(vertex);
                        mesh.PositionIds.push_back(level.PositionRemap[level.VertexOffsets[currentIndex] + vertexIndex]);
                    }
                    localRemap[vertexIndex] = entry->second;
                }
                for (uint32_t index = 0; index < cluster.TriangleCount * 3; ++index)
                {
                    mesh.Indices.push_back(localRemap[asset.TriangleIndices[cluster.TriangleOffset + index]]);
                }
            }
            return mesh;
        }

        EdgeCounts CountEdges(const GroupGeometry& mesh, std::span<const uint32_t> indices)
        {
            EdgeCounts edges;
            edges.reserve(indices.size());
            for (size_t triangle = 0; triangle < indices.size(); triangle += 3)
            {
                for (size_t corner = 0; corner < 3; ++corner)
                {
                    ++edges[EdgeKey(mesh.PositionIds[indices[triangle + corner]],
                        mesh.PositionIds[indices[triangle + (corner + 1) % 3]])];
                }
            }
            return edges;
        }

        std::vector<unsigned char> LockBoundary(const GroupGeometry& mesh,
            const LevelGeometry& level, EdgeCounts& protectedEdges)
        {
            auto counts = CountEdges(mesh, mesh.Indices);
            std::unordered_set<uint32_t> lockedPositions;
            for (const auto& [key, count] : counts)
            {
                const auto& global = level.Edges.at(key);
                if (count != 2 || global.Shared || global.Count != 2)
                {
                    protectedEdges.emplace(key, count);
                    lockedPositions.insert(static_cast<uint32_t>(key >> 32));
                    lockedPositions.insert(static_cast<uint32_t>(key));
                }
            }
            std::vector<unsigned char> locks(mesh.Vertices.size());
            for (size_t vertex = 0; vertex < mesh.Vertices.size(); ++vertex)
            {
                if (lockedPositions.contains(mesh.PositionIds[vertex]))
                {
                    // All seam copies sharing this position are locked.
                    locks[vertex] = meshopt_SimplifyVertex_Lock;
                }
            }
            return locks;
        }

        void ValidateBoundary(const GroupGeometry& mesh, std::span<const uint32_t> simplified,
            const EdgeCounts& protectedEdges)
        {
            auto after = CountEdges(mesh, simplified);
            for (const auto& [key, count] : protectedEdges)
            {
                auto entry = after.find(key);
                if (entry == after.end() || entry->second != count)
                {
                    throw std::runtime_error("Simplification changed a protected boundary edge");
                }
            }
            for (const auto& [key, count] : after)
            {
                if (count != 2 && !protectedEdges.contains(key))
                {
                    throw std::runtime_error("Simplification introduced a new geometric boundary");
                }
            }
            // simplifyWithAttributes returns original vertex indices and cannot move positions.
            // Exact positional edge comparison above is stronger than an epsilon position check.
        }

        VirtualGeometrySphere GroupBounds(const VirtualGeometryAsset& asset,
            const std::vector<uint32_t>& children)
        {
            std::vector<VirtualGeometrySphere> spheres;
            spheres.reserve(children.size());
            for (uint32_t index : children)
            {
                const auto& child = asset.Clusters[index];
                const auto& sphere = child.GeneratingGroupIndex == VirtualGeometryConfig::InvalidIndex
                    ? child.Bounds : asset.Groups[child.GeneratingGroupIndex].Bounds;
                spheres.push_back(sphere);
            }
            auto bounds = meshopt_computeSphereBounds(spheres[0].Center.data(), spheres.size(),
                sizeof(VirtualGeometrySphere), &spheres[0].Radius, sizeof(VirtualGeometrySphere));
            VirtualGeometrySphere result;
            std::copy_n(bounds.center, 3, result.Center.begin());
            double radius = 0;
            for (const auto& sphere : spheres)
            {
                double distanceSquared = 0;
                for (size_t axis = 0; axis < 3; ++axis)
                {
                    double difference = double(result.Center[axis]) - sphere.Center[axis];
                    distanceSquared += difference * difference;
                }
                radius = std::max(radius, std::sqrt(distanceSquared) + sphere.Radius);
            }
            result.Radius = std::nextafter(static_cast<float>(radius), std::numeric_limits<float>::infinity());
            return result;
        }

        std::vector<uint32_t> SimplifyGroup(VirtualGeometryAsset& asset, const std::vector<uint32_t>& current,
            const LevelGeometry& level, const std::vector<uint32_t>& partition)
        {
            if (partition.size() < 2)
            {
                return {};
            }
            GroupGeometry mesh = MergeGroup(asset, current, level, partition);
            EdgeCounts protectedEdges;
            auto locks = LockBoundary(mesh, level, protectedEdges);
            std::vector<std::array<float, 9>> attributes;
            attributes.reserve(mesh.Vertices.size());
            for (const auto& vertex : mesh.Vertices)
            {
                attributes.push_back({vertex.Normal[0], vertex.Normal[1], vertex.Normal[2],
                    vertex.TexCoord[0], vertex.TexCoord[1], vertex.Diffuse[0], vertex.Diffuse[1],
                    vertex.Diffuse[2], vertex.Diffuse[3]});
            }
            const auto& config = asset.Config;
            const float weights[] = {config.NormalWeight, config.NormalWeight, config.NormalWeight,
                config.TexCoordWeight, config.TexCoordWeight,
                config.ColorWeight, config.ColorWeight, config.ColorWeight, config.ColorWeight};
            size_t target = std::max(size_t(1), static_cast<size_t>((mesh.Indices.size() / 3) *
                double(config.SimplificationRatio))) * 3;
            std::vector<uint32_t> simplified(mesh.Indices.size());
            float relativeError = 0;
            size_t count = meshopt_simplifyWithAttributes(simplified.data(), mesh.Indices.data(), mesh.Indices.size(),
                mesh.Vertices[0].Position, mesh.Vertices.size(), sizeof(VirtualGeometryVertex), attributes[0].data(),
                sizeof(attributes[0]), weights, std::size(weights), locks.data(), target,
                config.TargetRelativeError, 0, &relativeError);
            if (count == 0 || count >= mesh.Indices.size())
            {
                return {};
            }
            simplified.resize(count);
            ValidateBoundary(mesh, simplified, protectedEdges);
            float absoluteError = relativeError * meshopt_simplifyScale(mesh.Vertices[0].Position,
                mesh.Vertices.size(), sizeof(VirtualGeometryVertex));
            if (!std::isfinite(absoluteError) || absoluteError < 0)
            {
                throw std::runtime_error("Invalid simplification error");
            }
            std::vector<uint32_t> children;
            float childError = 0;
            for (uint32_t currentIndex : partition)
            {
                uint32_t index = current[currentIndex];
                children.push_back(index);
                childError = std::max(childError, asset.Clusters[index].LodError);
            }
            uint32_t childLevel = asset.Clusters[children[0]].Level;
            size_t vertexStart = asset.Vertices.size();
            size_t triangleStart = asset.TriangleIndices.size();
            size_t clusterStart = asset.Clusters.size();
            float parentError = std::max(childError, absoluteError);
            auto parents = AppendClusters(asset, mesh.Vertices, simplified, childLevel + 1, parentError);
            if (parents.empty() || parents.size() >= children.size())
            {
                asset.Vertices.resize(vertexStart);
                asset.TriangleIndices.resize(triangleStart);
                asset.Clusters.resize(clusterStart);
                return {};
            }
            uint32_t groupIndex = CheckedIndex(asset.Groups.size());
            VirtualGeometryClusterGroup group;
            group.Level = childLevel;
            group.FirstChild = CheckedIndex(asset.HierarchyIndices.size());
            group.ChildCount = CheckedIndex(children.size());
            group.Bounds = GroupBounds(asset, children);
            group.MinError = childError;
            group.MaxError = parentError;
            asset.HierarchyIndices.insert(asset.HierarchyIndices.end(), children.begin(), children.end());
            group.FirstParent = CheckedIndex(asset.HierarchyIndices.size());
            group.ParentCount = CheckedIndex(parents.size());
            CheckedIndex(asset.HierarchyIndices.size() + parents.size());
            asset.HierarchyIndices.insert(asset.HierarchyIndices.end(), parents.begin(), parents.end());
            asset.Groups.push_back(group);
            for (uint32_t child : children)
            {
                asset.Clusters[child].GroupIndex = groupIndex;
            }
            for (uint32_t parent : parents)
            {
                asset.Clusters[parent].GeneratingGroupIndex = groupIndex;
            }
            return parents;
        }
    }

    void BuildHierarchy(VirtualGeometryAsset& asset, std::vector<uint32_t> current)
    {
        for (uint32_t level = 0; level + 1 < asset.Config.MaxHierarchyLevels && current.size() > 1; ++level)
        {
            auto geometry = PartitionLevel(asset, current);
            std::vector<uint32_t> next;
            for (const auto& partition : geometry.Partitions)
            {
                auto parents = SimplifyGroup(asset, current, geometry, partition);
                if (parents.empty())
                {
                    for (uint32_t currentIndex : partition)
                    {
                        asset.RootClusters.push_back(current[currentIndex]);
                    }
                }
                else
                {
                    next.insert(next.end(), parents.begin(), parents.end());
                }
            }
            if (next.empty())
            {
                current.clear();
                break;
            }
            if (next.size() >= current.size())
            {
                current = std::move(next);
                break;
            }
            current = std::move(next);
        }
        asset.RootClusters.insert(asset.RootClusters.end(), current.begin(), current.end());
        std::sort(asset.RootClusters.begin(), asset.RootClusters.end());
    }
}
