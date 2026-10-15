#include "virtualgeometrypages.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace VirtualGeometryPages
{
bool Validate(const VirtualGeometryGPU::PackedAsset& metadata, std::span<const uint8_t> pinned, std::string& error)
{
    auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    if (metadata.Clusters.empty() || metadata.Roots.empty() || pinned.empty())
    {
        return fail("Empty paged hierarchy");
    }
    std::vector<uint32_t> consumers(metadata.Clusters.size(), UINT32_MAX);
    std::vector<uint32_t> producers(metadata.Clusters.size(), UINT32_MAX);
    std::vector<uint8_t> roots(metadata.Clusters.size());
    for (uint32_t root : metadata.Roots)
    {
        if (root >= roots.size() || roots[root])
        {
            return fail("Invalid paged root");
        }
        roots[root] = 1;
    }
    for (uint32_t index = 0; index < metadata.Groups.size(); ++index)
    {
        const auto& group = metadata.Groups[index];
        if (group.Level >= 31 || !group.ChildCount || !group.ParentCount ||
            uint64_t(group.FirstChild) + group.ChildCount > metadata.HierarchyIndices.size() ||
            uint64_t(group.FirstParent) + group.ParentCount > metadata.HierarchyIndices.size() ||
            !std::isfinite(group.MaxError) || !std::isfinite(group.MinError) || group.MinError < 0 || group.MaxError < group.MinError)
        {
            return fail("Invalid paged group range/error");
        }
        for (uint32_t child = 0; child < group.ChildCount; ++child)
        {
            uint32_t cluster = metadata.HierarchyIndices[group.FirstChild + child];
            if (cluster >= consumers.size() || consumers[cluster] != UINT32_MAX || metadata.Clusters[cluster].Level != group.Level)
            {
                return fail("Invalid paged child link");
            }
            consumers[cluster] = index;
        }
        for (uint32_t parent = 0; parent < group.ParentCount; ++parent)
        {
            uint32_t cluster = metadata.HierarchyIndices[group.FirstParent + parent];
            if (cluster >= producers.size() || producers[cluster] != UINT32_MAX || metadata.Clusters[cluster].Level != group.Level + 1)
            {
                return fail("Invalid paged parent link");
            }
            producers[cluster] = index;
        }
    }
    for (size_t index = 0; index < metadata.Clusters.size(); ++index)
    {
        const auto& cluster = metadata.Clusters[index];
        uint64_t vertexEnd = uint64_t(cluster.VertexOffset) + uint64_t(cluster.VertexCount) * 48;
        uint64_t triangleEnd = (uint64_t(cluster.TriangleOffset) + uint64_t(cluster.TriangleCount) * 3 + 3) & ~uint64_t(3);
        if (!cluster.VertexCount || cluster.VertexCount > 64 || !cluster.TriangleCount || cluster.TriangleCount > 124 ||
            cluster.PageIndex >= pinned.size() || cluster.VertexOffset % 4 || cluster.TriangleOffset % 4 ||
            vertexEnd > PageBytes || triangleEnd > PageBytes || cluster.TriangleOffset < vertexEnd || cluster.Level >= 32 ||
            cluster.GroupIndex != consumers[index] || cluster.GeneratingGroupIndex != producers[index] ||
            bool(roots[index]) != (consumers[index] == UINT32_MAX) ||
            (cluster.Level != 0 && producers[index] == UINT32_MAX) || (roots[index] && !pinned[cluster.PageIndex]))
        {
            return fail("Invalid paged cluster layout/link");
        }
    }
    error.clear();
    return true;
}

bool Build(const VirtualGeometryAsset& asset, PagedAsset& output, std::string& error)
{
    output = {};
    try
    {
        PagedAsset result;
        if (!VirtualGeometryGPU::Pack(asset, result.Metadata, error))
        {
            return false;
        }
        std::vector<uint8_t> roots(asset.Clusters.size());
        for (uint32_t root : asset.RootClusters)
        {
            roots[root] = 1;
        }
        std::vector<uint32_t> order(asset.Clusters.size());
        std::iota(order.begin(), order.end(), 0u);
        std::stable_sort(order.begin(), order.end(), [&](uint32_t first, uint32_t second)
        {
            return roots[first] > roots[second];
        });
        uint32_t used = PageBytes;
        bool previousPinned = true;
        for (uint32_t index : order)
        {
            const auto& source = asset.Clusters[index];
            auto& target = result.Metadata.Clusters[index];
            uint32_t vertexBytes = source.VertexCount * 48;
            uint32_t triangleBytes = source.TriangleCount * 3;
            uint32_t total = (vertexBytes + triangleBytes + 3) & ~3u;
            bool pinned = roots[index] != 0;
            if (total > PageBytes - used || pinned != previousPinned)
            {
                if (result.Payloads.size() >= UINT32_MAX)
                {
                    throw std::runtime_error("Geometry Page ID overflow");
                }
                result.Payloads.emplace_back(PageBytes, 0);
                result.PinnedPages.push_back(uint8_t(pinned));
                used = 0;
            }
            previousPinned = pinned;
            target.PageIndex = uint32_t(result.Payloads.size() - 1);
            target.VertexOffset = used;
            target.TriangleOffset = used + vertexBytes;
            auto& bytes = result.Payloads.back();
            std::memcpy(bytes.data() + used, asset.Vertices.data() + source.VertexOffset, vertexBytes);
            std::memcpy(bytes.data() + used + vertexBytes, asset.TriangleIndices.data() + source.TriangleOffset, triangleBytes);
            used += total;
        }
        result.Metadata.Vertices = {};
        result.Metadata.Triangles = {};
        if (!Validate(result.Metadata, result.PinnedPages, error))
        {
            return false;
        }
        output = std::move(result);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
}
