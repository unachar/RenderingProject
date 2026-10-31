// Read-only experiment: zstd over existing DDS files and their BC mip payloads.
// This does not write a KTX2 container or change any runtime texture/cache path.
#include <DirectXTex.h>
#include "../External/zstd/zstd.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Bytes = std::vector<char>;
    struct Measurement
    {
        uint64_t storedBytes = 0;
        double compressMs = 0;
        double decompressMs = 0;
    };
    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    double Milliseconds(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    Measurement Measure(const void* source, size_t size, int level)
    {
        Require(size > 0, "empty probe payload");
        Bytes encoded(ZSTD_compressBound(size)), decoded(size);
        const auto encodeStart = Clock::now();
        const size_t stored = ZSTD_compress(encoded.data(), encoded.size(), source, size, level);
        const double encodeMs = Milliseconds(encodeStart);
        Require(!ZSTD_isError(stored), "zstd compression failed");
        double decodeMs = 0;
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            const auto decodeStart = Clock::now();
            const size_t restored = ZSTD_decompress(decoded.data(), decoded.size(), encoded.data(), stored);
            decodeMs += Milliseconds(decodeStart);
            Require(!ZSTD_isError(restored) && restored == size && std::memcmp(source, decoded.data(), size) == 0,
                    "zstd roundtrip changed texture bytes");
        }
        return {stored, encodeMs, decodeMs / 3};
    }
    Bytes Read(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        const std::streamoff length = stream.tellg();
        Require(stream && length >= 128 && length <= 256 * 1024 * 1024, "invalid/oversized probe DDS");
        Bytes bytes(static_cast<size_t>(length));
        stream.seekg(0);
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Require(static_cast<bool>(stream), "DDS read failed");
        return bytes;
    }
    const char* FormatName(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_BC4_UNORM: return "BC4_UNORM";
        case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
        case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
        default: throw std::runtime_error("DDS format outside this experiment's BC4/5/7 scope");
        }
    }
    void WriteMeasurement(std::ostream& stream, const Measurement& value)
    {
        stream << "{\"storedBytes\":" << value.storedBytes << ",\"compressMs\":" << value.compressMs
               << ",\"decompressMsMean\":" << value.decompressMs << '}';
    }

    double MeasureDdsMemoryLoad(const Bytes& dds, const DirectX::ScratchImage& expected, int level)
    {
        Bytes encoded, decoded;
        size_t encodedBytes = 0;
        if (level != 0)
        {
            encoded.resize(ZSTD_compressBound(dds.size()));
            encodedBytes = ZSTD_compress(encoded.data(), encoded.size(), dds.data(), dds.size(), level);
            Require(!ZSTD_isError(encodedBytes), "DDS load fixture compression failed");
            decoded.resize(dds.size());
        }
        double elapsedMs = 0;
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            DirectX::ScratchImage loaded;
            DirectX::TexMetadata metadata{};
            const auto start = Clock::now();
            const void* source = dds.data();
            if (level != 0)
            {
                const size_t restored = ZSTD_decompress(decoded.data(), decoded.size(), encoded.data(), encodedBytes);
                Require(!ZSTD_isError(restored) && restored == dds.size(), "DDS load fixture decode failed");
                source = decoded.data();
            }
            const HRESULT result = DirectX::LoadFromDDSMemory(static_cast<const uint8_t*>(source), dds.size(),
                                                             DirectX::DDS_FLAGS_NONE, &metadata, loaded);
            elapsedMs += Milliseconds(start);
            const auto& reference = expected.GetMetadata();
            Require(SUCCEEDED(result) && metadata.format == reference.format && metadata.width == reference.width &&
                    metadata.height == reference.height && metadata.mipLevels == reference.mipLevels &&
                    loaded.GetImageCount() == expected.GetImageCount() && loaded.GetPixelsSize() == expected.GetPixelsSize() &&
                    std::memcmp(loaded.GetPixels(), expected.GetPixels(), loaded.GetPixelsSize()) == 0,
                    "DDS memory load changed metadata or BC bytes");
        }
        return elapsedMs / 3;
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        Require(argc == 3, "usage: texture_storage_probe <DDS-directory> <new-report.json>");
        const std::filesystem::path directory = std::filesystem::absolute(argv[1]);
        const std::filesystem::path reportPath = std::filesystem::absolute(argv[2]);
        Require(!std::filesystem::exists(reportPath), "report must be a new file");
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.is_regular_file() && entry.path().extension() == ".dds") paths.push_back(entry.path());
        std::sort(paths.begin(), paths.end());
        Require(!paths.empty(), "no DDS files");
        std::ofstream report(reportPath, std::ios::binary);
        Require(static_cast<bool>(report), "report open failed");
        report << "{\n\"version\":" << std::quoted(ZSTD_versionString()) << ",\n\"decodeRepeats\":3,\n"
            << "\"scope\":\"Read-only DDS/BC payload experiment. No KTX2 headers/index/DFD, application startup, GPU or FPS measurement. Codec CPU times exclude I/O/buffer allocation/byte comparison; DDS memory-load times include ScratchImage allocation but exclude input/decode-buffer allocation and I/O.\",\n\"files\":[\n";
        uint64_t totalBytes = 0, totalPayloadBytes = 0;
        size_t fileIndex = 0;
        for (const auto& path : paths)
        {
            const auto timestamp = std::filesystem::last_write_time(path);
            const Bytes dds = Read(path);
            DirectX::TexMetadata metadata{};
            DirectX::ScratchImage image;
            Require(SUCCEEDED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image)),
                    "DirectXTex DDS load failed");
            Require(metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE2D && metadata.arraySize == 1 &&
                    metadata.depth == 1 && !metadata.IsCubemap() && image.GetImageCount() == metadata.mipLevels,
                    "probe requires single 2D textures");
            if (fileIndex++) report << ",\n";
            report << "{\"name\":" << std::quoted(path.filename().string()) << ",\"format\":"
                << std::quoted(FormatName(metadata.format)) << ",\"dxgiFormat\":" << metadata.format
                << ",\"width\":" << metadata.width << ",\"height\":" << metadata.height
                << ",\"mips\":" << metadata.mipLevels << ",\"ddsBytes\":" << dds.size()
                << ",\"bcPayloadBytes\":" << image.GetPixelsSize()
                << ",\"ddsMemoryLoadMsMean\":" << MeasureDdsMemoryLoad(dds, image, 0) << ",\"levels\":[";
            const int levels[] = {1, 3, 9};
            for (size_t index = 0; index < 3; ++index)
            {
                const auto whole = Measure(dds.data(), dds.size(), levels[index]);
                Measurement perMip;
                uint64_t sourceMipBytes = 0;
                for (size_t mip = 0; mip < metadata.mipLevels; ++mip)
                {
                    const auto& subresource = image.GetImages()[mip];
                    const auto value = Measure(subresource.pixels, subresource.slicePitch, levels[index]);
                    sourceMipBytes += subresource.slicePitch;
                    perMip.storedBytes += value.storedBytes;
                    perMip.compressMs += value.compressMs;
                    perMip.decompressMs += value.decompressMs;
                }
                Require(sourceMipBytes == image.GetPixelsSize(), "mip payload accounting mismatch");
                if (index) report << ',';
                report << "{\"level\":" << levels[index] << ",\"wholeDds\":";
                WriteMeasurement(report, whole);
                report << ",\"perMipBcFrames\":";
                WriteMeasurement(report, perMip);
                report << ",\"decodeAndDdsMemoryLoadMsMean\":" << MeasureDdsMemoryLoad(dds, image, levels[index]);
                report << '}';
            }
            report << "]}";
            Require(timestamp == std::filesystem::last_write_time(path) && dds.size() == std::filesystem::file_size(path),
                    "DDS changed during probe");
            totalBytes += dds.size();
            totalPayloadBytes += image.GetPixelsSize();
        }
        report << "\n],\n\"fileCount\":" << paths.size() << ",\n\"ddsBytes\":" << totalBytes
               << ",\n\"bcPayloadBytes\":" << totalPayloadBytes << "\n}\n";
        report.close();
        Require(!report.fail(), "report write failed");
        std::cout << "PASS " << paths.size() << " DDS files, " << totalBytes << " file bytes, "
                  << totalPayloadBytes << " BC bytes; levels1/3/9, whole-file and every-mip exact roundtrips\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
