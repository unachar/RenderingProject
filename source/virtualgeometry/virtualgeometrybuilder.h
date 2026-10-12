#pragma once
#include "virtualgeometryasset.h"
#include <span>

struct StaticMeshData;

class VirtualGeometryBuilder
{
public:
    static VirtualGeometryBuildResult Build(
        std::span<const VirtualGeometryVertex> vertices,
        std::span<const uint32_t> indices,
        const VirtualGeometryBuildConfig& config = {},
        uint32_t materialId = 0);

    // LOD0 only. Caller supplies the source resource's section index as materialId.
    static VirtualGeometryBuildResult Build(
        const StaticMeshData& source, const VirtualGeometryBuildConfig& config,
        uint32_t materialId);
};
