#include "virtualgeometryinternal.h"
#include "../../External/meshoptimizer/src/meshoptimizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

bool ValidateVirtualGeometryConfig(const VirtualGeometryBuildConfig& config, string& error)
{
    error.clear();
    if (config.MaxClusterVertices < 3 || config.MaxClusterVertices > VirtualGeometryConfig::MaxClusterVertices ||
        config.MaxClusterTriangles == 0 || config.MaxClusterTriangles > VirtualGeometryConfig::MaxClusterTriangles ||
        config.TargetClustersPerGroup == 0 || config.TargetClustersPerGroup > 1024 ||
        config.MaxHierarchyLevels == 0 || config.MaxHierarchyLevels > VirtualGeometryConfig::MaxHierarchyLevels)
    {
        error = "Invalid virtual geometry cluster/group/level limits";
        return false;
    }
    const float weights[] = {config.NormalWeight, config.TexCoordWeight, config.ColorWeight};
    for (float weight : weights)
    {
        if (!isfinite(weight) || weight < 0 || weight > 1000)
        {
            error = "Attribute weights must be finite and within [0, 1000]";
            return false;
        }
    }
    if (!isfinite(config.SimplificationRatio) || config.SimplificationRatio <= 0 || config.SimplificationRatio >= 1 ||
        !isfinite(config.ConeWeight) || config.ConeWeight < 0 || config.ConeWeight > 1 ||
        !isfinite(config.TargetRelativeError) || config.TargetRelativeError < 0 || config.TargetRelativeError > 1 ||
        !isfinite(config.BoundaryEpsilon) || config.BoundaryEpsilon < 0 || config.BoundaryEpsilon > 1)
    {
        error = "Invalid virtual geometry simplification/boundary settings";
        return false;
    }
    return true;
}

namespace VirtualGeometryInternal
{
    array<float, 12> VertexComponents(const VirtualGeometryVertex& vertex)
    {
        return {vertex.Position[0], vertex.Position[1], vertex.Position[2],
            vertex.Normal[0], vertex.Normal[1], vertex.Normal[2],
            vertex.TexCoord[0], vertex.TexCoord[1],
            vertex.Diffuse[0], vertex.Diffuse[1], vertex.Diffuse[2], vertex.Diffuse[3]};
    }

    bool IsFiniteVertex(const VirtualGeometryVertex& vertex)
    {
        auto components = VertexComponents(vertex);
        return all_of(components.begin(), components.end(), [](float value)
        {
            return isfinite(value);
        });
    }

    namespace
    {
        struct TriangleKey
        {
            array<uint32_t, 3> Vertices;
            bool operator==(const TriangleKey&) const = default;
        };
        struct TriangleHash
        {
            size_t operator()(const TriangleKey& key) const
            {
                size_t hash = 0;
                for (uint32_t index : key.Vertices)
                {
                    hash = hash * 16777619u ^ index;
                }
                return hash;
            }
        };

        bool HasZeroArea(const VirtualGeometryVertex& first,
            const VirtualGeometryVertex& second, const VirtualGeometryVertex& third)
        {
            double firstEdge[3];
            double secondEdge[3];
            for (size_t axis = 0; axis < 3; ++axis)
            {
                firstEdge[axis] = double(second.Position[axis]) - first.Position[axis];
                secondEdge[axis] = double(third.Position[axis]) - first.Position[axis];
            }
            double crossX = firstEdge[1] * secondEdge[2] - firstEdge[2] * secondEdge[1];
            double crossY = firstEdge[2] * secondEdge[0] - firstEdge[0] * secondEdge[2];
            double crossZ = firstEdge[0] * secondEdge[1] - firstEdge[1] * secondEdge[0];
            return crossX == 0 && crossY == 0 && crossZ == 0;
        }

