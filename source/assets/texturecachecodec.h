#pragma once
#include "../../External/zstd/zstd.h"
#include <array>
#include <cstdint>
#include <new>
#include <vector>

namespace TextureCacheCodec
{
    constexpr uint64_t Magic = 0x003154535A534444ull; // DDSZST1 + NUL, little-endian.
    constexpr uint64_t MinDdsBytes = 136;
    constexpr uint64_t MaxDdsBytes = 256ull * 1024 * 1024;
    constexpr uint64_t HeaderBytes = 32;
    struct Header { uint64_t rawBytes = 0, storedBytes = 0, checksum = 0; };
    enum class DecodeResult { Success, Invalid, AllocationFailure };

    inline uint64_t Hash(const void* data, size_t size)
    {
        uint64_t hash = 14695981039346656037ull;
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) { hash ^= bytes[index]; hash *= 1099511628211ull; }
        return hash;
    }
    inline bool ValidSizes(const Header& header)
    {
        return header.rawBytes >= MinDdsBytes && header.rawBytes <= MaxDdsBytes &&
               header.storedBytes > 0 && header.storedBytes <= header.rawBytes;
    }
    inline bool ParseHeader(const std::array<uint64_t, 4>& words, uint64_t fileBytes, Header& header)
    {
        header = {};
        const Header candidate{words[1], words[2], words[3]};
        if (words[0] != Magic || !ValidSizes(candidate) || fileBytes != HeaderBytes + candidate.storedBytes) return false;
        header = candidate;
        return true;
    }
    inline bool TryEncode(const uint8_t* dds, size_t size, std::vector<uint8_t>& payload, Header& header)
    {
        payload.clear();
        header = {};
        if (!dds || size < MinDdsBytes || size > MaxDdsBytes) return false;
        try
        {
            payload.resize(ZSTD_compressBound(size));
            const size_t stored = ZSTD_compress(payload.data(), payload.size(), dds, size, 3);
            if (ZSTD_isError(stored) || stored + HeaderBytes >= size) { payload.clear(); return false; }
            payload.resize(stored);
            header = {size, stored, Hash(dds, size)};
            return true;
        }
        catch (const std::bad_alloc&) { payload.clear(); return false; }
    }
    inline DecodeResult Decode(const Header& header, const std::vector<uint8_t>& payload, std::vector<uint8_t>& dds)
    {
        dds.clear();
        if (!ValidSizes(header) || payload.size() != header.storedBytes) return DecodeResult::Invalid;
        const auto contentBytes = ZSTD_getFrameContentSize(payload.data(), payload.size());
        const size_t frameBytes = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
        if (contentBytes != header.rawBytes || ZSTD_isError(frameBytes) || frameBytes != payload.size())
            return DecodeResult::Invalid;
        try
        {
            std::vector<uint8_t> candidate(static_cast<size_t>(header.rawBytes));
            const size_t restored = ZSTD_decompress(candidate.data(), candidate.size(), payload.data(), payload.size());
            if (ZSTD_isError(restored) || restored != candidate.size() || Hash(candidate.data(), candidate.size()) != header.checksum)
                return DecodeResult::Invalid;
            dds.swap(candidate);
            return DecodeResult::Success;
        }
        catch (const std::bad_alloc&) { return DecodeResult::AllocationFailure; }
    }
}
