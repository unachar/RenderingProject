#include "rendering/meshlets/meshletbuilder.h"
#include <cassert>
#include <iostream>
#include <fstream>

int main()
{
    using namespace Meshlets;
    std::vector<Position> positions{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    std::vector<uint32_t> indices{0, 1, 2, 2, 1, 3};
    auto asset = Build(positions, indices, {3, 1, 1});
    assert(asset.Meshlets.size() == 2 && asset.Clusters.size() == 2);
    std::string error;
    assert(Validate(asset, positions, error));
    std::vector<uint32_t> reconstructed;
    for (auto &meshlet : asset.Meshlets)
        for (uint32_t i = 0; i < meshlet.PrimitiveCount; ++i)
        {
            uint32_t triangle = asset.Primitives.at(meshlet.PrimitiveOffset + i);
            for (uint32_t j = 0; j < 3; ++j)
                reconstructed.push_back(
                    asset.Vertices.at(meshlet.VertexOffset + ((triangle >> (j * 8)) & 255)));
        }
    assert(reconstructed == indices);
    assert(asset.Bounds[0].ConeCutoff >= 0 && asset.Bounds[0].ConeCutoff < 0.01f);
    auto invalid = asset;
    invalid.Vertices[0] = 99;
    assert(!Validate(invalid, positions, error));
    invalid = asset;
    invalid.Primitives[0] = 255;
    assert(!Validate(invalid, positions, error));
    invalid = asset;
    invalid.Bounds[0].Radius = 0;
    assert(!Validate(invalid, positions, error));
    invalid = asset;
    invalid.Clusters[0].Bounds.Radius = 0;
    assert(!Validate(invalid, positions, error));
    bool rejected = false;
    try
    {
        Build(positions, std::vector<uint32_t>{0, 1, 99});
    }
    catch (...)
    {
        rejected = true;
    }
    assert(rejected);
    rejected = false;
    try
    {
        Build(positions, indices, {257, 1, 1});
    }
    catch (...)
    {
        rejected = true;
    }
    assert(rejected);
    auto degenerate = Build(positions, std::vector<uint32_t>{0, 0, 0});
    assert(degenerate.Bounds[0].ConeCutoff == 1);
    auto opposed = Build(positions, std::vector<uint32_t>{0, 1, 2, 2, 1, 0});
    assert(opposed.Bounds[0].ConeCutoff == 1);
    assert(Build({}, {}).Meshlets.empty());
    const auto key = GeometryKey(positions, indices, {3, 1, 1});
    assert(key != GeometryKey(positions, indices, {}));
    const std::filesystem::path path = "build/meshlets/test.meshlets";
    assert(Save(path, asset, key));
    Asset loaded;
    assert(Load(path, loaded, key, positions));
    assert(loaded.Primitives == asset.Primitives && loaded.Vertices == asset.Vertices);
    assert(!Load(path, loaded, key + 1, positions));
    // A different in-range, coplanar triangle still passes structural validation.
    // The persisted payload must reject this same-length corruption.
    auto planar = Build(positions, indices);
    assert(Save(path, planar, key));
    const auto payloadBytes = planar.Meshlets.size() * sizeof(Meshlet) +
                              (planar.Vertices.size() + planar.Primitives.size()) * sizeof(uint32_t) +
                              planar.Bounds.size() * sizeof(Bounds) +
                              planar.Clusters.size() * sizeof(Cluster);
    const auto primitiveOffset = std::filesystem::file_size(path) - payloadBytes +
                                 planar.Meshlets.size() * sizeof(Meshlet) +
                                 planar.Vertices.size() * sizeof(uint32_t);
    {
        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(primitiveOffset + sizeof(uint32_t));
        stream.write(reinterpret_cast<const char *>(&planar.Primitives[0]), sizeof(uint32_t));
    }
    assert(!Load(path, loaded, key, positions));
    {
        std::ofstream truncate(path, std::ios::binary | std::ios::trunc);
        truncate << "bad";
    }
    assert(!Load(path, loaded, key, positions));
    assert(DispatchDimensions(0)[0] == 0);
    assert(DispatchDimensions(65536)[0] <= 65535);
    assert(uint64_t(DispatchDimensions(1000000)[0]) * DispatchDimensions(1000000)[1] >= 1000000);
    std::cout << "Meshlet CPU tests PASS\n";
}