        VirtualGeometryNormalCone ComputeCone(span<const VirtualGeometryVertex> vertices,
            const uint32_t* localVertices, const uint8_t* triangles, uint32_t triangleCount,
            const VirtualGeometrySphere& sphere)
        {
            // Double intermediates keep tiny finite triangles from becoming zero normals
            // and large finite triangles from overflowing their squared cross products.
            vector<array<double, 3>> normals;
            vector<array<double, 3>> origins;
            array<double, 3> axis{};
            for (uint32_t triangle = 0; triangle < triangleCount; ++triangle)
            {
                const auto& first = vertices[localVertices[triangles[triangle * 3]]].Position;
                const auto& second = vertices[localVertices[triangles[triangle * 3 + 1]]].Position;
                const auto& third = vertices[localVertices[triangles[triangle * 3 + 2]]].Position;
                array<double, 3> firstEdge;
                array<double, 3> secondEdge;
                for (size_t component = 0; component < 3; ++component)
                {
                    firstEdge[component] = double(second[component]) - first[component];
                    secondEdge[component] = double(third[component]) - first[component];
                }
                array<double, 3> normal = {
                    firstEdge[1] * secondEdge[2] - firstEdge[2] * secondEdge[1],
                    firstEdge[2] * secondEdge[0] - firstEdge[0] * secondEdge[2],
                    firstEdge[0] * secondEdge[1] - firstEdge[1] * secondEdge[0]};
                double length = sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                if (length == 0)
                {
                    continue;
                }
                for (size_t component = 0; component < 3; ++component)
                {
                    normal[component] /= length;
                    axis[component] += normal[component];
                }
                normals.push_back(normal);
                origins.push_back({first[0], first[1], first[2]});
            }
            VirtualGeometryNormalCone cone;
            cone.Apex = sphere.Center;
            double axisLength = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            if (axisLength == 0 || normals.empty())
            {
                return cone; // Cutoff=1 disables rejection for a wide/degenerate cone.
            }
            for (size_t component = 0; component < 3; ++component)
            {
                axis[component] /= axisLength;
            }
            double minimumDot = 1;
            for (const auto& normal : normals)
            {
                minimumDot = min(minimumDot, normal[0] * axis[0] + normal[1] * axis[1] + normal[2] * axis[2]);
            }
            constexpr double minimumUsefulConeDot = 0.1;
            if (minimumDot <= minimumUsefulConeDot)
            {
                return cone;
            }
            double apexDistance = 0;
            double axisRoundingError = 0;
            for (size_t component = 0; component < 3; ++component)
            {
                cone.Axis[component] = static_cast<float>(axis[component]);
                axisRoundingError += abs(double(cone.Axis[component]) - axis[component]);
            }
            for (size_t triangle = 0; triangle < normals.size(); ++triangle)
            {
                const auto& normal = normals[triangle];
                double distance = 0;
                double dot = 0;
                for (size_t component = 0; component < 3; ++component)
                {
                    distance += (double(sphere.Center[component]) - origins[triangle][component]) * normal[component];
                    dot += cone.Axis[component] * normal[component];
                }
                apexDistance = max(apexDistance, distance / dot);
            }
            for (size_t component = 0; component < 3; ++component)
            {
                cone.Apex[component] = static_cast<float>(double(sphere.Center[component]) - cone.Axis[component] * apexDistance);
            }
            double cutoff = sqrt(max(0.0, 1 - minimumDot * minimumDot)) + axisRoundingError;
            cone.Cutoff = min(1.0f, nextafter(static_cast<float>(cutoff),
                numeric_limits<float>::infinity()));
            return cone;
        }
    }

