#pragma once
#include "virtualgeometrygpu.h"
#include <span>

namespace VirtualGeometryPages
{
inline constexpr uint32_t PageBytes = 64 * 1024;
struct PagedAsset
{
    VirtualGeometryGPU::PackedAsset Metadata;
    std::vector<std::vector<uint8_t>> Payloads;
    std::vector<uint8_t> PinnedPages;
};
bool Build(const VirtualGeometryAsset& asset, PagedAsset& output, std::string& error);
bool Validate(const VirtualGeometryGPU::PackedAsset& metadata, std::span<const uint8_t> pinned, std::string& error);
}
