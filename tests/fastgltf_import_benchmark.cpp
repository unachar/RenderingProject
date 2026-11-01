#include "source/models/fastgltfimport.h"
#include "assimp/cimport.h"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

int main()
{
    using Clock = std::chrono::steady_clock;
    constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                   aiProcess_ImproveCacheLocality | aiProcess_GenSmoothNormals |
                                   aiProcess_ConvertToLeftHanded;
    const char* paths[] = {"External/RTXNS/external/donut/thirdparty/cgltf/fuzz/data/Box.glb",
                           "External/Draco/upstream/testdata/CesiumMan/glTF_Binary/CesiumMan.glb",
                           "bin/validation/fastgltf/material.gltf", "bin/validation/fastgltf/animated.gltf"};
    std::cout << "[\n";
    bool first = true;
    for (const auto path : paths)
    {
        for (int pass = 0; pass < 5; ++pass)
        {
            double elapsed[2]{};
            for (int step = 0; step < 2; ++step)
            {
                const bool fast = ((pass + step) % 2) != 0;
                const auto start = Clock::now();
                for (int iteration = 0; iteration < 20; ++iteration)
                {
                    char error[512]{};
                    const auto scene =
                        fast ? FastGltfImport::Load(path, flags, error, sizeof(error)) : aiImportFile(path, flags);
                    if (!scene || !scene->HasMeshes())
                    {
                        std::cerr << "Import failed: " << path << ": " << error << '\n';
                        return 1;
                    }
                    if (fast)
                    {
                        FastGltfImport::Release(scene);
                    }
                    else
                    {
                        aiReleaseImport(scene);
                    }
                }
                elapsed[fast] = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / 20;
            }
            if (!first)
            {
                std::cout << ",\n";
            }
            first = false;
            std::cout << "{\"path\":\"" << path << "\",\"pass\":" << pass << ",\"assimpMs\":" << elapsed[0]
                      << ",\"fastgltfMs\":" << elapsed[1] << '}';
        }
    }
    std::cout << "\n]\n";
}
