#include "virtualgeometryserializer.h"
#include "virtualgeometrypagecache.h"
#include "virtualgeometryserializationinternal.h"
#include "virtualgeometryinternal.h"
#include "virtualgeometryvalidator.h"
#include <array>
#include <fstream>
#include <limits>
#include <atomic>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

bool VirtualGeometrySerializer::SaveEncoded(const std::filesystem::path& path, const VirtualGeometryAsset& asset,
    std::string& error, const VGCompressionSettings& settings, const VirtualGeometryCompression::Input& attributes)
{
    VirtualGeometryPages::PagedAsset pages;
    return VirtualGeometryPages::Build(asset, attributes, settings, pages, error) && VirtualGeometryPageCache::WriteAtomic(path, pages, error);
}

bool VirtualGeometrySerializer::LoadEncoded(const std::filesystem::path& path,
    VirtualGeometryPageCache::PageManifest& manifest, std::string& error)
{
    return VirtualGeometryPageCache::Open(path, manifest, error);
}

namespace
{
    using namespace VirtualGeometrySerialization;
    using namespace VirtualGeometryInternal;

    void WriteCluster(Writer& writer, const VirtualGeometryCluster& cluster)
    {
        writer.U32(cluster.VertexOffset);
        writer.U32(cluster.VertexCount);
        writer.U32(cluster.TriangleOffset);
        writer.U32(cluster.TriangleCount);
        writer.U32(cluster.Level);
        writer.U32(cluster.GroupIndex);
        writer.U32(cluster.GeneratingGroupIndex);
        writer.U32(cluster.MaterialId);
        writer.Sphere(cluster.Bounds);
        writer.Float3(cluster.Aabb.Min);
        writer.Float3(cluster.Aabb.Max);
        writer.Float3(cluster.Cone.Axis);
        writer.Float3(cluster.Cone.Apex);
        writer.F32(cluster.Cone.Cutoff);
        writer.F32(cluster.LodError);
    }

    VirtualGeometryCluster ReadCluster(Reader& reader)
    {
        VirtualGeometryCluster cluster;
        cluster.VertexOffset = reader.U32();
        cluster.VertexCount = reader.U32();
        cluster.TriangleOffset = reader.U32();
        cluster.TriangleCount = reader.U32();
        cluster.Level = reader.U32();
        cluster.GroupIndex = reader.U32();
        cluster.GeneratingGroupIndex = reader.U32();
        cluster.MaterialId = reader.U32();
        cluster.Bounds = reader.Sphere();
        cluster.Aabb.Min = reader.Float3();
        cluster.Aabb.Max = reader.Float3();
        cluster.Cone.Axis = reader.Float3();
        cluster.Cone.Apex = reader.Float3();
        cluster.Cone.Cutoff = reader.F32();
        cluster.LodError = reader.F32();
        return cluster;
    }

    struct Directory
    {
        std::array<uint64_t, SectionCount> Offsets{};
        std::array<uint64_t, SectionCount> Counts{};
    };

    Directory ReadDirectory(std::span<const uint8_t> bytes, const VirtualGeometryLoadLimits& limits)
    {
        if (bytes.size() < HeaderBytes || bytes.size() > limits.MaxFileBytes)
        {
            throw std::runtime_error("Invalid file size or file budget exceeded");
        }
        Reader header(bytes.first(HeaderBytes));
        if (header.U32() != Magic)
        {
            throw std::runtime_error("Invalid VGM1 magic");
        }
        if (header.U32() != VirtualGeometryConfig::AssetVersion)
        {
            throw std::runtime_error("Unsupported VGMesh format version");
        }
        if (header.U64() != bytes.size())
        {
            throw std::runtime_error("Virtual geometry file size mismatch");
        }
        uint64_t checksum = header.U64();
        if (header.U32() != SectionCount || header.U32() != HeaderBytes)
        {
            throw std::runtime_error("Invalid section directory");
        }
        Directory directory;
        uint64_t end = HeaderBytes;
        uint64_t decodedBytes = 0;
        const uint64_t memoryStrides[] = {sizeof(VirtualGeometryVertex), 1, sizeof(VirtualGeometryCluster),
            sizeof(VirtualGeometryClusterGroup), 4, 4, sizeof(VirtualGeometryLevelStats), 1};
        for (uint32_t section = 0; section < SectionCount; ++section)
        {
            uint64_t offset = header.U64();
            uint64_t count = header.U64();
            if (offset != end || offset > bytes.size() || count >= VirtualGeometryConfig::InvalidIndex ||
                count > (bytes.size() - offset) / Strides[section])
            {
                throw std::runtime_error("Invalid section offset/count/range");
            }
            if (decodedBytes > limits.MaxDecodedBytes || count > (limits.MaxDecodedBytes - decodedBytes) / memoryStrides[section])
            {
                throw std::runtime_error("Virtual geometry decoded allocation budget exceeded");
            }
            decodedBytes += count * memoryStrides[section];
            directory.Offsets[section] = offset;
            directory.Counts[section] = count;
            end += count * Strides[section];
        }
        header.RequireEnd();
        if (end != bytes.size() || directory.Counts[Levels] > VirtualGeometryConfig::MaxHierarchyLevels ||
            directory.Counts[Metadata] < 152 || directory.Counts[Clusters] == 0 || directory.Counts[Roots] == 0)
        {
            throw std::runtime_error("Invalid payload layout/counts");
        }
        // Cover offsets/counts too; hash excludes only the fixed 32-byte header.
        if (checksum != VirtualGeometrySerializer::Hash(bytes.subspan(32)))
        {
            throw std::runtime_error("Virtual geometry checksum mismatch");
        }
        return directory;
    }

