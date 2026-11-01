#define NOMINMAX
#include <windows.h>
#include "assimp/cimport.h"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    struct Adapter
    {
        using Load = const aiScene* (*)(const char*, unsigned int, char*, std::size_t) noexcept;
        using Release = bool (*)(const aiScene*) noexcept;
        HMODULE module;
        Load load;
        Release release;

        explicit Adapter(const std::filesystem::path& path)
        {
            module = LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module)
            {
                throw std::runtime_error("Cannot load benchmark adapter");
            }
            load = reinterpret_cast<Load>(GetProcAddress(module, "?Load@FastGltfImport@@YAPEBUaiScene@@PEBDIPEAD_K@Z"));
            release = reinterpret_cast<Release>(GetProcAddress(module, "?Release@FastGltfImport@@YA_NPEBUaiScene@@@Z"));
            if (!load || !release)
            {
                throw std::runtime_error("Benchmark adapter export mismatch");
            }
        }

        ~Adapter()
        {
            FreeLibrary(module);
        }

        Adapter(const Adapter&) = delete;
        Adapter& operator=(const Adapter&) = delete;
    };
}

int main(int argc, char** argv)
{
    try
    {
        Adapter before("bin/validation/fastgltf/optimization-before/FastGltfAdapter.dll");
        Adapter after(argc > 1 ? std::filesystem::path(argv[1]) :
                      std::filesystem::path("External/fastgltf/install-dx12/Release/bin/FastGltfAdapter.dll"));
        if (before.module == after.module)
        {
            throw std::runtime_error("Benchmark must load two independent DLL images");
        }
        constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                       aiProcess_ImproveCacheLocality | aiProcess_GenSmoothNormals |
                                       aiProcess_ConvertToLeftHanded;
        const char* paths[]{"External/RTXNS/external/donut/thirdparty/cgltf/fuzz/data/Box.glb",
                            "External/Draco/upstream/testdata/CesiumMan/glTF_Binary/CesiumMan.glb",
                            "bin/validation/fastgltf/material.gltf", "bin/validation/fastgltf/animated.gltf"};
        const char* modes[]{"native", "before", "after"};
        auto run = [&](const char* path, int mode) {
            char error[512]{};
            auto* adapter = mode == 1 ? &before : &after;
            const auto scene = mode == 0 ? aiImportFile(path, flags) : adapter->load(path, flags, error, sizeof(error));
            if (!scene || !scene->HasMeshes())
            {
                throw std::runtime_error(std::string("Benchmark import failed: ") + error);
            }
            if (mode == 0)
            {
                aiReleaseImport(scene);
            }
            else if (!adapter->release(scene))
            {
                throw std::runtime_error("Benchmark scene owner mismatch");
            }
        };
        std::cout << "[\n";
        bool first = true;
        for (const auto path : paths)
        {
            for (int mode = 0; mode < 3; ++mode)
            {
                run(path, mode);
            }
            for (int pass = 0; pass < 5; ++pass)
            {
                for (int step = 0; step < 3; ++step)
                {
                    const int mode = (pass + step) % 3;
                    const auto start = std::chrono::steady_clock::now();
                    for (int iteration = 0; iteration < 30; ++iteration)
                    {
                        run(path, mode);
                    }
                    const auto ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
                        30;
                    if (!first)
                    {
                        std::cout << ",\n";
                    }
                    first = false;
                    std::cout << "{\"path\":\"" << path << "\",\"mode\":\"" << modes[mode] << "\",\"pass\":" << pass
                              << ",\"ms\":" << ms << '}';
                }
            }
        }
        std::cout << "\n]\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
