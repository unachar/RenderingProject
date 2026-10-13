#include "virtualgeometry/virtualgeometryruntime.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <set>
#include <array>

void TestVirtualGeometryRuntime()
{
    auto check = [](bool value, const char *message)
    {
        if (!value)
            throw std::runtime_error(message);
    };
    std::vector<VirtualGeometryVertex> vertices(4);
    vertices[1].Position[0] = 1;
    vertices[2].Position[1] = 1;
    vertices[3] = vertices[0];
    vertices[3].TexCoord[0] = 0.75f; // Same position, distinct UV seam.
    for (auto &v : vertices)
    {
        v.Normal[2] = 1;
        v.Diffuse[3] = 7;
    }
    const std::vector<uint32_t> indices{0, 1, 2, 3, 2, 1};
    const auto directory = std::filesystem::temp_directory_path() / "render-vg-runtime-tests";
    std::filesystem::create_directories(directory);
    auto first = VirtualGeometryRuntime::Acquire(vertices, indices, 2, 7, directory);
    check(first && first->Error.empty(), "Runtime build");
    check(first->Hierarchy.Config.MaxClusterVertices == 64, "Mesh Shader vertex limit");
    check(first->Hierarchy.Config.MaxClusterTriangles <= 126, "Mesh Shader triangle limit");
    auto shared = VirtualGeometryRuntime::Acquire(vertices, indices, 2, 7, directory);
    check(shared == first, "Repeated mesh must share asset");
    std::multiset<std::array<uint32_t, 3>> triangles;
    for (const auto &meshlet : first->Meshlets.Meshlets)
        for (uint32_t i = 0; i < meshlet.PrimitiveCount; ++i)
        {
            const uint32_t packed = first->Meshlets.Primitives.at(meshlet.PrimitiveOffset + i);
            std::array<uint32_t, 3> tri;
            for (uint32_t j = 0; j < 3; ++j)
                tri[j] = first->Meshlets.Vertices.at(meshlet.VertexOffset + ((packed >> (8 * j)) & 255));
            // Canonical cyclic rotation preserves winding.
            while (tri[0] > tri[1] || tri[0] > tri[2])
                tri = {tri[1], tri[2], tri[0]};
            triangles.insert(tri);
        }
    check(triangles == std::multiset<std::array<uint32_t, 3>>{{0, 1, 2}, {1, 3, 2}},
          "Leaf triangles and seam remap");
    const auto originalKey = first->Key;
    auto changed = vertices;
    changed[3].TexCoord[0] = 0.5f;
    auto edited = VirtualGeometryRuntime::Acquire(changed, indices, 2, 7, directory);
    check(edited->Key != originalKey && edited->Error.empty(), "Attribute cache invalidation");
    auto material = VirtualGeometryRuntime::Acquire(vertices, indices, 2, 8, directory);
    check(material->Key != originalKey, "Material cache invalidation");
    const auto cache = first->CachePath;
    first.reset();
    shared.reset();
    auto loaded = VirtualGeometryRuntime::Acquire(vertices, indices, 2, 7, directory);
    check(loaded->CacheHit && loaded->Error.empty(), "Cache reload");
    loaded.reset();
    {
        std::ofstream output(cache, std::ios::binary | std::ios::trunc);
        output << "corrupt";
    }
    auto recovered = VirtualGeometryRuntime::Acquire(vertices, indices, 2, 7, directory);
    check(!recovered->CacheHit && recovered->Error.empty(), "Corrupt cache rebuild");
    auto invalid =
        VirtualGeometryRuntime::Acquire(vertices, std::vector<uint32_t>{0, 1, 99}, 2, 7, directory);
    check(!invalid->Error.empty() && invalid->Meshlets.Meshlets.empty(), "Invalid source fallback");
    for (auto path : {cache, edited->CachePath, material->CachePath, invalid->CachePath})
        if (!path.empty())
            std::filesystem::remove(path);
}
