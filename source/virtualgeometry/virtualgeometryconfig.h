#pragma once
#include <cstdint>
#include <string>

namespace VirtualGeometryConfig
{
    inline constexpr uint32_t MaxClusterTriangles = 128;
    inline constexpr uint32_t MaxClusterVertices = 256;
    inline constexpr uint32_t TargetClustersPerGroup = 8;
    inline constexpr uint32_t MaxHierarchyLevels = 32;
    inline constexpr uint32_t InvalidIndex = UINT32_MAX;
    inline constexpr uint32_t AssetVersion = 1;
}

struct VirtualGeometryBuildConfig
{
    uint32_t MaxClusterTriangles = VirtualGeometryConfig::MaxClusterTriangles;
    uint32_t MaxClusterVertices = VirtualGeometryConfig::MaxClusterVertices;
    uint32_t TargetClustersPerGroup = VirtualGeometryConfig::TargetClustersPerGroup;
    uint32_t MaxHierarchyLevels = VirtualGeometryConfig::MaxHierarchyLevels;
    float SimplificationRatio = 0.5f;
    float ConeWeight = 0.5f;
    float NormalWeight = 1.0f;
    float TexCoordWeight = 1.0f;
    float ColorWeight = 0.25f;
    float TargetRelativeError = 1.0f;
    float BoundaryEpsilon = 1e-5f;
};

bool ValidateVirtualGeometryConfig(const VirtualGeometryBuildConfig& config, string& error);
