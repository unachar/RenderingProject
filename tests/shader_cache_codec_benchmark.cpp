#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include "dxcshadercache.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Bytes = std::vector<char>;
    double Milliseconds(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    Bytes Read(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        Require(static_cast<bool>(stream), "open benchmark file");
        const auto size = stream.tellg();
        Require(size >= 0, "benchmark file length");
        Bytes bytes(static_cast<size_t>(size));
        stream.seekg(0);
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Require(static_cast<bool>(stream), "read benchmark file");
        return bytes;
    }
    void RawStore(const std::filesystem::path& path, const Bytes& object, const std::string& diagnostics)
    {
        const uint64_t header[] = {ShaderCacheCodec::MagicV1, object.size(), diagnostics.size(),
            DxcShaderCache::Hash(diagnostics.data(), diagnostics.size(), DxcShaderCache::Hash(object.data(), object.size()))};
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(header), sizeof(header));
        stream.write(object.data(), static_cast<std::streamsize>(object.size()));
        stream.write(diagnostics.data(), static_cast<std::streamsize>(diagnostics.size()));
        stream.close();
        Require(!stream.fail(), "raw baseline write");
    }
    ShaderCacheCodec::Header Header(const Bytes& bytes)
    {
        Require(bytes.size() >= 32, "header size");
        std::array<uint64_t, 6> words{};
        std::memcpy(words.data(), bytes.data(), 32);
        if (words[0] == ShaderCacheCodec::MagicV2)
        {
            Require(bytes.size() >= 48, "v2 header size");
            std::memcpy(words.data() + 4, bytes.data() + 32, 16);
        }
        ShaderCacheCodec::Header result;
        Require(ShaderCacheCodec::ParseHeader(words, bytes.size(), result), "benchmark header parse");
        return result;
    }
    // Exact pre-change v1 read path, for a fair comparison with the new Load.
    bool LegacyLoad(const std::filesystem::path& path, Bytes& object, std::string& diagnostics)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        const std::streamoff size = stream.tellg();
        uint64_t header[4]{};
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!stream || header[0] != ShaderCacheCodec::MagicV1 ||
            !ShaderCacheCodec::ValidSizes(header[1], header[2]) ||
            size != static_cast<std::streamoff>(32 + header[1] + header[2])) return false;
        object.resize(static_cast<size_t>(header[1]));
        diagnostics.resize(static_cast<size_t>(header[2]));
        stream.read(object.data(), static_cast<std::streamsize>(object.size()));
        stream.read(diagnostics.data(), static_cast<std::streamsize>(diagnostics.size()));
        return stream && DxcShaderCache::Hash(diagnostics.data(), diagnostics.size(),
            DxcShaderCache::Hash(object.data(), object.size())) == header[3];
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        Require(argc == 3, "usage: benchmark <source-cache> <new-output-directory>");
        const std::filesystem::path source = std::filesystem::absolute(argv[1]);
        const std::filesystem::path destination = std::filesystem::absolute(argv[2]);
        Require(!std::filesystem::exists(destination), "output must be a new isolated directory");
        std::filesystem::create_directories(destination / "raw");
        std::filesystem::create_directories(destination / "compressed");
        size_t count = 0, invalid = 0, compressedCount = 0;
        uint64_t inputBytes = 0, rawBytes = 0, storedBytes = 0;
        double storeMs = 0, compressionCpuMs = 0, rawReadMs = 0, compressedReadMs = 0;
        double rawDecodeMs = 0, compressedDecodeMs = 0, legacyLoadMs = 0, newLoadMs = 0, rawNewLoadMs = 0;
        double legacyPass[3]{}, newPass[3]{}, rawNewPass[3]{};
        Bytes object, decoded;
        std::string diagnostics, decodedDiagnostics;
        for (const auto& entry : std::filesystem::directory_iterator(source))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".bin") continue;
            if (!DxcShaderCache::Load(entry.path(), object, diagnostics)) { ++invalid; continue; }
            const auto rawPath = destination / "raw" / entry.path().filename();
            const auto compressedPath = destination / "compressed" / entry.path().filename();
            RawStore(rawPath, object, diagnostics);
            auto start = Clock::now();
            DxcShaderCache::Store(compressedPath, object.data(), object.size(), diagnostics);
            storeMs += Milliseconds(start);
            start = Clock::now();
            std::vector<char> candidate;
            ShaderCacheCodec::TryCompress(object.data(), object.size(), diagnostics, candidate);
            compressionCpuMs += Milliseconds(start);
            start = Clock::now();
            const Bytes rawFile = Read(rawPath);
            rawReadMs += Milliseconds(start);
            start = Clock::now();
            const Bytes compressedFile = Read(compressedPath);
            compressedReadMs += Milliseconds(start);
            inputBytes += entry.file_size();
            rawBytes += rawFile.size();
            storedBytes += compressedFile.size();
            const auto rawHeader = Header(rawFile), compressedHeader = Header(compressedFile);
            compressedCount += compressedHeader.compressed ? 1 : 0;
            const Bytes rawPayload(rawFile.begin() + static_cast<size_t>(rawHeader.headerBytes), rawFile.end());
            const Bytes compressedPayload(compressedFile.begin() + static_cast<size_t>(compressedHeader.headerBytes), compressedFile.end());
            for (int pass = 0; pass < 3; ++pass)
            {
                // Alternate order to reduce consistent first/second-call bias. These are warm-cache reads.
                for (int phase = 0; phase < 2; ++phase)
                {
                    const bool legacy = ((pass + phase) % 2) == 0;
                    start = Clock::now();
                    const bool loaded = legacy ? LegacyLoad(rawPath, decoded, decodedDiagnostics) :
                        DxcShaderCache::Load(compressedPath, decoded, decodedDiagnostics);
                    (legacy ? legacyPass[pass] : newPass[pass]) += Milliseconds(start);
                    Require(loaded && decoded == object && decodedDiagnostics == diagnostics, "native load mismatch");
                }
                start = Clock::now();
                const bool loaded = DxcShaderCache::Load(rawPath, decoded, decodedDiagnostics);
                rawNewPass[pass] += Milliseconds(start);
                Require(loaded && decoded == object && decodedDiagnostics == diagnostics, "v1 compatibility mismatch");
                start = Clock::now();
                const bool rawDecoded = ShaderCacheCodec::DecodePayload(rawHeader, rawPayload, decoded, decodedDiagnostics);
                rawDecodeMs += Milliseconds(start);
                Require(rawDecoded && decoded == object && decodedDiagnostics == diagnostics, "raw CPU decode mismatch");
                start = Clock::now();
                const bool compressedDecoded = ShaderCacheCodec::DecodePayload(compressedHeader, compressedPayload, decoded, decodedDiagnostics);
                compressedDecodeMs += Milliseconds(start);
                Require(compressedDecoded && decoded == object && decodedDiagnostics == diagnostics, "compressed CPU decode mismatch");
            }
            ++count;
        }
        Require(count > 0, "no valid input files");
        for (int pass = 0; pass < 3; ++pass)
        {
            legacyLoadMs += legacyPass[pass]; newLoadMs += newPass[pass]; rawNewLoadMs += rawNewPass[pass];
        }
        std::ofstream report(destination / "report.json");
        report << "{\n  \"zstd\": \"" << ZSTD_versionString() << "\",\n"
            << "  \"files\": " << count << ",\n  \"invalidInputs\": " << invalid
            << ",\n  \"compressedFiles\": " << compressedCount
            << ",\n  \"sourceFileBytes\": " << inputBytes << ",\n  \"rawFileBytes\": " << rawBytes
            << ",\n  \"storedFileBytes\": " << storedBytes
            << ",\n  \"savedPercent\": " << 100.0 * (1.0 - double(storedBytes) / double(rawBytes))
            << ",\n  \"storeIncludingIoMs\": " << storeMs << ",\n  \"compressionCpuMs\": " << compressionCpuMs
            << ",\n  \"rawReadFileMs\": " << rawReadMs << ",\n  \"compressedReadFileMs\": " << compressedReadMs
            << ",\n  \"rawDecodeCpuMsMean\": " << rawDecodeMs / 3
            << ",\n  \"compressedDecodeCpuMsMean\": " << compressedDecodeMs / 3
            << ",\n  \"legacyLoadWarmMsMean\": " << legacyLoadMs / 3
            << ",\n  \"newV1LoadWarmMsMean\": " << rawNewLoadMs / 3
            << ",\n  \"newCompressedLoadWarmMsMean\": " << newLoadMs / 3 << ",\n  \"warmPasses\": [";
        for (int pass = 0; pass < 3; ++pass)
        {
            if (pass) report << ',';
            report << "{\"legacyMs\":" << legacyPass[pass] << ",\"newV1Ms\":" << rawNewPass[pass]
                   << ",\"compressedMs\":" << newPass[pass] << '}';
        }
        report << "]\n}\n";
        report.close();
        Require(!report.fail(), "report write");
        std::cout << "PASS " << count << " exact roundtrips; " << rawBytes << " -> " << storedBytes
                  << " bytes; ignored invalid source files " << invalid << '\n';
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
