#include "virtualgeometrypagecache.h"
#include "virtualgeometryserializationinternal.h"
#include "virtualgeometryserializer.h"
#include <atomic>
#include <algorithm>
#include <bit>
#include <fstream>
#include <limits>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace VirtualGeometryPageCache
{
namespace
{
using VirtualGeometrySerialization::Writer;
using VirtualGeometrySerialization::Reader;
constexpr uint32_t Magic = 0x32504756; // VGP2
constexpr uint32_t HeaderBytes = 64;
constexpr uint64_t MaxMetadataBytes = 512ull * 1024 * 1024;

template<class T>
void WriteRecords(Writer& writer, const std::vector<T>& values)
{
    for (const auto& value : values)
    {
        const auto words = std::bit_cast<std::array<uint32_t, sizeof(T) / 4>>(value);
        for (uint32_t word : words)
        {
            writer.U32(word);
        }
    }
}
template<class T>
void ReadRecords(Reader& reader, std::vector<T>& values, uint32_t count)
{
    auto source = reader.Take(size_t(count) * sizeof(T));
    Reader records(source);
    values.resize(count);
    for (auto& value : values)
    {
        std::array<uint32_t, sizeof(T) / 4> words;
        for (auto& word : words)
        {
            word = records.U32();
        }
        value = std::bit_cast<T>(words);
    }
}
void ReadExact(std::ifstream& file, std::span<uint8_t> bytes)
{
    if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
    {
        throw std::runtime_error("Truncated Page cache");
    }
}
}

bool WriteGenerated(const std::filesystem::path& path, const VirtualGeometryGPU::PackedAsset& geometry,
    std::span<const uint8_t> pinned, const PageGenerator& generate, std::string& error)
{
    static std::atomic<uint64_t> sequence{0};
    auto temporary = path;
#ifdef _WIN32
    temporary += ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(++sequence);
#else
    temporary += ".tmp." + std::to_string(++sequence);
#endif
    try
    {
        if (!VirtualGeometryPages::Validate(geometry, pinned, error))
        {
            return false;
        }
        Writer metadata;
        metadata.U32(uint32_t(geometry.Clusters.size()));
        metadata.U32(uint32_t(geometry.Groups.size()));
        metadata.U32(uint32_t(geometry.HierarchyIndices.size()));
        metadata.U32(uint32_t(geometry.Roots.size()));
        WriteRecords(metadata, geometry.Clusters);
        WriteRecords(metadata, geometry.Groups);
        WriteRecords(metadata, geometry.HierarchyIndices);
        WriteRecords(metadata, geometry.Roots);
        uint64_t offset = HeaderBytes + metadata.Bytes.size() + pinned.size() * 24ull;
        std::vector<uint8_t> payload(VirtualGeometryPages::PageBytes);
        std::vector<uint64_t> checksums;
        for (uint32_t page = 0; page < pinned.size(); ++page)
        {
            if (!generate(page, payload, error))
            {
                throw std::runtime_error(error);
            }
            metadata.U64(offset);
            metadata.U32(VirtualGeometryPages::PageBytes);
            metadata.U32(pinned[page]);
            checksums.push_back(VirtualGeometrySerializer::Hash(payload));
            metadata.U64(checksums.back());
            offset += VirtualGeometryPages::PageBytes;
        }
        if (metadata.Bytes.size() > MaxMetadataBytes)
        {
            throw std::runtime_error("Page metadata budget exceeded");
        }
        Writer header;
        header.U32(Magic);
        header.U32(2);
        header.U32(VirtualGeometryPages::PageBytes);
        header.U32(uint32_t(pinned.size()));
        header.U64(offset);
        header.U64(metadata.Bytes.size());
        header.U64(VirtualGeometrySerializer::Hash(metadata.Bytes));
        header.Bytes.resize(HeaderBytes, 0);
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.write(reinterpret_cast<const char*>(header.Bytes.data()), header.Bytes.size());
        file.write(reinterpret_cast<const char*>(metadata.Bytes.data()), metadata.Bytes.size());
        for (uint32_t page = 0; page < pinned.size(); ++page)
        {
            if (!generate(page, payload, error) || VirtualGeometrySerializer::Hash(payload) != checksums[page])
            {
                throw std::runtime_error("Page generator failed or changed between passes: " + error);
            }
            file.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        }
        file.flush();
        file.close();
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            throw std::runtime_error("Atomic Page cache replacement failed, Win32=" + std::to_string(GetLastError()));
        }
#else
        std::filesystem::rename(temporary, path);
#endif
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = exception.what();
        return false;
    }
}