    void DecodeSections(std::span<const uint8_t> bytes, const Directory& directory, VirtualGeometryAsset& asset)
    {
        auto sectionReader = [&](Section section)
        {
            return Reader(bytes.subspan(static_cast<size_t>(directory.Offsets[section]),
                static_cast<size_t>(directory.Counts[section] * Strides[section])));
        };
        auto metadata = sectionReader(Metadata);
        ReadMetadata(metadata, asset);
        std::string error;
        if (!ValidateVirtualGeometryConfig(asset.Config, error))
        {
            throw std::runtime_error(error);
        }
        auto vertices = sectionReader(Vertices);
        asset.Vertices.resize(static_cast<size_t>(directory.Counts[Vertices]));
        for (auto& vertex : asset.Vertices)
        {
            for (float& value : vertex.Position)
            {
                value = vertices.F32();
            }
            for (float& value : vertex.Normal)
            {
                value = vertices.F32();
            }
            for (float& value : vertex.TexCoord)
            {
                value = vertices.F32();
            }
            for (float& value : vertex.Diffuse)
            {
                value = vertices.F32();
            }
        }
        vertices.RequireEnd();
        auto triangles = bytes.subspan(static_cast<size_t>(directory.Offsets[Triangles]),
            static_cast<size_t>(directory.Counts[Triangles]));
        asset.TriangleIndices.assign(triangles.begin(), triangles.end());
        auto clusters = sectionReader(Clusters);
        asset.Clusters.reserve(static_cast<size_t>(directory.Counts[Clusters]));
        for (uint64_t index = 0; index < directory.Counts[Clusters]; ++index)
        {
            asset.Clusters.push_back(ReadCluster(clusters));
        }
        clusters.RequireEnd();
        auto groups = sectionReader(Groups);
        asset.Groups.resize(static_cast<size_t>(directory.Counts[Groups]));
        for (auto& group : asset.Groups)
        {
            group.Level = groups.U32();
            group.FirstChild = groups.U32();
            group.ChildCount = groups.U32();
            group.FirstParent = groups.U32();
            group.ParentCount = groups.U32();
            group.Bounds = groups.Sphere();
            group.MinError = groups.F32();
            group.MaxError = groups.F32();
        }
        groups.RequireEnd();
        auto readIndices = [&](Section section, std::vector<uint32_t>& destination)
        {
            auto reader = sectionReader(section);
            destination.resize(static_cast<size_t>(directory.Counts[section]));
            for (auto& value : destination)
            {
                value = reader.U32();
            }
            reader.RequireEnd();
        };
        readIndices(Hierarchy, asset.HierarchyIndices);
        readIndices(Roots, asset.RootClusters);
        auto levels = sectionReader(Levels);
        asset.Levels.resize(static_cast<size_t>(directory.Counts[Levels]));
        for (auto& level : asset.Levels)
        {
            level.Clusters = levels.U32();
            level.Triangles = levels.U64();
            level.Vertices = levels.U64();
        }
        levels.RequireEnd();
    }
}

uint64_t VirtualGeometrySerializer::Hash(std::span<const uint8_t> bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : bytes)
    {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    return hash;
}

