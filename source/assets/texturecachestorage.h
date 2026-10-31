#pragma once
#include <windows.h>
#include "DirectXTex.h"
#include "texturecachecodec.h"
#include <filesystem>
#include <fstream>
#include <string>

namespace TextureCacheStorage
{
    inline std::filesystem::path CompressedPath(const std::filesystem::path& rawPath)
    {
        return rawPath.wstring() + L".zst";
    }

    inline std::wstring TemporaryPath(const std::filesystem::path& target)
    {
        return target.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
               std::to_wstring(GetCurrentThreadId()) + L".tmp";
    }

    inline bool StoreCompressed(const std::filesystem::path& rawPath, const DirectX::ScratchImage& image)
    {
        // Avoid allocating a DDS staging copy for targets that will use legacy storage.
        if (image.GetPixelsSize() > TextureCacheCodec::MaxDdsBytes - 148) return false;
        DirectX::Blob dds;
        if (FAILED(DirectX::SaveToDDSMemory(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                                           DirectX::DDS_FLAGS_NONE, dds))) return false;
        std::vector<uint8_t> payload;
        TextureCacheCodec::Header header;
        if (!TextureCacheCodec::TryEncode(dds.GetBufferPointer(), dds.GetBufferSize(), payload, header)) return false;
        const auto target = CompressedPath(rawPath);
        const auto temporary = TemporaryPath(target);
        std::error_code error;
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            const uint64_t words[] = {TextureCacheCodec::Magic, header.rawBytes, header.storedBytes, header.checksum};
            stream.write(reinterpret_cast<const char*>(words), sizeof(words));
            stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            stream.close();
            if (stream.fail()) { std::filesystem::remove(temporary, error); return false; }
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }

    inline bool StoreImpl(const std::filesystem::path& rawPath, const DirectX::ScratchImage& image)
    {
        if (rawPath.empty() || image.GetImageCount() == 0) return false;
        std::error_code error;
        if (!rawPath.parent_path().empty()) std::filesystem::create_directories(rawPath.parent_path(), error);
        if (error) return false;
        try { if (StoreCompressed(rawPath, image)) return true; }
        catch (const std::bad_alloc&) { /* The legacy file writer does not need a DDS staging copy. */ }
        const auto temporary = TemporaryPath(rawPath);
        if (FAILED(DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                                         DirectX::DDS_FLAGS_NONE, temporary.c_str())) ||
            !MoveFileExW(temporary.c_str(), rawPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }

    enum class ReadResult { Success, Missing, Corrupt, ResourceFailure };

    inline ReadResult ReadCompressedDds(const std::filesystem::path& path, std::vector<uint8_t>& dds)
    {
        dds.clear();
        try
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) return ReadResult::Missing;
            const std::streamoff fileBytes = stream.tellg();
            if (fileBytes < static_cast<std::streamoff>(TextureCacheCodec::HeaderBytes) ||
                fileBytes > static_cast<std::streamoff>(TextureCacheCodec::MaxDdsBytes + TextureCacheCodec::HeaderBytes))
                return ReadResult::Corrupt;
            std::array<uint64_t, 4> words{};
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(words.data()), sizeof(words));
            TextureCacheCodec::Header header;
            if (!stream || !TextureCacheCodec::ParseHeader(words, static_cast<uint64_t>(fileBytes), header))
                return ReadResult::Corrupt;
            std::vector<uint8_t> payload(static_cast<size_t>(header.storedBytes));
            stream.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            if (!stream) return ReadResult::ResourceFailure;
            switch (TextureCacheCodec::Decode(header, payload, dds))
            {
            case TextureCacheCodec::DecodeResult::Success: return ReadResult::Success;
            case TextureCacheCodec::DecodeResult::AllocationFailure: return ReadResult::ResourceFailure;
            default: return ReadResult::Corrupt;
            }
        }
        catch (const std::bad_alloc&) { return ReadResult::ResourceFailure; }
    }

    inline bool LoadImpl(const std::filesystem::path& rawPath, DirectX::TexMetadata& metadata,
                     DirectX::ScratchImage& image)
    {
        if (rawPath.empty()) return false;
        DirectX::TexMetadata candidateMetadata{};
        DirectX::ScratchImage candidate;
        std::error_code error;
        try
        {
            const auto sidecar = CompressedPath(rawPath);
            std::vector<uint8_t> dds;
            ReadResult result = ReadCompressedDds(sidecar, dds);
            // The encoded payload is released by ReadCompressedDds before the DDS/ScratchImage overlap.
            if (result == ReadResult::Success)
            {
                const HRESULT loaded = DirectX::LoadFromDDSMemory(dds.data(), dds.size(), DirectX::DDS_FLAGS_NONE,
                                                                   &candidateMetadata, candidate);
                if (SUCCEEDED(loaded) && DirectX::IsCompressed(candidateMetadata.format))
                {
                    metadata = candidateMetadata;
                    image = std::move(candidate);
                    return true;
                }
                result = loaded == E_OUTOFMEMORY ? ReadResult::ResourceFailure : ReadResult::Corrupt;
            }
            if (result == ReadResult::Corrupt) std::filesystem::remove(sidecar, error);
        }
        catch (const std::bad_alloc&) { /* Try the existing DDS without a decode buffer. */ }
        if (!std::filesystem::exists(rawPath, error)) return false;
        const HRESULT loaded = DirectX::LoadFromDDSFile(rawPath.c_str(), DirectX::DDS_FLAGS_NONE, &candidateMetadata, candidate);
        if (FAILED(loaded) ||
            !DirectX::IsCompressed(candidateMetadata.format))
        {
            if (loaded != E_OUTOFMEMORY) std::filesystem::remove(rawPath, error);
            return false;
        }
        metadata = candidateMetadata;
        image = std::move(candidate);
        return true;
    }

    inline bool Store(const std::filesystem::path& rawPath, const DirectX::ScratchImage& image)
    {
        try { return StoreImpl(rawPath, image); }
        catch (const std::bad_alloc&) { return false; }
    }
    inline bool Load(const std::filesystem::path& rawPath, DirectX::TexMetadata& metadata, DirectX::ScratchImage& image)
    {
        try { return LoadImpl(rawPath, metadata, image); }
        catch (const std::bad_alloc&) { return false; }
    }
}
