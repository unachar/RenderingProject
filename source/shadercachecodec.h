#pragma once

#include "../External/zstd/zstd.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace ShaderCacheCodec
{
    constexpr uint64_t MagicV1 = 0x3143584443484143ull;
    constexpr uint64_t MagicV2 = 0x3243584443484143ull;
    constexpr uint64_t MaxObjectBytes = 64ull * 1024 * 1024;
    constexpr uint64_t MaxDiagnosticBytes = 1024 * 1024;

    inline uint64_t Hash(const void* data, size_t size, uint64_t hash = 14695981039346656037ull)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    struct Header
    {
        uint64_t objectBytes = 0;
        uint64_t diagnosticBytes = 0;
        uint64_t storedBytes = 0;
        uint64_t checksum = 0;
        uint64_t headerBytes = 0;
        bool compressed = false;
    };

    inline bool ValidSizes(uint64_t objectBytes, uint64_t diagnosticBytes)
    {
        return objectBytes > 0 && objectBytes <= MaxObjectBytes && diagnosticBytes <= MaxDiagnosticBytes;
    }

    // Lengths are bounded before additions or allocations, including for hostile headers.
    inline bool ParseHeader(const std::array<uint64_t, 6>& words, uint64_t fileBytes, Header& result)
    {
        result = {};
        Header header;
        if (words[0] == MagicV1)
        {
            header = {words[1], words[2], 0, words[3], 32, false};
        }
        else if (words[0] == MagicV2 && words[1] == 1)
        {
            header = {words[2], words[3], words[4], words[5], 48, true};
        }
        else return false;
        if (!ValidSizes(header.objectBytes, header.diagnosticBytes)) return false;
        const uint64_t rawBytes = header.objectBytes + header.diagnosticBytes;
        if (!header.compressed) header.storedBytes = rawBytes;
        if (header.storedBytes == 0 || header.storedBytes > rawBytes ||
            fileBytes != header.headerBytes + header.storedBytes) return false;
        result = header;
        return true;
    }

    inline bool TryCompress(const void* object, size_t size, const std::string& diagnostics,
                            std::vector<char>& compressed)
    {
        compressed.clear();
        if (!object || !ValidSizes(size, diagnostics.size())) return false;
        try
        {
            std::vector<char> raw(size + diagnostics.size());
            std::memcpy(raw.data(), object, size);
            std::memcpy(raw.data() + size, diagnostics.data(), diagnostics.size());
            compressed.resize(ZSTD_compressBound(raw.size()));
            const size_t written = ZSTD_compress(compressed.data(), compressed.size(),
                                                 raw.data(), raw.size(), 3);
            // The extra 16 bytes of the v2 header count against the saving.
            if (ZSTD_isError(written) || written + 48 >= raw.size() + 32)
            {
                compressed.clear();
                return false;
            }
            compressed.resize(written);
            return true;
        }
        catch (const std::bad_alloc&)
        {
            compressed.clear();
            return false;
        }
    }

    inline bool DecodePayload(const Header& header, const std::vector<char>& payload,
                              std::vector<char>& object, std::string& diagnostics)
    {
        object.clear();
        diagnostics.clear();
        if (!ValidSizes(header.objectBytes, header.diagnosticBytes) ||
            payload.size() != header.storedBytes) return false;
        const size_t rawBytes = static_cast<size_t>(header.objectBytes + header.diagnosticBytes);
        try
        {
            std::vector<char> decoded;
            const char* raw = payload.data();
            if (header.compressed)
            {
                const auto contentBytes = ZSTD_getFrameContentSize(payload.data(), payload.size());
                const size_t frameBytes = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
                // Reject unknown sizes, skippable frames, multiple frames and trailing bytes.
                if (contentBytes != rawBytes || ZSTD_isError(frameBytes) || frameBytes != payload.size())
                    return false;
                decoded.resize(rawBytes);
                const size_t written = ZSTD_decompress(decoded.data(), decoded.size(), payload.data(), payload.size());
                if (ZSTD_isError(written) || written != rawBytes) return false;
                raw = decoded.data();
            }
            else if (payload.size() != rawBytes) return false;
            if (Hash(raw, rawBytes) != header.checksum) return false;
            std::vector<char> candidate(raw, raw + static_cast<size_t>(header.objectBytes));
            std::string candidateDiagnostics(raw + static_cast<size_t>(header.objectBytes),
                                             static_cast<size_t>(header.diagnosticBytes));
            object.swap(candidate);
            diagnostics.swap(candidateDiagnostics);
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }
}
