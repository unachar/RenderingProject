#include "virtualgeometry/virtualgeometrybuilder.h"
#include "virtualgeometry/virtualgeometrypagestress.h"
#include "virtualgeometry/virtualgeometryserializer.h"
#include "virtualgeometry/virtualgeometryvalidator.h"
#include "virtualgeometry/virtualgeometryreport.h"
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{
    void Check(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct Mesh
    {
        std::vector<VirtualGeometryVertex> Vertices;
        std::vector<uint32_t> Indices;
    };

    Mesh MakeGrid(uint32_t size)
    {
        Mesh mesh;
        for (uint32_t row = 0; row <= size; ++row)
        {
            for (uint32_t column = 0; column <= size; ++column)
            {
                float x = static_cast<float>(column) / size;
                float y = static_cast<float>(row) / size;
                VirtualGeometryVertex vertex;
                vertex.Position[0] = x;
                vertex.Position[1] = y;
                vertex.Position[2] = 0.05f * std::sin(x * 12) * std::sin(y * 12);
                vertex.Normal[2] = 1;
                vertex.TexCoord[0] = x;
                vertex.TexCoord[1] = y;
                std::fill(std::begin(vertex.Diffuse), std::end(vertex.Diffuse), 1.0f);
                mesh.Vertices.push_back(vertex);
            }
        }
        for (uint32_t row = 0; row < size; ++row)
        {
            for (uint32_t column = 0; column < size; ++column)
            {
                uint32_t first = row * (size + 1) + column;
                mesh.Indices.insert(mesh.Indices.end(),
                    {first, first + 1, first + size + 1,
                     first + 1, first + size + 2, first + size + 1});
            }
        }
        return mesh;
    }

    Mesh MakeCube()
    {
        Mesh mesh;
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            VirtualGeometryVertex vertex;
            vertex.Position[0] = (corner & 1) ? 1.0f : -1.0f;
            vertex.Position[1] = (corner & 2) ? 1.0f : -1.0f;
            vertex.Position[2] = (corner & 4) ? 1.0f : -1.0f;
            vertex.Normal[2] = 1;
            vertex.Diffuse[3] = 1;
            mesh.Vertices.push_back(vertex);
        }
        mesh.Indices = {0,2,1,1,2,3,4,5,6,5,7,6,0,1,4,1,5,4,
                        2,6,3,3,6,7,0,4,2,2,4,6,1,3,5,3,7,5};
        return mesh;
    }

    VirtualGeometryAsset TestRoundTrip(const Mesh& mesh, const char* name, bool multipleLevels)
    {
        auto result = VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices, {}, 7);
        Check(static_cast<bool>(result), result.Error);
        auto& asset = result.Asset;
        asset.DebugName = name;
        Check(asset.Stats.FinalLeafTriangles == mesh.Indices.size() / 3, "Leaf coverage");
        Check(!multipleLevels || asset.Levels.size() > 1, "Expected hierarchical reduction");
        std::string error;
        Check(VirtualGeometryValidator::Validate(asset, error), error);
        std::vector<uint8_t> bytes;
        Check(VirtualGeometrySerializer::Encode(asset, bytes, error), error);
        uint64_t hash = VirtualGeometrySerializer::Hash(bytes);
        auto second = VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices, {}, 7);
        Check(static_cast<bool>(second), second.Error);
        second.Asset.DebugName = name;
        std::vector<uint8_t> repeated;
        Check(VirtualGeometrySerializer::Encode(second.Asset, repeated, error), error);
        Check(bytes == repeated, "Deterministic complete payload");
        auto path = std::filesystem::temp_directory_path() / (std::string(name) + ".vgmesh");
        Check(VirtualGeometrySerializer::Save(path, asset, error), error);
        VirtualGeometryBuildStats measured = asset.Stats;
        asset = {}; // Unload before reading; original deterministic bytes remain as oracle.
        VirtualGeometryAsset loaded;
        Check(VirtualGeometrySerializer::Load(path, loaded, error), error);
        Check(VirtualGeometrySerializer::Encode(loaded, repeated, error), error);
        Check(bytes == repeated, "Complete geometry/hierarchy/bounds/error round trip");
        Check(VirtualGeometryReport::Save(path.string() + ".txt", loaded, error), error);
        std::cout << name << " hash=" << hash << " source=" << measured.SourceTriangles
                  << " leaf=" << measured.LeafClusters << " total=" << measured.TotalClusters
                  << " groups=" << measured.TotalGroups << " levels=" << measured.HierarchyLevels
                  << " roots=" << second.Asset.RootClusters.size()
                  << " build_ms=" << measured.BuildMilliseconds << '\n';
        for (size_t level = 0; level < second.Asset.Levels.size(); ++level)
        {
            const auto& stats = second.Asset.Levels[level];
            std::cout << "  level=" << level << " clusters=" << stats.Clusters
                      << " triangles=" << stats.Triangles << " vertices=" << stats.Vertices << '\n';
        }
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".txt");

        auto damaged = bytes;
        damaged.back() ^= 1;
        Check(!VirtualGeometrySerializer::Decode(damaged, loaded, error), "Checksum rejection");
        for (size_t length : {size_t(0), size_t(4), bytes.size() - 1})
        {
            Check(!VirtualGeometrySerializer::Decode(
                std::span<const uint8_t>(bytes.data(), length), loaded, error), "Truncation rejection");
        }
        damaged = bytes;
        damaged[4] = 99;
        Check(!VirtualGeometrySerializer::Decode(damaged, loaded, error), "Version rejection");
        damaged = bytes;
        std::fill(damaged.begin() + 32, damaged.begin() + 48, uint8_t(0xff));
        Check(!VirtualGeometrySerializer::Decode(damaged, loaded, error), "Hostile section bounds");
        VirtualGeometryLoadLimits limits;
        limits.MaxFileBytes = bytes.size() - 1;
        Check(!VirtualGeometrySerializer::Decode(bytes, loaded, error, limits), "Load budget");
        return second.Asset;
    }

    void TestBadGeometry()
    {
        Mesh mesh = MakeCube();
        mesh.Vertices[0].Normal[0] = std::numeric_limits<float>::quiet_NaN();
        Check(!VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices), "NaN failure");
        mesh = MakeCube();
        mesh.Vertices[0].Position[0] = std::numeric_limits<float>::infinity();
        Check(!VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices), "Inf failure");
        mesh = MakeCube();
        mesh.Indices[0] = 999;
        Check(!VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices), "Index failure");
        Check(!VirtualGeometryBuilder::Build({}, {}), "Empty source failure");
        mesh = MakeCube();
        mesh.Indices.resize(3);
        auto single = VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices);
        Check(static_cast<bool>(single) && single.Asset.Stats.FinalLeafTriangles == 1, "One triangle");
        mesh.Indices.insert(mesh.Indices.end(), {0,0,1,0,2,1});
        auto cleaned = VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices);
        Check(static_cast<bool>(cleaned), cleaned.Error);
        Check(cleaned.Asset.Stats.RemovedDegenerateTriangles == 1, "Degenerate accounting");
        Check(cleaned.Asset.Stats.RemovedDuplicateTriangles == 1, "Duplicate accounting");
        VirtualGeometryBuildConfig invalid;
        invalid.MaxClusterVertices = VirtualGeometryConfig::MaxClusterVertices + 1;
        Check(!VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices, invalid), "Bad config");

        mesh = MakeCube();
        for (auto& vertex : mesh.Vertices)
        {
            for (float& position : vertex.Position)
            {
                position *= 1e-12f;
            }
        }
        VirtualGeometryBuildConfig exactBounds;
        exactBounds.BoundaryEpsilon = 0;
        auto tiny = VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices, exactBounds);
        Check(static_cast<bool>(tiny), "Tiny finite geometry: " + tiny.Error);
        Check(tiny.Asset.Clusters[0].Bounds.Radius > 0, "Tiny sphere must cover nonzero geometry");
        mesh = MakeCube();
        for (auto& vertex : mesh.Vertices)
        {
            for (float& position : vertex.Position)
            {
                position *= 1e8f;
            }
        }
        auto large = VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices);
        Check(static_cast<bool>(large), "Large finite geometry: " + large.Error);
        mesh.Indices = {0, 0, 0};
        Check(!VirtualGeometryBuilder::Build(mesh.Vertices, mesh.Indices), "All-degenerate failure");
    }

    void TestSeamsAndIslands()
    {
        Mesh mesh = MakeGrid(40);
        // Duplicate every corner with a second UV mapping. Coincident positional edges
        // exercise seam-aware boundary detection and non-manifold locking.
        auto count = static_cast<uint32_t>(mesh.Vertices.size());
        auto copied = mesh.Vertices;
        for (auto& vertex : copied)
        {
            vertex.TexCoord[0] += 1;
            vertex.Normal[2] = -1;
        }
        mesh.Vertices.insert(mesh.Vertices.end(), copied.begin(), copied.end());
        auto indices = mesh.Indices;
        for (uint32_t index : indices)
        {
            mesh.Indices.push_back(index + count);
        }
        TestRoundTrip(mesh, "vg_seams", false);

        mesh = MakeGrid(80);
        count = static_cast<uint32_t>(mesh.Vertices.size());
        copied = mesh.Vertices;
        for (auto& vertex : copied)
        {
            vertex.TexCoord[0] += 1;
            vertex.Normal[1] = 0.1f;
        }
        mesh.Vertices.insert(mesh.Vertices.end(), copied.begin(), copied.end());
        for (size_t triangle = 0; triangle < mesh.Indices.size(); triangle += 3)
        {
            float centerX = 0;
            for (size_t corner = 0; corner < 3; ++corner)
            {
                centerX += mesh.Vertices[mesh.Indices[triangle + corner]].Position[0];
            }
            if (centerX > 1.5f)
            {
                for (size_t corner = 0; corner < 3; ++corner)
                {
                    mesh.Indices[triangle + corner] += count;
                }
            }
        }
        TestRoundTrip(mesh, "vg_split_uv_normal_seam", true);

        Mesh islands;
        for (uint32_t island = 0; island < 20; ++island)
        {
            Mesh cube = MakeCube();
            auto base = static_cast<uint32_t>(islands.Vertices.size());
            for (auto vertex : cube.Vertices)
            {
                vertex.Position[0] += island * 5;
                islands.Vertices.push_back(vertex);
            }
            for (uint32_t index : cube.Indices)
            {
                islands.Indices.push_back(base + index);
            }
        }
        VirtualGeometryBuildConfig config;
        config.MaxClusterTriangles = 12;
        config.MaxClusterVertices = 8;
        config.MaxHierarchyLevels = 1;
        auto result = VirtualGeometryBuilder::Build(islands.Vertices, islands.Indices, config);
        Check(static_cast<bool>(result) && result.Asset.RootClusters.size() > 1, "Multiple roots");
        std::string error;
        Check(VirtualGeometryValidator::Validate(result.Asset, error), error);
    }
}

