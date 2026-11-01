#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <cmath>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None)
    {
        return 3;
    }
    fastgltf::Parser parser;
    auto result = parser.loadGltf(data.get(), path.parent_path(), fastgltf::Options::LoadExternalBuffers);
    if (result.error() != fastgltf::Error::None)
    {
        std::cerr << "parse error " << static_cast<int>(result.error()) << '\n';
        return 4;
    }
    const auto& asset = result.get();
    std::size_t vertices = 0;
    double checksum = 0;
    for (const auto& mesh : asset.meshes)
    {
        for (const auto& primitive : mesh.primitives)
        {
            const auto position = primitive.findAttribute("POSITION");
            if (position == primitive.attributes.end())
            {
                return 5;
            }
            for (const auto value :
                 fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, asset.accessors[position->accessorIndex]))
            {
                for (int component = 0; component < 3; ++component)
                {
                    if (!std::isfinite(value[component]))
                    {
                        return 6;
                    }
                    checksum += std::abs(value[component]);
                }
                ++vertices;
            }
        }
    }
    if (asset.nodes.empty() || vertices == 0 || checksum == 0)
    {
        return 7;
    }
    std::cout << "PASS SDK file parse and position decode: meshes=" << asset.meshes.size()
              << " nodes=" << asset.nodes.size() << " vertices=" << vertices << " checksum=" << checksum << '\n';
}
