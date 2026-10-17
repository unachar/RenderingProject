#pragma once

#include <cstdint>
#include <array>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Preprocessed source includes the actual transitive include contents. Hash it
// with the compiler arguments and compiler binary identity before reusing DXIL.
namespace DxcShaderCache
{
inline uint64_t Hash(const void* data, size_t size, uint64_t hash = 14695981039346656037ull)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::wstring CompilerIdentity()
{
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(GetModuleHandleW(L"dxcompiler.dll"), modulePath, MAX_PATH))
    {
        return {};
    }
    std::ifstream stream(modulePath, std::ios::binary);
    if (!stream)
    {
        return {};
    }
    uint64_t hash = 14695981039346656037ull;
    std::array<char, 65536> buffer;
    while (stream)
    {
        stream.read(buffer.data(), buffer.size());
        hash = Hash(buffer.data(), static_cast<size_t>(stream.gcount()), hash);
    }
    if (!stream.eof())
    {
        return {};
    }
    return std::to_wstring(hash);
}

inline std::filesystem::path CachePath(IDxcCompiler3* compiler, IDxcIncludeHandler* includes,
    const DxcBuffer& source, const std::vector<LPCWSTR>& arguments)
{
    char disabled[8]{};
    if (GetEnvironmentVariableA("DX12_DISABLE_SHADER_CACHE", disabled, sizeof(disabled)) > 0 && atoi(disabled))
    {
        return {};
    }
    static const std::wstring identity = CompilerIdentity();
    if (identity.empty())
    {
        return {};
    }
    auto preprocessArguments = arguments;
    preprocessArguments.push_back(L"-P");
    Microsoft::WRL::ComPtr<IDxcResult> result;
    HRESULT status = E_FAIL;
    if (FAILED(compiler->Compile(&source, preprocessArguments.data(),
        static_cast<UINT32>(preprocessArguments.size()), includes, IID_PPV_ARGS(&result))) ||
        !result || FAILED(result->GetStatus(&status)) || FAILED(status))
    {
        return {};
    }
    Microsoft::WRL::ComPtr<IDxcBlobUtf8> preprocessed;
    if (FAILED(result->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(&preprocessed), nullptr)) || !preprocessed)
    {
        return {};
    }
    uint64_t hash = Hash(identity.data(), identity.size() * sizeof(wchar_t));
    hash = Hash(source.Ptr, source.Size, hash);
    for (const wchar_t* argument : arguments)
    {
        hash = Hash(argument, (wcslen(argument) + 1) * sizeof(wchar_t), hash);
    }
    hash = Hash(preprocessed->GetBufferPointer(), preprocessed->GetBufferSize(), hash);
    Microsoft::WRL::ComPtr<IDxcBlobUtf8> diagnostics;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnostics), nullptr)) && diagnostics)
    {
        hash = Hash(diagnostics->GetBufferPointer(), diagnostics->GetBufferSize(), hash);
    }
    return std::filesystem::path("EditorCache/Shaders") / (std::to_string(hash) + ".bin");
}

inline bool Load(const std::filesystem::path& path, std::vector<char>& object, std::string& diagnostics)
{
    if (path.empty())
    {
        return false;
    }
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    // Bound allocations and reject truncated/old cache formats before reading.
    constexpr uint64_t magic = 0x3143584443484143ull;
    uint64_t header[4]{};
    const auto fileSize = stream.tellg();
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!stream || header[0] != magic || header[1] > 64 * 1024 * 1024 ||
        header[2] > 1024 * 1024 || fileSize != sizeof(header) + header[1] + header[2])
    {
        return false;
    }
    object.resize(static_cast<size_t>(header[1]));
    diagnostics.resize(static_cast<size_t>(header[2]));
    stream.read(object.data(), static_cast<std::streamsize>(object.size()));
    stream.read(diagnostics.data(), static_cast<std::streamsize>(diagnostics.size()));
    return stream && !object.empty() &&
        Hash(diagnostics.data(), diagnostics.size(), Hash(object.data(), object.size())) == header[3];
}

inline void Store(const std::filesystem::path& path, const void* object, size_t size, const std::string& diagnostics)
{
    if (path.empty())
    {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        return;
    }
    const auto temporary = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        const uint64_t header[] = {0x3143584443484143ull, size, diagnostics.size(),
            Hash(diagnostics.data(), diagnostics.size(), Hash(object, size))};
        stream.write(reinterpret_cast<const char*>(header), sizeof(header));
        stream.write(static_cast<const char*>(object), static_cast<std::streamsize>(size));
        stream.write(diagnostics.data(), static_cast<std::streamsize>(diagnostics.size()));
        stream.close();
        if (stream.fail())
        {
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    // Readers see either a complete file or a cache miss, including across processes.
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        std::filesystem::remove(temporary, error);
    }
}
}
