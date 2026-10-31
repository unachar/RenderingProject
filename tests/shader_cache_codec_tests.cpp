#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include "dxcshadercache.h"
#include <iostream>
#include <stdexcept>
#include <limits>
#include <random>

using Bytes = std::vector<char>;
static size_t checks = 0;

static void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

static Bytes Read(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    Bytes bytes(static_cast<size_t>(stream.tellg()));
    stream.seekg(0);
    stream.read(bytes.data(), bytes.size());
    return bytes;
}

static void SetWord(Bytes& bytes, size_t index, uint64_t value)
{
    std::memcpy(bytes.data() + index * 8, &value, 8);
}

static Bytes File(const Bytes& payload, const Bytes& object, const std::string& diagnostics, bool compressed)
{
    Bytes result(compressed ? 48 : 32);
    SetWord(result, 0, compressed ? ShaderCacheCodec::MagicV2 : ShaderCacheCodec::MagicV1);
    const auto checksum = DxcShaderCache::Hash(diagnostics.data(), diagnostics.size(),
                                              DxcShaderCache::Hash(object.data(), object.size()));
    if (compressed)
    {
        SetWord(result, 1, 1);
        SetWord(result, 2, object.size());
        SetWord(result, 3, diagnostics.size());
        SetWord(result, 4, payload.size());
        SetWord(result, 5, checksum);
    }
    else
    {
        SetWord(result, 1, object.size());
        SetWord(result, 2, diagnostics.size());
        SetWord(result, 3, checksum);
    }
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

static void Write(const std::filesystem::path& path, const Bytes& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), bytes.size());
    Check(static_cast<bool>(stream), "test file write");
}

static void Reject(const std::filesystem::path& path, const Bytes& bytes)
{
    Write(path, bytes);
    Bytes object(7, 's');
    std::string diagnostics = "stale";
    Check(!DxcShaderCache::Load(path, object, diagnostics), "malformed file must miss");
    Check(object.empty() && diagnostics.empty(), "cache miss must clear outputs");
}

static Bytes Compress(const Bytes& raw, bool contentSize = true)
{
    ZSTD_CCtx* context = ZSTD_createCCtx();
    Check(context != nullptr, "compression context");
    ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, contentSize ? 1 : 0);
    Bytes payload(ZSTD_compressBound(raw.size()));
    const size_t size = ZSTD_compress2(context, payload.data(), payload.size(), raw.data(), raw.size());
    ZSTD_freeCCtx(context);
    Check(!ZSTD_isError(size), "test frame compression");
    payload.resize(size);
    return payload;
}