bool VirtualGeometrySerializer::Encode(const VirtualGeometryAsset& asset, std::vector<uint8_t>& bytes, std::string& error)
{
    error.clear();
    bytes.clear();
    if (!VirtualGeometryValidator::Validate(asset, error))
    {
        return false;
    }
    try
    {
        Writer writer;
        writer.Bytes.resize(HeaderBytes);
        Directory directory;
        directory.Counts = {asset.Vertices.size(), asset.TriangleIndices.size(), asset.Clusters.size(),
            asset.Groups.size(), asset.HierarchyIndices.size(), asset.RootClusters.size(), asset.Levels.size(), 0};
        directory.Offsets[Vertices] = writer.Bytes.size();
        for (const auto& vertex : asset.Vertices)
        {
            for (float value : VertexComponents(vertex))
            {
                writer.F32(value);
            }
        }
        directory.Offsets[Triangles] = writer.Bytes.size();
        writer.Bytes.insert(writer.Bytes.end(), asset.TriangleIndices.begin(), asset.TriangleIndices.end());
        directory.Offsets[Clusters] = writer.Bytes.size();
        for (const auto& cluster : asset.Clusters)
        {
            WriteCluster(writer, cluster);
        }
        directory.Offsets[Groups] = writer.Bytes.size();
        for (const auto& group : asset.Groups)
        {
            writer.U32(group.Level);
            writer.U32(group.FirstChild);
            writer.U32(group.ChildCount);
            writer.U32(group.FirstParent);
            writer.U32(group.ParentCount);
            writer.Sphere(group.Bounds);
            writer.F32(group.MinError);
            writer.F32(group.MaxError);
        }
        directory.Offsets[Hierarchy] = writer.Bytes.size();
        for (uint32_t index : asset.HierarchyIndices)
        {
            writer.U32(index);
        }
        directory.Offsets[Roots] = writer.Bytes.size();
        for (uint32_t index : asset.RootClusters)
        {
            writer.U32(index);
        }
        directory.Offsets[Levels] = writer.Bytes.size();
        for (const auto& level : asset.Levels)
        {
            writer.U32(level.Clusters);
            writer.U64(level.Triangles);
            writer.U64(level.Vertices);
        }
        directory.Offsets[Metadata] = writer.Bytes.size();
        WriteMetadata(writer, asset);
        directory.Counts[Metadata] = writer.Bytes.size() - directory.Offsets[Metadata];
        Writer header;
        header.U32(Magic);
        header.U32(asset.Version);
        header.U64(writer.Bytes.size());
        header.U64(0);
        header.U32(SectionCount);
        header.U32(HeaderBytes);
        for (uint32_t section = 0; section < SectionCount; ++section)
        {
            header.U64(directory.Offsets[section]);
            header.U64(directory.Counts[section]);
        }
        std::copy(header.Bytes.begin(), header.Bytes.end(), writer.Bytes.begin());
        uint64_t checksum = Hash(std::span<const uint8_t>(writer.Bytes).subspan(32));
        for (uint32_t byte = 0; byte < 8; ++byte)
        {
            writer.Bytes[16 + byte] = static_cast<uint8_t>(checksum >> (byte * 8));
        }
        bytes = std::move(writer.Bytes);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool VirtualGeometrySerializer::Decode(std::span<const uint8_t> bytes, VirtualGeometryAsset& asset,
    std::string& error, const VirtualGeometryLoadLimits& limits)
{
    error.clear();
    try
    {
        Directory directory = ReadDirectory(bytes, limits);
        VirtualGeometryAsset decoded;
        DecodeSections(bytes, directory, decoded);
        if (!VirtualGeometryValidator::Validate(decoded, error))
        {
            return false;
        }
        asset = std::move(decoded); // Transactional: retain caller's old asset on failure.
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool VirtualGeometrySerializer::Save(const std::filesystem::path& path, const VirtualGeometryAsset& asset, std::string& error)
{
    static std::atomic<uint64_t> sequence{0};
    auto temporary = path;
#ifdef _WIN32
    temporary += ".tmp."+std::to_string(GetCurrentProcessId())+"."+std::to_string(++sequence);
#else
    temporary += ".tmp."+std::to_string(++sequence);
#endif
    std::vector<uint8_t> bytes;
    if (!Encode(asset, bytes, error))
    {
        return false;
    }
    try
    {
        if (bytes.size() > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        {
            throw std::runtime_error("Virtual geometry file exceeds stream addressing");
        }
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            throw std::runtime_error("Cannot publish VirtualGeometry derived data");
        }
#else
        std::filesystem::rename(temporary,path);
#endif
        return true;
    }
    catch (const std::exception& exception)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary,ignored);
        error = exception.what();
        return false;
    }
}

bool VirtualGeometrySerializer::Load(const std::filesystem::path& path, VirtualGeometryAsset& asset,
    std::string& error, const VirtualGeometryLoadLimits& limits)
{
    error.clear();
    try
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        auto position = file.tellg();
        if (position < 0)
        {
            throw std::runtime_error("Cannot determine virtual geometry file size");
        }
        uint64_t size = static_cast<uint64_t>(position);
        if (size < HeaderBytes || size > limits.MaxFileBytes || size > std::numeric_limits<size_t>::max() ||
            size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        {
            throw std::runtime_error("Invalid virtual geometry file size or load budget exceeded");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        return Decode(bytes, asset, error, limits);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}
