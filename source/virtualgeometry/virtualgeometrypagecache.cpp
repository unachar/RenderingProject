#include "virtualgeometrypagecache.h"
#include "virtualgeometryserializationinternal.h"
#include "virtualgeometryserializer.h"
#include <atomic>
#include <algorithm>
#include <bit>
#include <fstream>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace VirtualGeometryPageCache
{
    namespace
    {
        using VirtualGeometrySerialization::Reader;
        using VirtualGeometrySerialization::Writer;
        constexpr uint32_t Magic = 0x32504756; // ページ形式の識別子（VGP2）
        constexpr uint32_t HeaderBytes = 64;
        constexpr uint64_t MaxMetadataBytes = 512ull * 1024 * 1024;

        void WriteValue(Writer& writer, uint32_t value)
        {
            writer.U32(value);
        }

        void ReadValue(Reader& reader, uint32_t& value)
        {
            value = reader.U32();
        }

        void WriteValue(Writer& writer, const VirtualGeometryGPUCluster& value)
        {
            writer.U32(value.VertexOffset);
            writer.U32(value.VertexCount);
            writer.U32(value.TriangleOffset);
            writer.U32(value.TriangleCount);
            writer.U32(value.Level);
            writer.U32(value.GroupIndex);
            writer.U32(value.GeneratingGroupIndex);
            writer.U32(value.MaterialId);
            for (float component : value.Bounds)
            {
                writer.F32(component);
            }
            for (float component : value.Cone)
            {
                writer.F32(component);
            }
            writer.F32(value.LodError);
            writer.U32(value.PageIndex);
            writer.U32(value.Flags);
            writer.U32(value.Padding);
            for (float component : value.ConeApex)
            {
                writer.F32(component);
            }
        }

        void ReadValue(Reader& reader, VirtualGeometryGPUCluster& value)
        {
            value.VertexOffset = reader.U32();
            value.VertexCount = reader.U32();
            value.TriangleOffset = reader.U32();
            value.TriangleCount = reader.U32();
            value.Level = reader.U32();
            value.GroupIndex = reader.U32();
            value.GeneratingGroupIndex = reader.U32();
            value.MaterialId = reader.U32();
            for (float& component : value.Bounds)
            {
                component = reader.F32();
            }
            for (float& component : value.Cone)
            {
                component = reader.F32();
            }
            value.LodError = reader.F32();
            value.PageIndex = reader.U32();
            value.Flags = reader.U32();
            value.Padding = reader.U32();
            for (float& component : value.ConeApex)
            {
                component = reader.F32();
            }
        }

        void WriteValue(Writer& writer, const VirtualGeometryGPUGroup& value)
        {
            writer.U32(value.FirstChild);
            writer.U32(value.ChildCount);
            writer.U32(value.FirstParent);
            writer.U32(value.ParentCount);
            for (float component : value.Bounds)
            {
                writer.F32(component);
            }
            writer.F32(value.MinError);
            writer.F32(value.MaxError);
            writer.U32(value.PageIndex);
            writer.U32(value.Flags);
            writer.U32(value.Level);
            for (uint32_t component : value.Padding)
            {
                writer.U32(component);
            }
        }

        void ReadValue(Reader& reader, VirtualGeometryGPUGroup& value)
        {
            value.FirstChild = reader.U32();
            value.ChildCount = reader.U32();
            value.FirstParent = reader.U32();
            value.ParentCount = reader.U32();
            for (float& component : value.Bounds)
            {
                component = reader.F32();
            }
            value.MinError = reader.F32();
            value.MaxError = reader.F32();
            value.PageIndex = reader.U32();
            value.Flags = reader.U32();
            value.Level = reader.U32();
            for (uint32_t& component : value.Padding)
            {
                component = reader.U32();
            }
        }

        template <class T> void WriteRecords(Writer& writer, const std::vector<T>& values)
        {
            for (const auto& value : values)
            {
                WriteValue(writer, value);
            }
        }

        template <class T> void ReadRecords(Reader& reader, std::vector<T>& values, uint32_t count, uint32_t wireStride)
        {
            auto source = reader.Take(size_t(count) * wireStride);
            Reader records(source);
            values.resize(count);
            for (auto& value : values)
            {
                ReadValue(records, value);
            }
            records.RequireEnd();
        }

        void WriteMetrics(Writer& writer, const VirtualGeometryCompression::Metrics& value)
        {
            writer.U64(value.EncodedBytes);
            writer.U64(value.RawBytes);
            writer.U64(value.FallbackCount);
            writer.U64(value.PositionQuantized);
            writer.U64(value.HalfUV);
            writer.U64(value.Index8Clusters);
            writer.U64(value.Index16Clusters);
        }

        void ReadMetrics(Reader& reader, VirtualGeometryCompression::Metrics& value)
        {
            value.EncodedBytes = reader.U64();
            value.RawBytes = reader.U64();
            value.FallbackCount = reader.U64();
            value.PositionQuantized = reader.U64();
            value.HalfUV = reader.U64();
            value.Index8Clusters = reader.U64();
            value.Index16Clusters = reader.U64();
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
                        std::span<const uint8_t> pinned, const PageGenerator& generate, std::string& error,
                        const VirtualGeometryCompression::Metrics& compression, uint64_t sourceTriangles,
                        uint64_t sourceVertices)
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
            bool encoded = std::any_of(geometry.Clusters.begin(), geometry.Clusters.end(), [](const auto& cluster) {
                return (cluster.Flags & VirtualGeometryCompression::ClusterFlag) != 0;
            });
            if (sourceTriangles == 0 || sourceVertices == 0)
            {
                uint64_t triangles = 0;
                uint64_t vertices = 0;
                for (const auto& cluster : geometry.Clusters)
                {
                    if (cluster.Level == 0)
                    {
                        triangles += cluster.TriangleCount;
                        vertices += cluster.VertexCount;
                    }
                }
                if (sourceTriangles == 0)
                {
                    sourceTriangles = triangles;
                }
                if (sourceVertices == 0)
                {
                    sourceVertices = vertices;
                }
            }
            bool extended = true; // 製品用コンテナーは常に指標とバージョンのメタデータを持つ。
            if (extended)
            {
                WriteMetrics(metadata, compression);
            }
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
            header.U32(uint32_t(FormatVersion::Production));
            header.U32(VirtualGeometryPages::PageBytes);
            header.U32(uint32_t(pinned.size()));
            header.U64(offset);
            header.U64(metadata.Bytes.size());
            header.U64(VirtualGeometrySerializer::Hash(metadata.Bytes));
            header.U32(VirtualGeometryConfig::BuilderVersion);
            header.U32(encoded ? VirtualGeometryCompression::CompressionVersion : 0);
            header.U64(sourceTriangles);
            header.U64(sourceVertices);
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
                throw std::runtime_error("Atomic Page cache replacement failed, Win32=" +
                                         std::to_string(GetLastError()));
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

    bool WriteAtomic(const std::filesystem::path& path, const VirtualGeometryPages::PagedAsset& asset,
                     std::string& error)
    {
        if (asset.Payloads.size() != asset.PinnedPages.size())
        {
            error = "Page payload/directory count mismatch";
            return false;
        }
        return WriteGenerated(
            path, asset.Metadata, asset.PinnedPages,
            [&](uint32_t page, std::span<uint8_t> payload, std::string& message) {
                if (asset.Payloads[page].size() != payload.size())
                {
                    message = "Invalid Page payload size";
                    return false;
                }
                std::copy(asset.Payloads[page].begin(), asset.Payloads[page].end(), payload.begin());
                return true;
            },
            error, asset.Compression, asset.SourceTriangles, asset.SourceVertices);
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
            if (header.U32() != Magic)
            {
                throw std::runtime_error("Invalid VGMesh Page magic");
            }
            uint32_t version = header.U32();
            if (version < uint32_t(FormatVersion::Streaming) || version > uint32_t(FormatVersion::Production))
            {
                throw std::runtime_error("Unsupported VGMesh format version");
            }
            if (header.U32() != VirtualGeometryPages::PageBytes)
            {
                throw std::runtime_error("Invalid VGMesh Streaming Page size");
            }
            uint32_t pageCount = header.U32();
            uint64_t declaredFileBytes = header.U64();
            uint64_t metadataBytes = header.U64();
            uint64_t identity = header.U64();
            const uint32_t builderVersion = header.U32();
            const uint32_t compressionVersion = header.U32();
            const uint64_t sourceTriangles = header.U64();
            const uint64_t sourceVertices = header.U64();
            if (version == uint32_t(FormatVersion::Production) &&
                (builderVersion != VirtualGeometryConfig::BuilderVersion ||
                 compressionVersion > VirtualGeometryCompression::CompressionVersion))
            {
                throw std::runtime_error("VGMesh builder/codec version requires asset rebuild");
            }
            if (!pageCount || declaredFileBytes != fileBytes || fileBytes < HeaderBytes || metadataBytes < 16 ||
                metadataBytes > MaxMetadataBytes || metadataBytes > fileBytes - HeaderBytes ||
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
            result.Version = FormatVersion(version);
            result.BuilderVersion = builderVersion;
            result.CompressionVersion = compressionVersion;
            result.SourceTriangles = sourceTriangles;
            result.SourceVertices = sourceVertices;
            result.Path = path;
            result.Identity = identity;
            result.FileBytes = fileBytes;
            ReadRecords(metadata, result.Metadata.Clusters, clusters, 96);
            ReadRecords(metadata, result.Metadata.Groups, groups, 64);
            ReadRecords(metadata, result.Metadata.HierarchyIndices, hierarchy, 4);
            ReadRecords(metadata, result.Metadata.Roots, roots, 4);
            if (version >= uint32_t(FormatVersion::GeometryCompression))
            {
                ReadMetrics(metadata, result.Compression);
            }
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
            result.PageClusters.resize(pageCount);
            for (uint32_t index = 0; index < result.Metadata.Clusters.size(); ++index)
            {
                const auto& cluster = result.Metadata.Clusters[index];
                if (version == uint32_t(FormatVersion::Production) &&
                    (cluster.Flags & VirtualGeometryCompression::ClusterFlag) &&
                    compressionVersion != VirtualGeometryCompression::CompressionVersion)
                {
                    throw std::runtime_error("Compressed VGMesh requires matching codec version");
                }
                if (version == uint32_t(FormatVersion::Streaming) &&
                    (cluster.Flags & VirtualGeometryCompression::ClusterFlag))
                {
                    throw std::runtime_error("Compressed cluster in legacy Page format");
                }
                result.PageClusters[cluster.PageIndex].push_back(index);
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
            if (entry.Size != bytes.size() || entry.Offset > manifest.FileBytes ||
                entry.Size > manifest.FileBytes - entry.Offset ||
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
            if (manifest.PageClusters.size() != manifest.Pages.size())
            {
                throw std::runtime_error("Missing validated Page cluster directory");
            }
            for (uint32_t index : manifest.PageClusters[page])
            {
                const auto& cluster = manifest.Metadata.Clusters.at(index);
                if ((cluster.Flags & VirtualGeometryCompression::ClusterFlag) != 0)
                {
                    if (cluster.VertexOffset > bytes.size() || cluster.Padding > bytes.size() - cluster.VertexOffset ||
                        !VirtualGeometryCompression::Validate(bytes.subspan(cluster.VertexOffset, cluster.Padding),
                                                              cluster.VertexCount, cluster.TriangleCount, error))
                    {
                        throw std::runtime_error(manifest.Path.string() + " cluster " + std::to_string(index) + ": " +
                                                 error);
                    }
                }
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
} // 名前空間 VirtualGeometryPageCache の終わり。
