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
    inline constexpr uint32_t BuilderVersion = 4;
    inline constexpr uint32_t GeometryPageBytes = 64 * 1024;
} // 名前空間 VirtualGeometryConfig の終わり。

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

enum class GeometryRenderingMode
{
    Auto,
    VirtualGeometry,
    Legacy
};

// インポートと実行時で共有する。VG を強制指定しても、非対応なら代替経路を使う。
inline GeometryRenderingMode& GetGeometryRenderingMode()
{
    static GeometryRenderingMode mode = GeometryRenderingMode::Auto;
    return mode;
}

namespace VGSettings
{
    inline VirtualGeometryBuildConfig Production()
    {
        VirtualGeometryBuildConfig config;
        config.MaxClusterVertices = 64;
        config.MaxClusterTriangles = 124;
        config.TargetRelativeError = 0.01f;
        return config;
    }
} // 名前空間 VGSettings の終わり。

bool ValidateVirtualGeometryConfig(const VirtualGeometryBuildConfig& config, std::string& error);
