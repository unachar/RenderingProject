#pragma once
#include "virtualgeometrypages.h"
#include <filesystem>
#include <functional>

namespace VirtualGeometryPageCache
{
enum class FormatVersion : uint32_t { Streaming = 2, GeometryCompression = 3, Production = 4 };
struct PageEntry
{
    uint64_t Offset = 0;
    uint32_t Size = VirtualGeometryPages::PageBytes;
    uint64_t Checksum = 0;
};
struct PageManifest
{
    std::filesystem::path Path;
    uint64_t Identity = 0;
    uint64_t FileBytes = 0;
    VirtualGeometryGPU::PackedAsset Metadata;
    std::vector<uint8_t> PinnedPages;
    std::vector<PageEntry> Pages;
    std::vector<std::vector<uint32_t>> PageClusters;
    FormatVersion Version = FormatVersion::Streaming;
    uint32_t BuilderVersion = 0, CompressionVersion = 0;
    uint32_t StreamingPriority = 1; // Runtime import policy, not part of the encoded page layout.
    uint64_t SourceTriangles = 0, SourceVertices = 0;
    VirtualGeometryCompression::Metrics Compression;
};
bool WriteAtomic(const std::filesystem::path& path, const VirtualGeometryPages::PagedAsset& asset, std::string& error);
using PageGenerator = std::function<bool(uint32_t, std::span<uint8_t>, std::string&)>;
bool WriteGenerated(const std::filesystem::path& path, const VirtualGeometryGPU::PackedAsset& metadata,
    std::span<const uint8_t> pinned, const PageGenerator& generate, std::string& error,
    const VirtualGeometryCompression::Metrics& compression = {}, uint64_t sourceTriangles = 0, uint64_t sourceVertices = 0);
bool Open(const std::filesystem::path& path, PageManifest& output, std::string& error);
bool ReadPage(const PageManifest& manifest, uint32_t page, std::span<uint8_t> bytes, std::string& error);
}
