#pragma once
#include "virtualgeometryasset.h"
#include "virtualgeometrycompression.h"
#include <filesystem>
#include <span>

namespace VirtualGeometryPageCache
{
    struct PageManifest;
}

struct VirtualGeometryLoadLimits
{
    uint64_t MaxFileBytes = 1024ull * 1024 * 1024;
    uint64_t MaxDecodedBytes = 1024ull * 1024 * 1024;
};

class VirtualGeometrySerializer
{
  public:
    // GPU 固有の形式。読み込みではメタデータとディレクトリだけを返し、頂点は展開しない。
    static bool SaveEncoded(const std::filesystem::path& path, const VirtualGeometryAsset& asset, std::string& error,
                            const VGCompressionSettings& settings = {},
                            const VirtualGeometryCompression::Input& attributes = {});
    static bool Encode(const VirtualGeometryAsset& asset, std::vector<uint8_t>& bytes, std::string& error);
    static bool Decode(std::span<const uint8_t> bytes, VirtualGeometryAsset& asset, std::string& error,
                       const VirtualGeometryLoadLimits& limits = {});
    static bool Save(const std::filesystem::path& path, const VirtualGeometryAsset& asset, std::string& error);
    static bool Load(const std::filesystem::path& path, VirtualGeometryAsset& asset, std::string& error,
                     const VirtualGeometryLoadLimits& limits = {});
    // FNV-1a 64: 決定的な整合性・内容の識別値。暗号学的な認証ではない。
    static uint64_t Hash(std::span<const uint8_t> bytes);
};
