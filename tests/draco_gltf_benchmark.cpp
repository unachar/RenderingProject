#include "assimp/cimport.h"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
using namespace std;
#include "source/models/modelimportutils.h"

int main()
{
    const char* plain = "External/Draco/upstream/testdata/Box/glTF_Binary/Box.glb";
    const char* compressed = "External/Draco/upstream/testdata/Box/glTF_Binary/Box_Draco.glb";

    struct Case
    {
        const char* name;
        const char* path;
        const char* setting;
    };

    const Case cases[]{{"native-plain", plain, "0"},
                       {"fastgltf-plain", plain, "1"},
                       {"default-draco", compressed, ""},
                       {"fastgltf-draco", compressed, "1"}};
    constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                   aiProcess_ImproveCacheLocality | aiProcess_GenSmoothNormals |
                                   aiProcess_ConvertToLeftHanded;
    using Clock = std::chrono::steady_clock;
    std::cout << "{\"plainBytes\":" << std::filesystem::file_size(plain)
              << ",\"dracoBytes\":" << std::filesystem::file_size(compressed) << ",\"passes\":[\n";
    bool first = true;
    for (int pass = 0; pass < 5; ++pass)
    {
        for (int step = 0; step < 4; ++step)
        {
            const auto& item = cases[(pass + step) % 4];
            _putenv_s("DX12_USE_FASTGLTF", item.setting);
            const auto start = Clock::now();
            for (int iteration = 0; iteration < 100; ++iteration)
            {
                const auto scene = ImportModelScene(item.path, flags);
                if (!scene || scene->mNumMeshes != 1 || scene->mMeshes[0]->mNumFaces != 12)
                {
                    std::cerr << "Import failed: " << item.name << '\n';
                    return 1;
                }
                ReleaseModelScene(scene);
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / 100;
            if (!first)
            {
                std::cout << ",\n";
            }
            first = false;
            std::cout << "{\"case\":\"" << item.name << "\",\"ms\":" << elapsed << '}';
        }
    }
    std::cout << "\n]}\n";
}