bool WriteAtomic(const std::filesystem::path& path, const VirtualGeometryPages::PagedAsset& asset, std::string& error)
{
    if (asset.Payloads.size() != asset.PinnedPages.size())
    {
        error = "Page payload/directory count mismatch";
        return false;
    }
    return WriteGenerated(path, asset.Metadata, asset.PinnedPages,
        [&](uint32_t page, std::span<uint8_t> payload, std::string& message)
        {
            if (asset.Payloads[page].size() != payload.size())
            {
                message = "Invalid Page payload size";
                return false;
            }
            std::copy(asset.Payloads[page].begin(), asset.Payloads[page].end(), payload.begin());
            return true;
        }, error);
}

bool Open(const std::filesystem::path& path, PageManifest& output, std::string& error)
{
    output = {};
    try
    {
        uint64_t fileBytes = std::filesystem::file_size(path);
        std::ifstream file(path, std::ios::binary);
        std::array<uint8_t, HeaderBytes> headerBytes{};
        ReadExact(file, headerBytes);
        Reader header(headerBytes);
        if (header.U32() != Magic || header.U32() != 2 || header.U32() != VirtualGeometryPages::PageBytes)
        {
            throw std::runtime_error("Invalid VGP2 header/version");
        }
        uint32_t pageCount = header.U32();
        uint64_t declaredFileBytes = header.U64();
        uint64_t metadataBytes = header.U64();
        uint64_t identity = header.U64();
        if (!pageCount || declaredFileBytes != fileBytes || fileBytes < HeaderBytes ||
            metadataBytes < 16 || metadataBytes > MaxMetadataBytes || metadataBytes > fileBytes - HeaderBytes ||
            uint64_t(pageCount) * 24 > metadataBytes ||
            uint64_t(pageCount) * VirtualGeometryPages::PageBytes != fileBytes - HeaderBytes - metadataBytes)
        {
            throw std::runtime_error("Invalid Page cache size/directory");
        }
        std::vector<uint8_t> bytes(size_t(metadataBytes), 0);
        ReadExact(file, bytes);
        if (VirtualGeometrySerializer::Hash(bytes) != identity)
        {
            throw std::runtime_error("Page metadata checksum mismatch");
        }
        Reader metadata(bytes);
        uint32_t clusters = metadata.U32();
        uint32_t groups = metadata.U32();
        uint32_t hierarchy = metadata.U32();
        uint32_t roots = metadata.U32();
        PageManifest result;
        result.Path = path;
        result.Identity = identity;
        result.FileBytes = fileBytes;
        ReadRecords(metadata, result.Metadata.Clusters, clusters);
        ReadRecords(metadata, result.Metadata.Groups, groups);
        ReadRecords(metadata, result.Metadata.HierarchyIndices, hierarchy);
        ReadRecords(metadata, result.Metadata.Roots, roots);
        result.Pages.resize(pageCount);
        result.PinnedPages.resize(pageCount);
        uint64_t offset = HeaderBytes + metadataBytes;
        for (uint32_t page = 0; page < pageCount; ++page)
        {
            auto& entry = result.Pages[page];
            entry.Offset = metadata.U64();
            entry.Size = metadata.U32();
            uint32_t pinned = metadata.U32();
            entry.Checksum = metadata.U64();
            if (entry.Offset != offset || entry.Size != VirtualGeometryPages::PageBytes || pinned > 1)
            {
                throw std::runtime_error("Invalid Page directory entry");
            }
            result.PinnedPages[page] = uint8_t(pinned);
            offset += entry.Size;
        }
        metadata.RequireEnd();
        if (!VirtualGeometryPages::Validate(result.Metadata, result.PinnedPages, error))
        {
            return false;
        }
        output = std::move(result);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool ReadPage(const PageManifest& manifest, uint32_t page, std::span<uint8_t> bytes, std::string& error)
{
    try
    {
        if (page >= manifest.Pages.size() || bytes.size() != VirtualGeometryPages::PageBytes)
        {
            throw std::runtime_error("Invalid Page read range");
        }
        const auto& entry = manifest.Pages[page];
        if (entry.Size != bytes.size() || entry.Offset > manifest.FileBytes || entry.Size > manifest.FileBytes - entry.Offset ||
            entry.Offset > uint64_t(std::numeric_limits<std::streamoff>::max()))
        {
            throw std::runtime_error("Page file offset overflow");
        }
        std::ifstream file(manifest.Path, std::ios::binary);
        file.seekg(std::streamoff(entry.Offset));
        ReadExact(file, bytes);
        if (VirtualGeometrySerializer::Hash(bytes) != entry.Checksum)
        {
            throw std::runtime_error("Page payload checksum mismatch");
        }
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
}