    vector<uint32_t> CleanSource(span<const VirtualGeometryVertex> vertices,
        span<const uint32_t> indices, VirtualGeometryBuildStats& stats)
    {
        if (vertices.empty() || indices.empty() || indices.size() % 3 != 0)
        {
            throw runtime_error("Source must contain vertices and complete triangles");
        }
        CheckedIndex(vertices.size());
        CheckedIndex(indices.size());
        // Avoid overflow inside meshoptimizer's float metrics, including unused input vertices.
        constexpr float maximumComponentMagnitude = 1e8f;
        for (const auto& vertex : vertices)
        {
            for (float value : VertexComponents(vertex))
            {
                if (!isfinite(value) || abs(value) > maximumComponentMagnitude)
                {
                    throw runtime_error("Non-finite or unsupported extreme source vertex component");
                }
            }
        }
        for (uint32_t index : indices)
        {
            if (index >= vertices.size())
            {
                throw runtime_error("Source triangle index out of bounds");
            }
        }
        // Exact full-attribute remap identifies duplicate vertices without welding UV/normal seams.
        vector<uint32_t> remap(vertices.size());
        meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(),
            vertices.data(), vertices.size(), sizeof(VirtualGeometryVertex));
        unordered_set<TriangleKey, TriangleHash> seen;
        seen.reserve(indices.size() / 3);
        vector<uint32_t> clean;
        clean.reserve(indices.size());
        for (size_t offset = 0; offset < indices.size(); offset += 3)
        {
            uint32_t first = indices[offset];
            uint32_t second = indices[offset + 1];
            uint32_t third = indices[offset + 2];
            if (HasZeroArea(vertices[first], vertices[second], vertices[third]))
            {
                ++stats.RemovedDegenerateTriangles;
                continue;
            }
            // Rotate to canonical winding. Opposite-facing triangles are intentionally retained.
            TriangleKey key{{remap[first], remap[second], remap[third]}};
            auto smallest = min_element(key.Vertices.begin(), key.Vertices.end());
            rotate(key.Vertices.begin(), smallest, key.Vertices.end());
            if (!seen.insert(key).second)
            {
                ++stats.RemovedDuplicateTriangles;
                continue;
            }
            clean.insert(clean.end(), {first, second, third});
        }
        if (clean.empty())
        {
            throw runtime_error("Source has no non-degenerate unique triangles");
        }
        stats.FinalLeafTriangles = clean.size() / 3;
        return clean;
    }

    VirtualGeometrySphere SphereFromAabb(const VirtualGeometryAabb& bounds)
    {
        VirtualGeometrySphere sphere;
        double squaredRadius = 0;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            sphere.Center[axis] = static_cast<float>((double(bounds.Min[axis]) + bounds.Max[axis]) * 0.5);
            double extent = max(abs(double(bounds.Min[axis]) - sphere.Center[axis]),
                abs(double(bounds.Max[axis]) - sphere.Center[axis]));
            squaredRadius += extent * extent;
        }
        sphere.Radius = nextafter(static_cast<float>(sqrt(squaredRadius)),
            numeric_limits<float>::infinity());
        return sphere;
    }

    vector<uint32_t> AppendClusters(VirtualGeometryAsset& asset,
        span<const VirtualGeometryVertex> vertices, span<const uint32_t> indices,
        uint32_t level, float error)
    {
        const auto& config = asset.Config;
        size_t bound = meshopt_buildMeshletsBound(indices.size(), config.MaxClusterVertices, config.MaxClusterTriangles);
        vector<meshopt_Meshlet> meshlets(bound);
        vector<uint32_t> meshletVertices(indices.size());
        vector<uint8_t> meshletTriangles(indices.size());
        size_t count = meshopt_buildMeshlets(meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
            indices.data(), indices.size(), vertices[0].Position, vertices.size(), sizeof(VirtualGeometryVertex),
            config.MaxClusterVertices, config.MaxClusterTriangles, config.ConeWeight);
        vector<uint32_t> clusters;
        clusters.reserve(count);
        for (size_t index = 0; index < count; ++index)
        {
            const auto& meshlet = meshlets[index];
            uint32_t* localVertices = meshletVertices.data() + meshlet.vertex_offset;
            uint8_t* triangles = meshletTriangles.data() + meshlet.triangle_offset;
            meshopt_optimizeMeshlet(localVertices, triangles, meshlet.triangle_count, meshlet.vertex_count);
            VirtualGeometryCluster cluster;
            cluster.VertexOffset = CheckedIndex(asset.Vertices.size());
            cluster.TriangleOffset = CheckedIndex(asset.TriangleIndices.size());
            CheckedIndex(asset.Vertices.size() + meshlet.vertex_count);
            CheckedIndex(asset.TriangleIndices.size() + size_t(meshlet.triangle_count) * 3);
            cluster.VertexCount = meshlet.vertex_count;
            cluster.TriangleCount = meshlet.triangle_count;
            cluster.Level = level;
            cluster.MaterialId = asset.SourceMaterialId;
            cluster.LodError = error;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                cluster.Aabb.Min[axis] = numeric_limits<float>::max();
                cluster.Aabb.Max[axis] = numeric_limits<float>::lowest();
            }
            for (uint32_t vertexIndex = 0; vertexIndex < meshlet.vertex_count; ++vertexIndex)
            {
                const auto& vertex = vertices[localVertices[vertexIndex]];
                asset.Vertices.push_back(vertex);
                for (size_t axis = 0; axis < 3; ++axis)
                {
                    cluster.Aabb.Min[axis] = min(cluster.Aabb.Min[axis], vertex.Position[axis]);
                    cluster.Aabb.Max[axis] = max(cluster.Aabb.Max[axis], vertex.Position[axis]);
                }
            }
            cluster.Bounds = SphereFromAabb(cluster.Aabb);
            cluster.Cone = ComputeCone(vertices, localVertices, triangles, meshlet.triangle_count, cluster.Bounds);
            asset.TriangleIndices.insert(asset.TriangleIndices.end(), triangles, triangles + meshlet.triangle_count * 3);
            clusters.push_back(CheckedIndex(asset.Clusters.size()));
            asset.Clusters.push_back(cluster);
        }
        return clusters;
    }
}
