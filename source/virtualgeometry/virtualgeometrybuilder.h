#pragma once
#include "virtualgeometryasset.h"
#include <span>

struct StaticMeshData;

// インポート時の適格性だけを判定する。実行時にはマテリアル・変形・デバイス対応も確認する。
bool CanUseVirtualGeometry(const StaticMeshData& source, std::string& reason);

class VirtualGeometryBuilder
{
  public:
    static VirtualGeometryBuildResult Build(std::span<const VirtualGeometryVertex> vertices,
                                            std::span<const uint32_t> indices,
                                            const VirtualGeometryBuildConfig& config = {}, uint32_t materialId = 0);

    // LOD0 のみ。呼び出し元は元リソースのセクション番号を materialId として渡す。
    static VirtualGeometryBuildResult Build(const StaticMeshData& source, const VirtualGeometryBuildConfig& config,
                                            uint32_t materialId);
};
