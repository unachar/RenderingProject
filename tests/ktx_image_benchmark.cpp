#define NOMINMAX
#include "../source/rendering/ktxbasis.h"
#include "../source/assets/texturecompression.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace
{
    using Clock = std::chrono::steady_clock;

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    double Milliseconds(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    std::vector<uint8_t> Read(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        const auto size = stream.tellg();
        Require(bool(stream) && size > 0 && size < std::streamoff(KtxBasis::MaxImageBytes), "DDS input length");
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        Require(bool(stream), "DDS input read");
        return bytes;
    }

    void Equal(const DirectX::ScratchImage& first, const DirectX::ScratchImage& second)
    {
        const auto& a = first.GetMetadata();
        const auto& b = second.GetMetadata();
        Require(a.width == b.width && a.height == b.height && a.depth == b.depth && a.arraySize == b.arraySize &&
                    a.mipLevels == b.mipLevels && a.format == b.format && a.dimension == b.dimension &&
                    a.miscFlags == b.miscFlags && a.miscFlags2 == b.miscFlags2 &&
                    first.GetPixelsSize() == second.GetPixelsSize() &&
                    std::memcmp(first.GetPixels(), second.GetPixels(), first.GetPixelsSize()) == 0,
                "native image mismatch");
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        const std::filesystem::path directory = argc > 1 ? argv[1] : L"Data/Cache/Textures";
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".dds")
            {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
        Require(!paths.empty(), "no DDS inputs");
        uint64_t rawBytes = 0, nativeBytes = 0, zstdBytes = 0, pixelBytes = 0;
        double nativeEncode = 0, zstdEncode = 0;
        double passes[3][5]{};
        std::cout
            << "{\"scope\":\"Resident-buffer CPU decoding including allocations; 5 rotating-order passes. All native pixels and metadata compared outside timing. No file I/O, payload hash, GPU, process-memory or FPS comparison.\",\"files\":[\n";
        size_t count = 0;
        for (const auto& path : paths)
        {
            const auto dds = Read(path);
            DirectX::ScratchImage reference;
            DirectX::TexMetadata metadata{};
            Require(SUCCEEDED(DirectX::LoadFromDDSMemory(dds.data(), dds.size(), DirectX::DDS_FLAGS_NONE, &metadata,
                                                         reference)),
                    "DDS source decode");
            std::vector<uint8_t> native, zstd;
            auto start = Clock::now();
            Require(SUCCEEDED(KtxBasis::Encode(reference, false, native)), "native KTX2 encode");
            nativeEncode += Milliseconds(start);
            start = Clock::now();
            Require(SUCCEEDED(KtxBasis::Encode(reference, true, zstd)), "zstd KTX2 encode");
            zstdEncode += Milliseconds(start);
            for (unsigned pass = 0; pass < 5; ++pass)
            {
                for (unsigned phase = 0; phase < 3; ++phase)
                {
                    const unsigned mode = (pass + phase) % 3;
                    DirectX::ScratchImage decoded;
                    DirectX::TexMetadata decodedMetadata{};
                    start = Clock::now();
                    HRESULT result;
                    if (mode == 0)
                    {
                        result = DirectX::LoadFromDDSMemory(dds.data(), dds.size(), DirectX::DDS_FLAGS_NONE,
                                                            &decodedMetadata, decoded);
                    }
                    else
                    {
                        const auto& encoded = mode == 1 ? native : zstd;
                        result = KtxBasis::Load(encoded.data(), encoded.size(), TextureUsage::Color, decodedMetadata,
                                                decoded);
                    }
                    passes[mode][pass] += Milliseconds(start);
                    Require(SUCCEEDED(result), "timed decode failed");
                    Equal(reference, decoded);
                }
            }
            rawBytes += dds.size();
            nativeBytes += native.size();
            zstdBytes += zstd.size();
            pixelBytes += reference.GetPixelsSize();
            if (count++)
            {
                std::cout << ",\n";
            }
            std::cout << "{\"name\":" << std::quoted(path.filename().string()) << ",\"ddsBytes\":" << dds.size()
                      << ",\"nativeKtxBytes\":" << native.size() << ",\"zstdKtxBytes\":" << zstd.size() << '}';
        }
        std::cout << "\n],\"fileCount\":" << count << ",\"ddsBytes\":" << rawBytes
                  << ",\"nativeKtxBytes\":" << nativeBytes << ",\"zstdKtxBytes\":" << zstdBytes
                  << ",\"pixelBytes\":" << pixelBytes << ",\"nativeEncodeMs\":" << nativeEncode
                  << ",\"zstdEncodeMs\":" << zstdEncode << ",\"passes\":[";
        for (unsigned pass = 0; pass < 5; ++pass)
        {
            if (pass)
            {
                std::cout << ',';
            }
            std::cout << "{\"ddsMs\":" << passes[0][pass] << ",\"nativeKtxMs\":" << passes[1][pass]
                      << ",\"zstdKtxMs\":" << passes[2][pass] << '}';
        }
        std::cout << "]}\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
