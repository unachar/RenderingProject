#include "virtualgeometry/virtualgeometrypages.h"
#include "virtualgeometry/virtualgeometrypagecache.h"
#include "virtualgeometry/virtualgeometrybuilder.h"
#include <cstring>
#include <fstream>
#include <stdexcept>

void TestVirtualGeometryPages()
{
    auto require = [](bool condition, const std::string& message)
    {
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
        size_t vertexBytes = source.VertexCount * sizeof(VirtualGeometryVertex);
        size_t triangleBytes = source.TriangleCount * 3;
        require(cluster.VertexOffset + vertexBytes <= page.size(), "Vertex page overflow");
        require((cluster.TriangleOffset + triangleBytes + 3) / 4 * 4 <= page.size(), "Triangle word overflow");
        require(std::memcmp(page.data() + cluster.VertexOffset, built.Asset.Vertices.data() + source.VertexOffset, vertexBytes) == 0,
            "Page vertex roundtrip");
        require(std::memcmp(page.data() + cluster.TriangleOffset, built.Asset.TriangleIndices.data() + source.TriangleOffset, triangleBytes) == 0,
            "Page triangle roundtrip");
    }
    for (uint32_t root : built.Asset.RootClusters)
    {
        require(packed.PinnedPages.at(packed.Metadata.Clusters[root].PageIndex) != 0, "Root is not pinned");
    }
    const auto path = std::filesystem::current_path() / "vg_page_roundtrip.vgp2";
    require(VirtualGeometryPageCache::WriteAtomic(path, packed, error), error);
    VirtualGeometryPageCache::PageManifest manifest;
    require(VirtualGeometryPageCache::Open(path, manifest, error), error);
    require(manifest.Pages.size() == packed.Payloads.size(), "Cache page count");
    std::vector<uint8_t> payload(VirtualGeometryPages::PageBytes);
    for (uint32_t page = 0; page < manifest.Pages.size(); ++page)
    {
        require(VirtualGeometryPageCache::ReadPage(manifest, page, payload, error), error);
        require(payload == packed.Payloads[page], "Disk page roundtrip");
    }
    require(!VirtualGeometryPageCache::ReadPage(manifest, UINT32_MAX, payload, error), "Invalid page accepted");
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
}
