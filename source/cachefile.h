#pragma once

#include "cachecompressionpolicy.h"
#include "shadercachecodec.h"
#include <filesystem>
#include <fstream>
#include <span>
#include <windows.h>

// Bounded, checksummed Zstandard storage for opaque CPU cache payloads.
namespace CacheFile
{
    constexpr uint64_t Magic = 0x3154535A45484343ull; // CCHEZST1
    constexpr size_t HeaderBytes = 32;

    inline bool Load(const std::filesystem::path& path, std::vector<char>& bytes, size_t maximumBytes)
    {
        bytes.clear();
        try
        {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            const std::streamoff fileBytes = input.tellg();
            if (!input || fileBytes <= 0 || uint64_t(fileBytes) > maximumBytes + HeaderBytes)
            {
                return false;
            }
            input.seekg(0);
            std::array<uint64_t, 4> header{};
            if (fileBytes >= HeaderBytes)
            {
                input.read(reinterpret_cast<char*>(header.data()), HeaderBytes);
            }
            if (header[0] != Magic)
            {
                if (uint64_t(fileBytes) > maximumBytes)
                {
                    return false;
                }
                input.seekg(0);
                std::vector<char> raw(static_cast<size_t>(fileBytes));
                if (!input.read(raw.data(), raw.size()))
                {
                    return false;
                }
                bytes.swap(raw);
                return true;
            }
            const uint64_t rawBytes = header[1];
            const uint64_t storedBytes = header[2];
            if (!rawBytes || rawBytes > maximumBytes || !storedBytes || storedBytes >= rawBytes ||
                storedBytes != uint64_t(fileBytes) - HeaderBytes)
            {
                return false;
            }
            std::vector<char> payload(static_cast<size_t>(storedBytes));
            if (!input.read(payload.data(), payload.size()))
            {
                return false;
            }
            const size_t frameBytes = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
            if (ZSTD_getFrameContentSize(payload.data(), payload.size()) != rawBytes || ZSTD_isError(frameBytes) ||
                frameBytes != payload.size())
            {
                return false;
            }
            std::vector<char> raw(static_cast<size_t>(rawBytes));
            const size_t restored = ZSTD_decompress(raw.data(), raw.size(), payload.data(), payload.size());
            if (ZSTD_isError(restored) || restored != raw.size() ||
                ShaderCacheCodec::Hash(raw.data(), raw.size()) != header[3])
            {
                return false;
            }
            bytes.swap(raw);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    inline bool Store(const std::filesystem::path& path, std::span<const char> bytes)
    {
        auto temporary = path;
        temporary += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + ".tmp";
        std::error_code error;
        try
        {
            if (bytes.empty())
            {
                return false;
            }
            std::vector<char> compressed;
            bool useCompression = false;
            if (CacheCompressionPolicy::PreferSmallerFiles())
            {
                compressed.resize(ZSTD_compressBound(bytes.size()));
                const size_t stored =
                    ZSTD_compress(compressed.data(), compressed.size(), bytes.data(), bytes.size(), 3);
                useCompression = !ZSTD_isError(stored) && stored + HeaderBytes < bytes.size();
                if (useCompression)
                {
                    compressed.resize(stored);
                }
            }
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (useCompression)
            {
                const uint64_t header[] = {Magic, bytes.size(), compressed.size(),
                                           ShaderCacheCodec::Hash(bytes.data(), bytes.size())};
                output.write(reinterpret_cast<const char*>(header), sizeof(header));
                output.write(compressed.data(), compressed.size());
            }
            else
            {
                output.write(bytes.data(), bytes.size());
            }
            output.close();
            if (!output.fail() && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                return true;
            }
        }
        catch (const std::exception&)
        {
        }
        std::filesystem::remove(temporary, error);
        return false;
    }
}
