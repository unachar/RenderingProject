#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include "assimp/cimport.h"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

static PROCESS_MEMORY_COUNTERS_EX Memory()
{
    PROCESS_MEMORY_COUNTERS_EX value{};
    value.cb = sizeof(value);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value), sizeof(value)))
        throw std::runtime_error("Cannot read worker memory");
    return value;
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    try
    {
        using Load = const aiScene* (*)(const char*, unsigned int, char*, std::size_t) noexcept;
        using Release = bool (*)(const aiScene*) noexcept;
        HMODULE module = nullptr;
        Load load = nullptr;
        Release release = nullptr;
        const bool native = std::string(argv[1]) == "native";
        if (!native)
        {
            module = LoadLibraryExW(std::filesystem::absolute(argv[1]).c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module)
                throw std::runtime_error("Cannot load worker adapter");
            load = reinterpret_cast<Load>(GetProcAddress(module, "?Load@FastGltfImport@@YAPEBUaiScene@@PEBDIPEAD_K@Z"));
            release = reinterpret_cast<Release>(GetProcAddress(module, "?Release@FastGltfImport@@YA_NPEBUaiScene@@@Z"));
            if (!load || !release)
                throw std::runtime_error("Worker export mismatch");
        }
        constexpr unsigned int flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                       aiProcess_ImproveCacheLocality | aiProcess_GenSmoothNormals | aiProcess_ConvertToLeftHanded;
        const auto before = Memory();
        char error[512]{};
        const auto scene = native ? aiImportFile(argv[2], flags) : load(argv[2], flags, error, sizeof(error));
        if (!scene || !scene->HasMeshes())
            throw std::runtime_error(std::string("Memory worker import failed: ") + error);
        const auto loaded = Memory();
        if (native)
            aiReleaseImport(scene);
        else if (!release(scene))
            throw std::runtime_error("Memory worker scene owner mismatch");
        const auto released = Memory();
        std::cout << "{\"initialPrivateBytes\":" << before.PrivateUsage << ",\"initialPeakCommitBytes\":" << before.PeakPagefileUsage
                  << ",\"loadedPrivateBytes\":" << loaded.PrivateUsage << ",\"peakCommitBytes\":" << loaded.PeakPagefileUsage
                  << ",\"releasedPrivateBytes\":" << released.PrivateUsage << '}';
        if (module)
            FreeLibrary(module);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
