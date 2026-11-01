#pragma once

#include <cstddef>

struct aiScene;

#if defined(FASTGLTF_IMPORT_BUILD_DLL)
#define FASTGLTF_IMPORT_API __declspec(dllexport)
#elif defined(RENDERINGPROJECT_USE_FASTGLTF) && RENDERINGPROJECT_USE_FASTGLTF
#define FASTGLTF_IMPORT_API __declspec(dllimport)
#else
#define FASTGLTF_IMPORT_API
#endif

// This boundary intentionally exposes no STL objects: the adapter also calls the Release Assimp DLL.
namespace FastGltfImport
{
    FASTGLTF_IMPORT_API bool Available() noexcept;
    FASTGLTF_IMPORT_API const aiScene* Load(const char* utf8Path, unsigned int flags, char* error,
                                            std::size_t errorCapacity) noexcept;
    FASTGLTF_IMPORT_API bool Release(const aiScene* scene) noexcept;
}

#undef FASTGLTF_IMPORT_API