void TestVirtualGeometryRuntime();
void TestVirtualGeometrySelection();
void TestVirtualGeometryGPU();
void TestVirtualGeometryPages();
void TestVirtualGeometryResidentSelection();
void TestVirtualGeometryResidency();
void TestVirtualGeometryPageIo();

int main(int argc, char** argv)
{
    try
    {
        if (argc == 4 && std::string_view(argv[1]) == "--generate-streaming")
        {
            std::filesystem::path directory(argv[2]);
            uint64_t requested = std::stoull(argv[3]);
            if (!requested || requested > 1000000000ull) throw std::runtime_error("Stress triangle range is 1..1000000000");
            std::filesystem::create_directories(directory);
            uint64_t count = (requested+VirtualGeometryPageStress::SourceTrianglesPerAsset-1)/VirtualGeometryPageStress::SourceTrianglesPerAsset;
            uint64_t expectedBytes = count * 74ull * 1024 * 1024;
            if (std::filesystem::space(directory).available < expectedBytes+8ull*1024*1024*1024)
                throw std::runtime_error("Stress generation would leave less than 8 GiB free disk space");
            for (uint64_t asset = 0; asset < count; ++asset)
            {
                auto path = directory/("asset-"+std::to_string(asset)+".vgp2");
                VirtualGeometryPageCache::PageManifest manifest;
                std::string error;
                if (!VirtualGeometryPageCache::Open(path,manifest,error) &&
                    !VirtualGeometryPageStress::Create(path,VirtualGeometryPageStress::LeavesPerAsset,manifest,error))
                    throw std::runtime_error(error);
                std::cout << "Generated asset " << asset+1 << '/' << count << ", disk bytes " << manifest.FileBytes << std::endl;
            }
            std::cout << "Source triangles " << count*VirtualGeometryPageStress::SourceTrianglesPerAsset << ", instances " << count << ", native assets " << count << '\n';
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--streaming-cpu")
        {
            TestVirtualGeometryPages();
            TestVirtualGeometryResidentSelection();
            TestVirtualGeometryResidency();
            TestVirtualGeometryPageIo();
            std::cout << "PASS: Page cache, resident selection, residency, SSD I/O\n";
            return 0;
        }
        TestVirtualGeometryRuntime();
        TestVirtualGeometrySelection();
        TestVirtualGeometryGPU();
        TestVirtualGeometryPages();
        TestVirtualGeometryResidentSelection();
        TestVirtualGeometryResidency();
        TestVirtualGeometryPageIo();
        TestBadGeometry();
        TestRoundTrip(MakeCube(), "vg_cube", false);
        auto asset = TestRoundTrip(MakeGrid(80), "vg_medium", true);
        std::string error;
        auto corrupt = asset;
        corrupt.TriangleIndices[0] = 255;
        Check(!VirtualGeometryValidator::Validate(corrupt, error), "Local index rejection");
        corrupt = asset;
        corrupt.Clusters[0].MaterialId++;
        Check(!VirtualGeometryValidator::Validate(corrupt, error), "Material rejection");
        corrupt = asset;
        corrupt.Clusters[0].GroupIndex = VirtualGeometryConfig::InvalidIndex;
        Check(!VirtualGeometryValidator::Validate(corrupt, error), "Unlisted terminal root rejection");
        corrupt = asset;
        corrupt.Groups[0].FirstParent = corrupt.Groups[0].FirstChild;
        Check(!VirtualGeometryValidator::Validate(corrupt, error), "Cycle/level rejection");
        corrupt = asset;
        auto parent = corrupt.HierarchyIndices[corrupt.Groups[0].FirstParent];
        corrupt.Clusters[parent].LodError = -1;
        Check(!VirtualGeometryValidator::Validate(corrupt, error), "Invalid parent error rejection");
        corrupt = asset;
        corrupt.Clusters[0].Bounds.Radius = -1;
        Check(!VirtualGeometryValidator::Validate(corrupt, error), "Negative sphere rejection");
        TestSeamsAndIslands();
        bool million = argc > 1 && std::string_view(argv[1]) == "--million";
        TestRoundTrip(MakeGrid(million ? 710 : 224), "vg_stress", true);
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
