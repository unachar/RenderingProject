#define NOMINMAX
#include "../source/assets/texturecachestorage.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Bytes = std::vector<uint8_t>;
    void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    double Milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
    Bytes Read(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        Require(static_cast<bool>(stream), "benchmark file open");
        const std::streamoff size = stream.tellg();
        Require(size >= 0 && size <= static_cast<std::streamoff>(TextureCacheCodec::MaxDdsBytes + 32), "benchmark file cap");
        Bytes result(static_cast<size_t>(size));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
        Require(static_cast<bool>(stream), "benchmark file read");
        return result;
    }
    bool Equal(const DirectX::ScratchImage& a, const DirectX::ScratchImage& b)
    {
        const auto& x = a.GetMetadata(); const auto& y = b.GetMetadata();
        return x.format == y.format && x.width == y.width && x.height == y.height && x.depth == y.depth &&
            x.arraySize == y.arraySize && x.mipLevels == y.mipLevels && x.miscFlags == y.miscFlags &&
            x.miscFlags2 == y.miscFlags2 && x.dimension == y.dimension && a.GetImageCount() == b.GetImageCount() &&
            a.GetPixelsSize() == b.GetPixelsSize() && std::memcmp(a.GetPixels(), b.GetPixels(), a.GetPixelsSize()) == 0;
    }
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        Require(argc == 3, "usage: benchmark <legacy-DDS-directory> <new-output-directory>");
        const std::filesystem::path source = std::filesystem::absolute(argv[1]);
        const std::filesystem::path destination = std::filesystem::absolute(argv[2]);
        Require(!std::filesystem::exists(destination), "benchmark output must be new");
        const auto input = destination / "input", output = destination / "stored";
        std::filesystem::create_directories(input);
        std::filesystem::create_directories(output);
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(source))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".dds") continue;
            const auto copied = input / entry.path().filename();
            Require(std::filesystem::copy_file(entry.path(), copied), "DDS snapshot copy");
            files.push_back(copied);
        }
        std::sort(files.begin(), files.end());
        Require(!files.empty(), "no legacy DDS inputs");
        std::ofstream report(destination / "report.json");
        report << "{\"scope\":\"Real storage helper over an immutable DDS snapshot. Warm file-load means are three alternating-order passes. CPU codec decode includes allocation/hash but excludes I/O. Buffer budgets exclude allocator/zstd/DirectXTex overhead; not process peak or startup/FPS evidence.\",\n\"files\":[\n";
        uint64_t rawBytes = 0, storedBytes = 0, bcBytes = 0, compressedCount = 0;
        uint64_t loadBufferBudget = 0, storeBufferBudget = 0;
        double storeMs = 0, rawReadMs = 0, storedReadMs = 0, decodeCpuMs = 0;
        double baselinePass[3]{}, storedPass[3]{};
        size_t index = 0;
        for (const auto& file : files)
        {
            DirectX::ScratchImage image;
            DirectX::TexMetadata metadata{};
            Require(SUCCEEDED(DirectX::LoadFromDDSFile(file.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image)), "snapshot DDS load");
            const auto target = output / file.filename();
            auto start = Clock::now();
            Require(TextureCacheStorage::Store(target, image), "product storage helper write");
            const double writeMs = Milliseconds(start);
            storeMs += writeMs;
            const auto sidecar = TextureCacheStorage::CompressedPath(target);
            const bool compressed = std::filesystem::exists(sidecar);
            const auto savedPath = compressed ? sidecar : target;
            start = Clock::now(); const Bytes originalFile = Read(file); rawReadMs += Milliseconds(start);
            start = Clock::now(); const Bytes storedFile = Read(savedPath); storedReadMs += Milliseconds(start);
            DirectX::Blob serialized;
            Require(SUCCEEDED(DirectX::SaveToDDSMemory(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::DDS_FLAGS_NONE, serialized)), "expected DDS serialization");
            double fileDecodeCpu = 0;
            if (compressed)
            {
                Require(!std::filesystem::exists(target), "beneficial save should not duplicate legacy output");
                std::array<uint64_t, 4> words{};
                Require(storedFile.size() >= sizeof(words), "sidecar header length");
                std::memcpy(words.data(), storedFile.data(), sizeof(words));
                TextureCacheCodec::Header header;
                Require(TextureCacheCodec::ParseHeader(words, storedFile.size(), header), "stored header validation");
                const Bytes payload(storedFile.begin() + 32, storedFile.end());
                for (int repeat = 0; repeat < 3; ++repeat)
                {
                    Bytes decoded;
                    start = Clock::now();
                    const auto result = TextureCacheCodec::Decode(header, payload, decoded);
                    fileDecodeCpu += Milliseconds(start);
                    Require(result == TextureCacheCodec::DecodeResult::Success && decoded.size() == serialized.GetBufferSize() &&
                            std::memcmp(decoded.data(), serialized.GetBufferPointer(), decoded.size()) == 0, "whole DDS codec byte mismatch");
                }
                loadBufferBudget = std::max(loadBufferBudget, std::max(header.storedBytes + header.rawBytes,
                    header.rawBytes + image.GetPixelsSize()));
                storeBufferBudget = std::max(storeBufferBudget, static_cast<uint64_t>(image.GetPixelsSize() +
                    serialized.GetBufferSize() + ZSTD_compressBound(serialized.GetBufferSize())));
            }
            decodeCpuMs += fileDecodeCpu / 3;
            for (int pass = 0; pass < 3; ++pass)
            {
                for (int phase = 0; phase < 2; ++phase)
                {
                    const bool baseline = ((pass + phase) % 2) == 0;
                    DirectX::ScratchImage candidate;
                    DirectX::TexMetadata candidateMetadata{};
                    start = Clock::now();
                    const bool loaded = baseline ?
                        SUCCEEDED(DirectX::LoadFromDDSFile(file.c_str(), DirectX::DDS_FLAGS_NONE, &candidateMetadata, candidate)) :
                        TextureCacheStorage::Load(target, candidateMetadata, candidate);
                    (baseline ? baselinePass[pass] : storedPass[pass]) += Milliseconds(start);
                    Require(loaded && Equal(image, candidate), "product helper metadata/BC mismatch");
                }
            }
            rawBytes += originalFile.size(); storedBytes += storedFile.size(); bcBytes += image.GetPixelsSize();
            compressedCount += compressed ? 1 : 0;
            if (index++) report << ",\n";
            report << "{\"name\":" << std::quoted(file.filename().string()) << ",\"format\":" << metadata.format
                << ",\"width\":" << metadata.width << ",\"height\":" << metadata.height << ",\"mips\":" << metadata.mipLevels
                << ",\"rawBytes\":" << originalFile.size() << ",\"storedBytes\":" << storedFile.size()
                << ",\"sidecar\":" << (compressed ? "true" : "false") << ",\"storeMs\":" << writeMs
                << ",\"codecDecodeCpuMsMean\":" << fileDecodeCpu / 3 << '}';
        }
        report << "\n],\n\"fileCount\":" << files.size() << ",\"compressedFiles\":" << compressedCount
            << ",\"rawFallbackFiles\":" << files.size() - compressedCount << ",\"rawFileBytes\":" << rawBytes
            << ",\"storedFileBytes\":" << storedBytes << ",\"bcBytes\":" << bcBytes
            << ",\"savedPercent\":" << 100 * (1 - double(storedBytes) / double(rawBytes))
            << ",\"storeIncludingIoMs\":" << storeMs << ",\"rawReadFileMs\":" << rawReadMs
            << ",\"storedReadFileMs\":" << storedReadMs << ",\"codecDecodeCpuMsMean\":" << decodeCpuMs
            << ",\"maxLoadBufferBudgetBytes\":" << loadBufferBudget << ",\"maxStoreBufferBudgetBytes\":" << storeBufferBudget
            << ",\"warmPasses\":[";
        for (int pass = 0; pass < 3; ++pass)
        {
            if (pass) report << ',';
            report << "{\"baselineMs\":" << baselinePass[pass] << ",\"storedMs\":" << storedPass[pass] << '}';
        }
        report << "]}\n";
        report.close();
        Require(!report.fail(), "benchmark report write");
        std::cout << "PASS " << files.size() << " exact DDS/BC loads; " << rawBytes << " -> " << storedBytes
                  << " bytes, " << compressedCount << " sidecars\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
