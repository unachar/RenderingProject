#pragma once

#include <cstdint>
#include <array>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "shadercachecodec.h"

// プリプロセス済みソースには間接的な include の内容も含まれる。DXIL の再利用前に、
// コンパイラ引数とコンパイラ実行ファイルの識別情報も合わせてハッシュ化する。
namespace DxcShaderCache
{
    inline uint64_t Hash(const void* data, size_t size, uint64_t hash = 14695981039346656037ull)
    {
        return ShaderCacheCodec::Hash(data, size, hash);
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
                                     static_cast<UINT32>(preprocessArguments.size()), includes,
                                     IID_PPV_ARGS(&result))) ||
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
        object.clear();
        diagnostics.clear();
        if (path.empty())
        {
            return false;
        }
        try
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            const std::streamoff fileSize = stream.tellg();
            if (fileSize < 32 || fileSize > static_cast<std::streamoff>(
                ShaderCacheCodec::MaxObjectBytes + ShaderCacheCodec::MaxDiagnosticBytes + 48)) return false;
            std::array<uint64_t, 6> words{};
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(words.data()), 32);
            if (words[0] == ShaderCacheCodec::MagicV2)
                stream.read(reinterpret_cast<char*>(words.data() + 4), 16);
            ShaderCacheCodec::Header header;
            if (!stream || !ShaderCacheCodec::ParseHeader(words, static_cast<uint64_t>(fileSize), header))
                return false;
            std::vector<char> payload(static_cast<size_t>(header.storedBytes));
            stream.read(payload.data(), static_cast<std::streamsize>(payload.size()));
            return stream && ShaderCacheCodec::DecodePayload(header, payload, object, diagnostics);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }

    inline void Store(const std::filesystem::path& path, const void* object, size_t size,
                      const std::string& diagnostics)
    {
        if (path.empty() || !object || !ShaderCacheCodec::ValidSizes(size, diagnostics.size()))
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
        std::vector<char> compressed;
        const bool useCompression = ShaderCacheCodec::TryCompress(object, size, diagnostics, compressed);
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            const uint64_t checksum = Hash(diagnostics.data(), diagnostics.size(), Hash(object, size));
            if (useCompression)
            {
                const uint64_t header[] = {ShaderCacheCodec::MagicV2, 1, size, diagnostics.size(),
                                           compressed.size(), checksum};
                stream.write(reinterpret_cast<const char*>(header), sizeof(header));
                stream.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
            }
            else
            {
                const uint64_t header[] = {ShaderCacheCodec::MagicV1, size, diagnostics.size(), checksum};
                stream.write(reinterpret_cast<const char*>(header), sizeof(header));
                stream.write(static_cast<const char*>(object), static_cast<std::streamsize>(size));
                stream.write(diagnostics.data(), static_cast<std::streamsize>(diagnostics.size()));
            }
            stream.close();
            if (stream.fail())
            {
                std::filesystem::remove(temporary, error);
                return;
            }
        }
        // 別プロセスからも、完全なファイルかキャッシュミスのどちらかだけが見える。
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            std::filesystem::remove(temporary, error);
        }
    }
} // 名前空間 DxcShaderCache の終わり。
