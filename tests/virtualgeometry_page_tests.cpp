#include "virtualgeometry/virtualgeometrypages.h"
#include "virtualgeometry/virtualgeometrypagecache.h"
#include "virtualgeometry/virtualgeometrybuilder.h"
#include "virtualgeometry/virtualgeometrypagestress.h"
#include "virtualgeometry/virtualgeometryserializer.h"
#include <cstring>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>

void TestVirtualGeometryPages()
{
    auto require = [](bool condition, const std::string& message) {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    };
    std::vector<VirtualGeometryVertex> vertices(101 * 101);
    std::vector<uint32_t> indices;
    for (uint32_t y = 0; y <= 100; ++y)
    {
        for (uint32_t x = 0; x <= 100; ++x)
        {
            auto& vertex = vertices[y * 101 + x];
            vertex.Position[0] = float(x);
            vertex.Position[1] = float(y);
            vertex.Normal[2] = 1;
            vertex.Diffuse[3] = 1;
            if (x < 100 && y < 100)
            {
                uint32_t first = y * 101 + x;
                indices.insert(indices.end(), {first, first + 1, first + 101, first + 1, first + 102, first + 101});
            }
        }
    }
    VirtualGeometryBuildConfig config;
    config.MaxClusterVertices = 64;
    config.MaxClusterTriangles = 124;
    auto built = VirtualGeometryBuilder::Build(vertices, indices, config);
    require(bool(built), "Page test fixture failed");
    VirtualGeometryPages::PagedAsset packed;
    VirtualGeometryPages::PagedAsset repeated;
    std::string error;
    require(VirtualGeometryPages::Build(built.Asset, packed, error), error);
    require(VirtualGeometryPages::Build(built.Asset, repeated, error), error);
    require(packed.Payloads == repeated.Payloads, "Page packing is not deterministic");
    require(packed.Payloads.size() > 2, "Page boundary fixture too small");
    require(packed.Metadata.Vertices.empty() && packed.Metadata.Triangles.empty(), "Metadata retains payload");
    for (size_t index = 0; index < built.Asset.Clusters.size(); ++index)
    {
        const auto& source = built.Asset.Clusters[index];
        const auto& cluster = packed.Metadata.Clusters[index];
        const auto& page = packed.Payloads.at(cluster.PageIndex);
        auto encoded = std::span<const uint8_t>(page).subspan(cluster.VertexOffset, cluster.Padding);
        require(VirtualGeometryCompression::Validate(encoded, cluster.VertexCount, cluster.TriangleCount, error),
                error);
        for (uint32_t vertex = 0; vertex < cluster.VertexCount; ++vertex)
        {
            auto decoded = VirtualGeometryCompression::DecodeVertexCPU(encoded, vertex);
            double squared = 0;
            double sphereSquared = 0;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                double delta =
                    double(decoded.Position[axis]) - built.Asset.Vertices[source.VertexOffset + vertex].Position[axis];
                squared += delta * delta;
                double sphereDelta = double(decoded.Position[axis]) - cluster.Bounds[axis];
                sphereSquared += sphereDelta * sphereDelta;
            }
            require(std::sqrt(squared) <= VGCompressionSettings{}.MaxPositionError, "Page position error");
            require(std::sqrt(sphereSquared) <= cluster.Bounds[3], "Decoded vertex outside conservative sphere");
        }
        for (uint32_t triangle = 0; triangle < cluster.TriangleCount; ++triangle)
        {
            auto decoded = VirtualGeometryCompression::LoadTriangleCPU(encoded, triangle);
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                require(decoded[corner] == built.Asset.TriangleIndices[source.TriangleOffset + triangle * 3 + corner],
                        "Page index roundtrip");
            }
        }
    }
    for (uint32_t root : built.Asset.RootClusters)
    {
        require(packed.PinnedPages.at(packed.Metadata.Clusters[root].PageIndex) != 0, "Root is not pinned");
    }
    const auto path = std::filesystem::current_path() / "vg_page_roundtrip.vgp2";
    require(VirtualGeometryPageCache::WriteAtomic(path, packed, error), error);
    VirtualGeometryPageCache::PageManifest manifest;
    require(VirtualGeometryPageCache::Open(path, manifest, error), error);
    require(manifest.Version == VirtualGeometryPageCache::FormatVersion::Production, "Production format version");
    {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> first((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        require(VirtualGeometryPageCache::WriteAtomic(path, repeated, error), error);
        std::ifstream secondFile(path, std::ios::binary);
        std::vector<uint8_t> second((std::istreambuf_iterator<char>(secondFile)), std::istreambuf_iterator<char>());
        require(first == second, "Encoded vgmesh output is not deterministic");
    }
    require(manifest.Pages.size() == packed.Payloads.size(), "Cache page count");
    std::vector<uint8_t> payload(VirtualGeometryPages::PageBytes);
    for (uint32_t page = 0; page < manifest.Pages.size(); ++page)
    {
        require(VirtualGeometryPageCache::ReadPage(manifest, page, payload, error), error);
        require(payload == packed.Payloads[page], "Disk page roundtrip");
    }
    require(!VirtualGeometryPageCache::ReadPage(manifest, UINT32_MAX, payload, error), "Invalid page accepted");
    require(!VirtualGeometryPageCache::WriteGenerated(
                path, packed.Metadata, packed.PinnedPages,
                [](uint32_t, std::span<uint8_t>, std::string& message) {
                    message = "Injected generator failure";
                    return false;
                },
                error),
            "Failed cache generation accepted");
    require(VirtualGeometryPageCache::Open(path, manifest, error), "Failed generation replaced a valid cache");
    {
        std::ifstream input(path, std::ios::binary);
        std::vector<uint8_t> damaged((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        input.close();
        uint64_t directory = manifest.Pages[0].Offset - manifest.Pages.size() * 24;
        for (size_t byte = 0; byte < 8; ++byte)
        {
            damaged[size_t(directory) + byte] = 255;
        }
        uint64_t checksum = VirtualGeometrySerializer::Hash(
            std::span<const uint8_t>(damaged).subspan(64, size_t(manifest.Pages[0].Offset - 64)));
        for (size_t byte = 0; byte < 8; ++byte)
        {
            damaged[32 + byte] = uint8_t(checksum >> (byte * 8));
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(damaged.data()), damaged.size());
    }
    require(!VirtualGeometryPageCache::Open(path, manifest, error), "64-bit Page directory overflow accepted");
    require(VirtualGeometryPageCache::WriteAtomic(path, packed, error), error);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(4);
        const char unknownVersion[4] = {99, 0, 0, 0};
        file.write(unknownVersion, 4);
    }
    require(!VirtualGeometryPageCache::Open(path, manifest, error) && error == "Unsupported VGMesh format version",
            "Unknown format version not explicitly rejected");
    require(VirtualGeometryPageCache::WriteAtomic(path, packed, error), error);
    require(VirtualGeometryPageCache::Open(path, manifest, error), error);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(std::streamoff(manifest.Pages[0].Offset));
        uint8_t corrupt = uint8_t(packed.Payloads[0][0] ^ 1);
        file.write(reinterpret_cast<const char*>(&corrupt), 1);
    }
    require(!VirtualGeometryPageCache::ReadPage(manifest, 0, payload, error), "Checksum corruption accepted");
    require(VirtualGeometryPageCache::WriteAtomic(path, packed, error), error);
    std::filesystem::resize_file(path, 12);
    require(!VirtualGeometryPageCache::Open(path, manifest, error), "Truncated header accepted");
    std::filesystem::remove(path);
    require(VirtualGeometryPageStress::Create(path, 8, manifest, error), error);
    require(manifest.Metadata.Clusters.size() == 9 && manifest.Metadata.Groups.size() == 1,
            "Generated hierarchy counts");
    require(manifest.Metadata.Vertices.empty(), "Generated scene retained full RAM geometry");
    require(!VirtualGeometryPageStress::Create(path, 7, manifest, error), "Invalid generated leaf count accepted");
    std::filesystem::remove(path);
}
