#include "virtualgeometrypagestress.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace VirtualGeometryPageStress
{
    bool Create(const std::filesystem::path& path, uint32_t leafCount, VirtualGeometryPageCache::PageManifest& manifest,
                std::string& error)
    {
        // 各部分木は同じ平面上の連続した帯になる。その粗い格子は、
        // 8 つの子を合わせたものとまったく同じ矩形を覆う。
        uint32_t power = 1;
        while (power < leafCount && power <= LeavesPerAsset / 8)
        {
            power *= 8;
        }
        if (power != leafCount || leafCount < 8 || leafCount > LeavesPerAsset)
        {
            error = "Stress asset leaf count must be a power of eight, 8..32768";
            return false;
        }

        struct Strip
        {
            uint32_t First;
            uint32_t Count;
        };

        VirtualGeometryGPU::PackedAsset metadata;
        std::vector<Strip> strips;
        auto appendCluster = [&](Strip strip, uint32_t level) {
            VirtualGeometryGPUCluster cluster;
            cluster.VertexCount = 36;
            cluster.TriangleCount = 50;
            cluster.Level = level;
            float first = -4 + 8 * (float(strip.First) / leafCount);
            float width = 8 * (float(strip.Count) / leafCount);
            cluster.Bounds = {first + width * 0.5f, 0, 0, std::sqrt(16 + width * width * 0.25f)};
            cluster.Cone = {0, 0, -1, 1};
            cluster.LodError = float(level);
            metadata.Clusters.push_back(cluster);
            strips.push_back(strip);
        };
        std::vector<uint32_t> current;
        for (uint32_t leaf = 0; leaf < leafCount; ++leaf)
        {
            appendCluster({leaf, 1}, 0);
            current.push_back(leaf);
        }
        uint32_t level = 0;
        while (current.size() > 1)
        {
            std::vector<uint32_t> parents;
            for (size_t begin = 0; begin < current.size(); begin += 8)
            {
                uint32_t groupIndex = uint32_t(metadata.Groups.size());
                VirtualGeometryGPUGroup group;
                group.Level = level;
                group.FirstChild = uint32_t(metadata.HierarchyIndices.size());
                group.ChildCount = 8;
                for (uint32_t offset = 0; offset < 8; ++offset)
                {
                    uint32_t child = current[begin + offset];
                    metadata.Clusters[child].GroupIndex = groupIndex;
                    metadata.HierarchyIndices.push_back(child);
                }
                uint32_t parent = uint32_t(metadata.Clusters.size());
                auto strip = strips[current[begin]];
                strip.Count *= 8;
                appendCluster(strip, level + 1);
                metadata.Clusters.back().GeneratingGroupIndex = groupIndex;
                group.FirstParent = uint32_t(metadata.HierarchyIndices.size());
                group.ParentCount = 1;
                group.Bounds = metadata.Clusters.back().Bounds;
                group.MaxError = float(level + 1);
                metadata.HierarchyIndices.push_back(parent);
                metadata.Groups.push_back(group);
                parents.push_back(parent);
            }
            current = std::move(parents);
            ++level;
        }
        metadata.Roots = current;
        std::vector<uint32_t> order(metadata.Clusters.size());
        std::iota(order.begin(), order.end(), 0u);
        std::reverse(order.begin(), order.end());
        std::vector<std::vector<uint32_t>> pageClusters;
        std::vector<uint8_t> pinned;
        uint32_t used = 65536;
        bool wasPinned = true;
        for (uint32_t index : order)
        {
            bool root = index == current[0];
            if (1880 > 65536 - used || root != wasPinned)
            {
                pageClusters.emplace_back();
                pinned.push_back(uint8_t(root));
                used = 0;
            }
            wasPinned = root;
            auto& cluster = metadata.Clusters[index];
            cluster.PageIndex = uint32_t(pageClusters.size() - 1);
            cluster.VertexOffset = used;
            cluster.TriangleOffset = used + 36 * 48;
            pageClusters.back().push_back(index);
            used += 1880;
        }
        bool saved = VirtualGeometryPageCache::WriteGenerated(
            path, metadata, pinned,
            [&](uint32_t page, std::span<uint8_t> payload, std::string&) {
                std::fill(payload.begin(), payload.end(), 0);
                for (uint32_t index : pageClusters[page])
                {
                    const auto& cluster = metadata.Clusters[index];
                    auto strip = strips[index];
                    float first = -4 + 8 * (float(strip.First) / leafCount);
                    float width = 8 * (float(strip.Count) / leafCount);
                    for (uint32_t y = 0; y < 6; ++y)
                    {
                        for (uint32_t x = 0; x < 6; ++x)
                        {
                            VirtualGeometryVertex vertex;
                            vertex.Position[0] = first + width * (float(x) / 5);
                            vertex.Position[1] = -4 + 8 * (float(y) / 5);
                            vertex.Normal[2] = -1;
                            std::fill(std::begin(vertex.Diffuse), std::end(vertex.Diffuse), 1.0f);
                            std::memcpy(payload.data() + cluster.VertexOffset + (y * 6 + x) * 48, &vertex, 48);
                        }
                    }
                    uint32_t triangle = cluster.TriangleOffset;
                    for (uint8_t y = 0; y < 5; ++y)
                    {
                        for (uint8_t x = 0; x < 5; ++x)
                        {
                            uint8_t firstVertex = uint8_t(y * 6 + x);
                            const uint8_t indices[] = {firstVertex,
                                                       uint8_t(firstVertex + 6),
                                                       uint8_t(firstVertex + 1),
                                                       uint8_t(firstVertex + 1),
                                                       uint8_t(firstVertex + 6),
                                                       uint8_t(firstVertex + 7)};
                            std::memcpy(payload.data() + triangle, indices, 6);
                            triangle += 6;
                        }
                    }
                }
                return true;
            },
            error);
        return saved && VirtualGeometryPageCache::Open(path, manifest, error);
    }
} // 名前空間 VirtualGeometryPageStress の終わり。
