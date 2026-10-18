#include "virtualgeometry/virtualgeometrygpu.h"
#include "virtualgeometry/virtualgeometrybuilder.h"
#include "virtualgeometry/virtualgeometryruntime.h"
#include <stdexcept>
#include <cstring>
#include <cmath>

void TestVirtualGeometryGPU()
{
    auto check = [](bool condition, const char* message) {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    };
    std::vector<VirtualGeometryVertex> vertices(4);
    vertices[1].Position[0] = 1;
    vertices[2].Position[1] = 1;
    vertices[3].Position[0] = vertices[3].Position[1] = 1;
    for (auto& vertex : vertices)
    {
        vertex.Normal[2] = 1;
        vertex.TexCoord[0] = 0.25f;
        vertex.Diffuse[3] = 7;
    }
    const std::vector<uint32_t> indices{0, 1, 2, 1, 3, 2};
    auto built = VirtualGeometryBuilder::Build(vertices, indices, {}, 3);
    check(bool(built), "GPU test geometry build");
    VirtualGeometryGPU::PackedAsset packed;
    std::string error;
    check(VirtualGeometryGPU::Pack(built.Asset, packed, error), error.c_str());
    check(packed.Clusters.size() == built.Asset.Clusters.size(), "Lost hierarchy clusters");
    check(packed.Triangles.size() % 4 == 0, "Byte buffer is not padded");
    check(std::memcmp(packed.Vertices.data(), built.Asset.Vertices.data(),
                      packed.Vertices.size() * sizeof(VirtualGeometryVertex)) == 0,
          "Changed native vertex attributes");
    for (size_t offset = 0; offset < built.Asset.TriangleIndices.size(); ++offset)
    {
        uint32_t word = 0;
        std::memcpy(&word, packed.Triangles.data() + (offset & ~size_t(3)), 4);
        check(((word >> ((offset & 3) * 8)) & 255) == built.Asset.TriangleIndices[offset], "Byte decoding mismatch");
    }
    VirtualGeometryGPU::PackedAsset global;
    VirtualGeometryGPUAsset first{}, second{};
    check(VirtualGeometryGPU::Append(packed, global, first, error), error.c_str());
    check(VirtualGeometryGPU::Append(packed, global, second, error), error.c_str());
    check(second.ClusterOffset == first.ClusterCount, "Cluster offset");
    check(second.VertexOffset == packed.Vertices.size(), "Native vertex offset");
    check(second.TriangleOffset == packed.Triangles.size(), "Triangle byte offset");
    check(second.RootOffset == packed.Roots.size(), "Root offset");
    check(global.Roots[second.RootOffset] == packed.Roots[0], "Indices must remain asset relative");
    auto corrupt = built.Asset;
    corrupt.TriangleIndices[0] = 255;
    check(!VirtualGeometryGPU::Pack(corrupt, packed, error), "Invalid upload accepted");
    check(packed.Clusters.empty(), "Failed pack exposes partial geometry");
    auto runtime = std::make_shared<VirtualGeometryRuntime::RuntimeAsset>();
    runtime->Hierarchy = built.Asset;
    VirtualGeometryGPU::AssetRegistry registry;
    auto id = registry.Register(runtime, error);
    check(id == 0, "First native asset ID");
    check(registry.Register(runtime, error) == id, "Shared asset duplicated");
    auto replaced = std::make_shared<VirtualGeometryRuntime::RuntimeAsset>(*runtime);
    check(registry.Register(replaced, error) == 1, "Replacement overwrites live asset");
    check(registry.Table().size() == 2, "Unexpected registration count");
    replaced = std::make_shared<VirtualGeometryRuntime::RuntimeAsset>(*runtime);
    replaced->Error = "Failed build";
    check(registry.Register(replaced, error) == UINT32_MAX, "Failed registration exposed");
    check(registry.Register(nullptr, error) == UINT32_MAX, "Null registration exposed");
    vertices.clear();
    std::vector<uint32_t> curvedIndices;
    constexpr uint32_t size = 24;
    for (uint32_t y = 0; y <= size; ++y)
    {
        for (uint32_t x = 0; x <= size; ++x)
        {
            VirtualGeometryVertex vertex;
            vertex.Position[0] = float(x) / size;
            vertex.Position[1] = float(y) / size;
            vertex.Position[2] = 0.1f * std::sin(float(x) * 0.3f) * std::cos(float(y) * 0.2f);
            vertex.Normal[2] = 1;
            vertex.Diffuse[3] = 7;
            vertices.push_back(vertex);
        }
    }
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            uint32_t a = y * (size + 1) + x, b = a + 1, c = a + size + 1, d = c + 1;
            curvedIndices.insert(curvedIndices.end(), {a, c, b, b, c, d});
        }
    }
    VirtualGeometryBuildConfig config;
    config.MaxClusterVertices = 64;
    config.MaxClusterTriangles = 124;
    built = VirtualGeometryBuilder::Build(vertices, curvedIndices, config);
    check(bool(built) && built.Asset.Levels.size() > 1, "Packing fixture must contain parent geometry");
    check(VirtualGeometryGPU::Pack(built.Asset, packed, error), error.c_str());
    check(packed.Clusters.size() == built.Asset.Clusters.size(), "Parent cluster upload incomplete");
    for (size_t index = 0; index < packed.Clusters.size(); ++index)
    {
        check(packed.Clusters[index].Level == built.Asset.Clusters[index].Level, "Wrong uploaded hierarchy level");
        check(packed.Clusters[index].VertexOffset == built.Asset.Clusters[index].VertexOffset,
              "Parent vertex remapped to leaf");
    }
    check(std::memcmp(packed.Vertices.data(), built.Asset.Vertices.data(),
                      packed.Vertices.size() * sizeof(VirtualGeometryVertex)) == 0,
          "Changed parent geometry attributes");
    global = {};
    check(VirtualGeometryGPU::Append(packed, global, first, error), error.c_str());
    check((first.Flags >> 8) == built.Asset.Levels.size() - 1, "GPU hierarchy depth metadata mismatch");
}
