#pragma once
#include "virtualgeometryasset.h"
#include <span>
#include <stdexcept>
#include <limits>

namespace VirtualGeometryInternal
{
    inline uint32_t CheckedIndex(size_t value)
    {
        if (value >= VirtualGeometryConfig::InvalidIndex)
        {
            throw std::runtime_error("Virtual geometry exceeds 32-bit asset addressing");
        }
        return static_cast<uint32_t>(value);
    }

    inline bool RangeFits(uint64_t offset, uint64_t count, uint64_t size)
    {
        return offset <= size && count <= size - offset;
    }

    bool IsFiniteVertex(const VirtualGeometryVertex& vertex);
    std::array<float, 12> VertexComponents(const VirtualGeometryVertex& vertex);
    std::vector<uint32_t> CleanSource(std::span<const VirtualGeometryVertex> vertices,
                                      std::span<const uint32_t> indices, VirtualGeometryBuildStats& stats);
    std::vector<uint32_t> AppendClusters(VirtualGeometryAsset& asset, std::span<const VirtualGeometryVertex> vertices,
                                         std::span<const uint32_t> indices, uint32_t level, float error);
    void BuildHierarchy(VirtualGeometryAsset& asset, std::vector<uint32_t> current);
    void UpdateStatistics(VirtualGeometryAsset& asset);
    VirtualGeometrySphere SphereFromAabb(const VirtualGeometryAabb& bounds);
} // 名前空間 VirtualGeometryInternal の終わり。