int main()
{
    try
    {
        const auto directory = std::filesystem::path("bin/validation/technology/codec-tests");
        std::filesystem::create_directories(directory);
        const auto path = directory / "roundtrip.bin";
        const std::vector<char> input(8192, 'x');
        const std::string warning = "warning: preserved diagnostics";
        DxcShaderCache::Store(path, input.data(), input.size(), warning);
        uint64_t magic = 0;
        std::ifstream stream(path, std::ios::binary);
        stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        stream.close();
        Check(magic == 0x3243584443484143ull, "compressible Store must use v2");
        std::vector<char> output;
        std::string diagnostics;
        Check(DxcShaderCache::Load(path, output, diagnostics), "v2 Load");
        Check(output == input && diagnostics == warning, "byte-exact roundtrip");
        const Bytes validV2 = Read(path);
        Bytes raw = input;
        raw.insert(raw.end(), warning.begin(), warning.end());
        const Bytes validV1 = File(raw, input, warning, false);
        Write(path, validV1);
        Check(DxcShaderCache::Load(path, output, diagnostics), "old v1 Load");
        Check(output == input && diagnostics == warning, "old v1 exact bytes");
        for (const Bytes& valid : {validV1, validV2})
        {
            const bool v2 = valid.size() == validV2.size();
            Bytes broken = valid;
            SetWord(broken, 0, 0);
            Reject(path, broken);
            for (const uint64_t value : {0ull, 64ull * 1024 * 1024 + 1, ~0ull})
            {
                broken = valid;
                SetWord(broken, v2 ? 2 : 1, value);
                Reject(path, broken);
            }
            for (const uint64_t value : {1024ull * 1024 + 1, ~0ull})
            {
                broken = valid;
                SetWord(broken, v2 ? 3 : 2, value);
                Reject(path, broken);
            }
            broken = valid;
            SetWord(broken, v2 ? 5 : 3, 0);
            Reject(path, broken);
            broken = valid;
            broken.back() ^= 1;
            Reject(path, broken);
            broken = valid;
            broken.push_back('x');
            Reject(path, broken);
            // All compressed truncations; header boundaries plus last byte for raw.
            const size_t count = v2 ? valid.size() : 33;
            for (size_t size = 0; size < count; ++size)
                Reject(path, Bytes(valid.begin(), valid.begin() + size));
            Reject(path, Bytes(valid.begin(), valid.end() - 1));
        }
        for (uint64_t codec : {0ull, 2ull, ~0ull})
        {
            Bytes broken = validV2;
            SetWord(broken, 1, codec);
            Reject(path, broken);
        }
        for (uint64_t stored : {0ull, static_cast<uint64_t>(raw.size() + 1), ~0ull})
        {
            Bytes broken = validV2;
            SetWord(broken, 4, stored);
            Reject(path, broken);
        }
        Bytes frame = Compress(raw);
        Bytes concatenated = frame;
        concatenated.insert(concatenated.end(), frame.begin(), frame.end());
        Reject(path, File(concatenated, input, warning, true));
        frame.push_back(0);
        Reject(path, File(frame, input, warning, true));
        Reject(path, File(Compress(raw, false), input, warning, true));
        Bytes different = raw;
        different.push_back('x');
        Reject(path, File(Compress(different), input, warning, true));
        // Skippable frame, arbitrary invalid frame, and a checksum-valid but wrong payload.
        Reject(path, File(Bytes{0x50, 0x2a, 0x4d, 0x18, 0, 0, 0, 0}, input, warning, true));
        Reject(path, File(Bytes(32, 'z'), input, warning, true));
        different = raw;
        different[0] ^= 1;
        Reject(path, File(Compress(different), input, warning, true));

        for (size_t size : {size_t(1), size_t(16), size_t(8192)})
        {
            Bytes noise(size);
            std::mt19937 random(42);
            for (char& byte : noise) byte = static_cast<char>(random());
            DxcShaderCache::Store(path, noise.data(), noise.size(), "");
            Check(Read(path).size() == 32 + size, "non-beneficial input keeps v1");
            Check(DxcShaderCache::Load(path, output, diagnostics) && output == noise && diagnostics.empty(),
                  "raw fallback exact bytes");
        }
        // Compression can save bytes but still lose after the larger header is counted.
        bool foundHeaderBoundary = false;
        for (size_t size = 16; size < 64; ++size)
        {
            Bytes repeated(size, 'a');
            const auto compressed = Compress(repeated);
            if (compressed.size() < size && compressed.size() + 16 >= size)
            {
                DxcShaderCache::Store(path, repeated.data(), repeated.size(), "");
                Check(Read(path).size() == 32 + size, "header cost forces raw fallback");
                foundHeaderBoundary = true;
                break;
            }
        }
        Check(foundHeaderBoundary, "header boundary fixture exists");
        const std::string binaryDiagnostics("warning\0\xff\n", 10);
        DxcShaderCache::Store(path, input.data(), input.size(), binaryDiagnostics);
        Check(DxcShaderCache::Load(path, output, diagnostics) && output == input && diagnostics == binaryDiagnostics,
              "diagnostics preserve embedded NUL and non-ASCII bytes");
        const Bytes beforeInvalidStore = Read(path);
        DxcShaderCache::Store(path, nullptr, input.size(), "");
        DxcShaderCache::Store(path, input.data(), 0, "");
        DxcShaderCache::Store(path, input.data(), static_cast<size_t>(ShaderCacheCodec::MaxObjectBytes + 1), "");
        Check(Read(path) == beforeInvalidStore, "invalid Store cannot replace an existing entry");
        output = {'x'}; diagnostics = "old";
        Check(!DxcShaderCache::Load({}, output, diagnostics) && output.empty() && diagnostics.empty(),
              "disabled path clears output");
        std::array<uint64_t, 6> boundary{ShaderCacheCodec::MagicV1,
            ShaderCacheCodec::MaxObjectBytes, ShaderCacheCodec::MaxDiagnosticBytes, 0, 0, 0};
        ShaderCacheCodec::Header header;
        Check(ShaderCacheCodec::ParseHeader(boundary, 32 + boundary[1] + boundary[2], header),
              "exact size caps accepted before allocation");
        std::cout << "PASS shader cache codec " << checks << " checks; zstd " << ZSTD_versionString() << '\n';
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
