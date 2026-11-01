#pragma once

#include "cimport.h"
#include "scene.h"
#include "fastgltfimport.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

inline filesystem::path ModelPathFromUtf8(const char* value)
{
    const char* statusText;
    if (value)
    {
        statusText = value;
    }
    else
    {
        statusText = "";
    }
    return filesystem::u8path(statusText);
}

inline string ModelPathToUtf8(const filesystem::path& path)
{
    const auto value = path.u8string();
    return string(reinterpret_cast<const char*>(value.data()), value.size());
}

inline string ModelPathLowerExtension(const filesystem::path& path)
{
    string extension = ModelPathToUtf8(path.extension());
    transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c));
    });
    return extension;
}

inline const aiScene* ImportModelScene(const char* fileName, unsigned int flags)
{
    const filesystem::path path = ModelPathFromUtf8(fileName);
    const string extension = ModelPathLowerExtension(path);
    const auto fastgltf = std::getenv("DX12_USE_FASTGLTF");
    const bool gltfInput = extension == ".gltf" || extension == ".glb" || extension == ".vrm";
    const bool disableFast = fastgltf && std::strcmp(fastgltf, "0") == 0;
    const bool preferFast =
        gltfInput && !disableFast && (FastGltfImport::Available() || (fastgltf && std::strcmp(fastgltf, "1") == 0));
    auto tryFast = [&]() -> const aiScene* {
        char error[512]{};
        if (const auto scene = FastGltfImport::Load(fileName, flags, error, sizeof(error)))
        {
            return scene;
        }
        std::fprintf(stderr, "fastgltf fallback: %s: %s\n", fileName, error);
        return nullptr;
    };
    if (preferFast)
    {
        if (const auto scene = tryFast())
        {
            return scene;
        }
    }
    const char* formatHint = nullptr;
    if (extension == ".vrm")
    {
        formatHint = "glb";
    }
    else if (extension == ".pmx")
    {
        formatHint = "pmx";
    }
    else if (extension == ".vmd")
    {
        formatHint = "vmd";
    }

    if (!formatHint)
    {
        return aiImportFile(fileName, flags);
    }

    ifstream stream(path, ios::binary | ios::ate);
    if (!stream)
    {
        return nullptr;
    }

    const streamsize size = stream.tellg();
    if (size <= 0)
    {
        return nullptr;
    }

    vector<char> bytes(static_cast<size_t>(size));
    stream.seekg(0, ios::beg);
    if (!stream.read(bytes.data(), size))
    {
        return nullptr;
    }

    return aiImportFileFromMemory(bytes.data(), static_cast<unsigned int>(bytes.size()), flags, formatHint);
}

inline void ReleaseModelScene(const aiScene* scene)
{
    if (!FastGltfImport::Release(scene))
    {
        aiReleaseImport(scene);
    }
}
