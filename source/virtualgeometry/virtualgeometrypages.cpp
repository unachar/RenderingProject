#include "virtualgeometrypages.h"
#include "virtualgeometryserializationinternal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <limits>
#include <iostream>
#include <stdexcept>

namespace VirtualGeometryPages
{
    bool Validate(const VirtualGeometryGPU::PackedAsset& metadata, std::span<const uint8_t> pinned, std::string& error)
    {
        auto fail = [&](const char* message) {
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
            bool finiteBounds = group.Bounds[3] >= 0;
            for (float component : group.Bounds)
            {
                finiteBounds &= std::isfinite(component);
            }
            if (group.Level >= 31 || !group.ChildCount || !group.ParentCount ||
                uint64_t(group.FirstChild) + group.ChildCount > metadata.HierarchyIndices.size() ||
                uint64_t(group.FirstParent) + group.ParentCount > metadata.HierarchyIndices.size() || !finiteBounds ||
                !std::isfinite(group.MaxError) || !std::isfinite(group.MinError) || group.MinError < 0 ||
                group.MaxError < group.MinError)
            {
                return fail("Invalid paged group range/error");
            }
            for (uint32_t child = 0; child < group.ChildCount; ++child)
            {
                uint32_t cluster = metadata.HierarchyIndices[group.FirstChild + child];
                if (cluster >= consumers.size() || consumers[cluster] != UINT32_MAX ||
                    metadata.Clusters[cluster].Level != group.Level)
                {
                    return fail("Invalid paged child link");
                }
                consumers[cluster] = index;
            }
            for (uint32_t parent = 0; parent < group.ParentCount; ++parent)
            {
                uint32_t cluster = metadata.HierarchyIndices[group.FirstParent + parent];
                if (cluster >= producers.size() || producers[cluster] != UINT32_MAX ||
                    metadata.Clusters[cluster].Level != group.Level + 1)
                {
                    return fail("Invalid paged parent link");
                }
                producers[cluster] = index;
            }
        }
        for (size_t index = 0; index < metadata.Clusters.size(); ++index)
        {
            const auto& cluster = metadata.Clusters[index];
            bool finiteBounds = cluster.Bounds[3] >= 0 && std::isfinite(cluster.LodError) && cluster.LodError >= 0;
            for (float component : cluster.Bounds)
            {
                finiteBounds &= std::isfinite(component);
            }
            for (float component : cluster.Cone)
            {
                finiteBounds &= std::isfinite(component);
            }
            for (float component : cluster.ConeApex)
            {
                finiteBounds &= std::isfinite(component);
            }
            bool encoded = (cluster.Flags & VirtualGeometryCompression::ClusterFlag) != 0;
            uint64_t vertexEnd = uint64_t(cluster.VertexOffset) + uint64_t(cluster.VertexCount) * 48;
            uint64_t triangleEnd =
                (uint64_t(cluster.TriangleOffset) + uint64_t(cluster.TriangleCount) * 3 + 3) & ~uint64_t(3);
            bool invalidLayout = vertexEnd > PageBytes || triangleEnd > PageBytes || cluster.TriangleOffset < vertexEnd;
            if (encoded)
            {
                uint64_t end = uint64_t(cluster.VertexOffset) + cluster.Padding;
                invalidLayout =
                    cluster.Padding < VirtualGeometryCompression::HeaderBytes || cluster.Padding % 4 ||
                    end > PageBytes ||
                    cluster.TriangleOffset < cluster.VertexOffset + VirtualGeometryCompression::HeaderBytes ||
                    cluster.TriangleOffset >= end;
            }
            if (!cluster.VertexCount || cluster.VertexCount > 64 || !cluster.TriangleCount ||
                cluster.TriangleCount > 124 || cluster.PageIndex >= pinned.size() || cluster.VertexOffset % 4 ||
                cluster.TriangleOffset % 4 || invalidLayout || !finiteBounds || cluster.Level >= 32 ||
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
        return Build(asset, VGCompressionSettings{}, output, error);
    }

    bool Build(const VirtualGeometryAsset& asset, const VGCompressionSettings& settings, PagedAsset& output,
               std::string& error)
    {
        return Build(asset, VirtualGeometryCompression::Input{}, settings, output, error);
    }

    bool Build(const VirtualGeometryAsset& asset, const VirtualGeometryCompression::Input& attributes,
               const VGCompressionSettings& settings, PagedAsset& output, std::string& error)
    {
        output = {};
        try
        {
            PagedAsset result;
            result.SourceTriangles = asset.Stats.SourceTriangles;
            result.SourceVertices = asset.Stats.SourceVertices;
            if (!VirtualGeometryGPU::Pack(asset, result.Metadata, error))
            {
                return false;
            }
            VirtualGeometryCompression::Input input = attributes;
            input.Vertices = asset.Vertices;
            VirtualGeometryCompression::Plan plan;
            if (settings.Enabled && !VirtualGeometryCompression::Select(input, settings, plan, error))
            {
                error = asset.DebugName + ": " + error;
                return false;
            }
            result.CompressionFallbacks = plan.FallbackReasons;
            for (const auto& reason : plan.FallbackReasons)
            {
                std::clog << "Geometry compression " << asset.DebugName << ": " << reason << '\n';
            }
            result.Compression.FallbackCount = plan.FallbackReasons.size();
            for (const auto& stream : plan.Streams)
            {
                result.Compression.PositionQuantized += stream.Encoding == VirtualGeometryCompression::Codec::UNorm16x3;
                result.Compression.HalfUV += stream.Encoding == VirtualGeometryCompression::Codec::Float16x2;
            }
            std::vector<uint8_t> roots(asset.Clusters.size());
            for (uint32_t root : asset.RootClusters)
            {
                roots[root] = 1;
            }
            std::vector<uint32_t> order(asset.Clusters.size());
            std::iota(order.begin(), order.end(), 0u);
            std::stable_sort(order.begin(), order.end(), [&](uint32_t first, uint32_t second) {
                return roots[first] > roots[second];
            });
            uint32_t rawStride = 48;
            if (settings.Enabled)
            {
                rawStride = 0;
                for (const auto& stream : plan.Streams)
                {
                    if (stream.Attribute >= VirtualGeometryCompression::Texcoord0)
                    {
                        rawStride += 8;
                    }
                    else if (stream.Attribute == VirtualGeometryCompression::Tangent ||
                             stream.Attribute == VirtualGeometryCompression::Color)
                    {
                        rawStride += 16;
                    }
                    else
                    {
                        rawStride += 12;
                    }
                }
            }
            uint32_t used = PageBytes;
            bool previousPinned = true;
            for (uint32_t index : order)
            {
                const auto& source = asset.Clusters[index];
                auto& target = result.Metadata.Clusters[index];
                std::vector<uint32_t> indices;
                indices.reserve(source.TriangleCount * 3);
                for (uint32_t triangle = 0; triangle < source.TriangleCount * 3; ++triangle)
                {
                    indices.push_back(asset.TriangleIndices[source.TriangleOffset + triangle]);
                }
                std::vector<uint8_t> payload;
                if (settings.Enabled &&
                    !VirtualGeometryCompression::EncodeCluster(input, plan, source.VertexOffset, source.VertexCount,
                                                               indices, payload, error))
                {
                    error = asset.DebugName + " cluster " + std::to_string(index) + ": " + error;
                    return false;
                }
                if (!settings.Enabled)
                {
                    VirtualGeometrySerialization::Writer writer;
                    for (uint32_t vertex = 0; vertex < source.VertexCount; ++vertex)
                    {
                        const auto& value = asset.Vertices[source.VertexOffset + vertex];
                        for (float component : value.Position)
                        {
                            writer.F32(component);
                        }
                        for (float component : value.Normal)
                        {
                            writer.F32(component);
                        }
                        for (float component : value.TexCoord)
                        {
                            writer.F32(component);
                        }
                        for (float component : value.Diffuse)
                        {
                            writer.F32(component);
                        }
                    }
                    for (uint32_t index : indices)
                    {
                        writer.Bytes.push_back(uint8_t(index));
                    }
                    writer.Bytes.resize((writer.Bytes.size() + 3) & ~size_t(3), 0);
                    payload = std::move(writer.Bytes);
                }
                uint32_t total = uint32_t(payload.size());
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
                // ヘッダーはメッシュ全体の量子化メタデータも兼ねる。内部オフセットはクラスター相対。
                uint32_t indexOffset = source.VertexCount * 48;
                if (settings.Enabled)
                {
                    indexOffset = uint32_t(payload[24]) | (uint32_t(payload[25]) << 8) | (uint32_t(payload[26]) << 16) |
                                  (uint32_t(payload[27]) << 24);
                    target.Flags |= VirtualGeometryCompression::ClusterFlag;
                    target.Padding = total;
                }
                target.TriangleOffset = used + indexOffset;
                // 中心を変えず、復号した位置を使って球の半径を合わせ直す。
                // 量子化で三角形の法線が変わるため、頂点のテストも含めて古いコーンを無効化する。
                double radius = target.Bounds[3];
                for (uint32_t vertex = 0; settings.Enabled && vertex < source.VertexCount; ++vertex)
                {
                    auto decoded = VirtualGeometryCompression::DecodeVertexCPU(payload, vertex);
                    double squared = 0;
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        double delta = double(decoded.Position[axis]) - target.Bounds[axis];
                        squared += delta * delta;
                    }
                    radius = std::max(radius, std::sqrt(squared));
                }
                if (settings.Enabled)
                {
                    target.Bounds[3] = std::nextafter(float(radius), std::numeric_limits<float>::infinity());
                }
                if (plan.PositionError > 0)
                {
                    target.Cone[3] = 1;
                    target.LodError =
                        std::nextafter(target.LodError + plan.PositionError, std::numeric_limits<float>::infinity());
                }
                result.Compression.RawBytes +=
                    uint64_t(source.VertexCount) * rawStride + uint64_t(source.TriangleCount) * 3;
                result.Compression.EncodedBytes += total;
                if (!settings.Enabled || (settings.AllowIndex8 && source.VertexCount <= 256))
                {
                    ++result.Compression.Index8Clusters;
                }
                else
                {
                    ++result.Compression.Index16Clusters;
                }
                auto& bytes = result.Payloads.back();
                std::copy(payload.begin(), payload.end(), bytes.begin() + used);
                used += total;
            }
            for (auto& group : result.Metadata.Groups)
            {
                // グループの球は、すでにすべての子孫の元頂点を覆っている。
                if (plan.PositionError > 0)
                {
                    group.Bounds[3] =
                        std::nextafter(group.Bounds[3] + plan.PositionError, std::numeric_limits<float>::infinity());
                    group.MaxError =
                        std::nextafter(group.MaxError + plan.PositionError, std::numeric_limits<float>::infinity());
                }
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
} // 名前空間 VirtualGeometryPages の終わり。
