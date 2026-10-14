#pragma once
#include "virtualgeometryasset.h"
#include <filesystem>
#include <span>

struct VirtualGeometryLoadLimits
{
    uint64_t MaxFileBytes = 1024ull * 1024 * 1024;
    uint64_t MaxDecodedBytes = 1024ull * 1024 * 1024;
};

class VirtualGeometrySerializer
{
public:
    static bool Encode(const VirtualGeometryAsset& asset, vector<uint8_t>& bytes, string& error);
    static bool Decode(span<const uint8_t> bytes, VirtualGeometryAsset& asset, string& error,
        const VirtualGeometryLoadLimits& limits = {});
    static bool Save(const filesystem::path& path, const VirtualGeometryAsset& asset, string& error);
    static bool Load(const filesystem::path& path, VirtualGeometryAsset& asset, string& error,
        const VirtualGeometryLoadLimits& limits = {});
    // FNV-1a 64: deterministic integrity/content fingerprint, not cryptographic authentication.
    static uint64_t Hash(span<const uint8_t> bytes);
};
