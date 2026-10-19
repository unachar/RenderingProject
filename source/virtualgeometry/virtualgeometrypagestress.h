#pragma once
#include "virtualgeometrypagecache.h"

namespace VirtualGeometryPageStress
{
    inline constexpr uint32_t LeavesPerAsset = 32768;
    inline constexpr uint64_t SourceTrianglesPerAsset = uint64_t(LeavesPerAsset) * 50;
    bool Create(const std::filesystem::path& path, uint32_t leafCount, VirtualGeometryPageCache::PageManifest& manifest,
                std::string& error);
} // 名前空間 VirtualGeometryPageStress の終わり。
