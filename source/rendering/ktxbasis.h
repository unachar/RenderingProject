#pragma once
#include <windows.h>
#include <DirectXTex.h>
#include <cstdint>
#include <filesystem>
#include <vector>

enum class TextureUsage : uint32_t;

namespace KtxBasis
{
    constexpr size_t MaxImageBytes = 256ull * 1024 * 1024;
    bool IsKtx2(const uint8_t* bytes, size_t size);
    // Owns all returned pixels. Unsupported orientation/format/channel contracts fail explicitly.
    HRESULT Load(const uint8_t* bytes, size_t size, TextureUsage usage, DirectX::TexMetadata& metadata,
                 DirectX::ScratchImage& image);
    HRESULT LoadFile(const std::filesystem::path& path, TextureUsage usage, DirectX::TexMetadata& metadata,
                     DirectX::ScratchImage& image);
    // Native pixels/BC blocks are preserved; zstd supercompression is optional.
    HRESULT Encode(const DirectX::ScratchImage& image, bool zstd, std::vector<uint8_t>& bytes);
}
