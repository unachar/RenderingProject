#pragma once
#include "virtualgeometrygpu.h"
#include "virtualgeometrycompression.h"
#include <span>

namespace VirtualGeometryPages
{
    inline constexpr uint32_t PageBytes = VirtualGeometryConfig::GeometryPageBytes;

    struct PagedAsset
    {
        VirtualGeometryGPU::PackedAsset Metadata;
        std::vector<std::vector<uint8_t>> Payloads;
        std::vector<uint8_t> PinnedPages;
        VirtualGeometryCompression::Metrics Compression;
        std::vector<std::string> CompressionFallbacks;
        uint64_t SourceTriangles = 0;
        uint64_t SourceVertices = 0;
    };

    bool Build(const VirtualGeometryAsset& asset, PagedAsset& output, std::string& error);
    bool Build(const VirtualGeometryAsset& asset, const VGCompressionSettings& settings, PagedAsset& output,
               std::string& error);
    // 任意のストリームは、親頂点も含む最終階層の平坦化した頂点順を使う。
    bool Build(const VirtualGeometryAsset& asset, const VirtualGeometryCompression::Input& attributes,
               const VGCompressionSettings& settings, PagedAsset& output, std::string& error);
    bool Validate(const VirtualGeometryGPU::PackedAsset& metadata, std::span<const uint8_t> pinned, std::string& error);
} // 名前空間 VirtualGeometryPages の終わり。
