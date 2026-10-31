#define NOMINMAX
#include <windows.h>
#include <DirectXTex.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "zstd_allocator_probe.h"
using namespace std;
using namespace DirectX;
// Inject one precise vector allocation failure without affecting other tests or third-party allocators.
static atomic<size_t> failAllocationSize{0};
void* operator new(size_t size)
{
    size_t expected = size;
    if (size != 0 && failAllocationSize.compare_exchange_strong(expected, 0)) throw bad_alloc();
    if (void* memory = malloc(size ? size : 1)) return memory;
    throw bad_alloc();
}
void operator delete(void* memory) noexcept { free(memory); }
void operator delete(void* memory, size_t) noexcept { free(memory); }
// Only source-import helpers are replaced. Cache keys, flags, counters and storage are production code.
namespace Debug { template<class... Args> void Log(const char*, Args...) {} }
static HRESULT LoadImageFromFile(const wchar_t*, TexMetadata&, ScratchImage&) { return E_FAIL; }
static HRESULT LoadImageFromMemory(const void*, size_t, TexMetadata&, ScratchImage&) { return E_FAIL; }
static void EnsureMipChain(TexMetadata&, ScratchImage&) {}
#include "../source/assets/texturecompression.inl"

static size_t checks = 0;
static void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw runtime_error(message);
}
static ScratchImage MakeBc4Image(bool noise = false)
{
    ScratchImage image;
    Check(SUCCEEDED(image.Initialize2D(DXGI_FORMAT_BC4_UNORM, noise ? 512 : 64, noise ? 512 : 64, 1, 1)), "fixture allocation");
    // Every 8-byte BC4 block is a valid endpoint/index encoding.
    mt19937 random(42);
    for (size_t index = 0; index < image.GetPixelsSize(); ++index)
        image.GetPixels()[index] = noise ? static_cast<uint8_t>(random()) : uint8_t(0);
    return image;
}
static void Equal(const ScratchImage& a, const ScratchImage& b)
{
    Check(a.GetMetadata().format == b.GetMetadata().format && a.GetMetadata().width == b.GetMetadata().width &&
          a.GetMetadata().height == b.GetMetadata().height && a.GetMetadata().mipLevels == b.GetMetadata().mipLevels &&
          a.GetPixelsSize() == b.GetPixelsSize() && memcmp(a.GetPixels(), b.GetPixels(), a.GetPixelsSize()) == 0,
          "metadata and BC payload must match");
}
using Bytes = vector<uint8_t>;
static Bytes Read(const filesystem::path& path)
{
    ifstream stream(path, ios::binary | ios::ate);
    Check(static_cast<bool>(stream), "read fixture");
    Bytes result(static_cast<size_t>(stream.tellg()));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(result.data()), result.size());
    Check(static_cast<bool>(stream), "complete fixture read");
    return result;
}
static void Write(const filesystem::path& path, const Bytes& bytes)
{
    ofstream stream(path, ios::binary | ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Check(static_cast<bool>(stream), "fixture write");
}
static void SetWord(Bytes& bytes, size_t index, uint64_t value) { memcpy(bytes.data() + index * 8, &value, 8); }
static Bytes Frame(const Bytes& raw, bool contentSize = true)
{
    ZSTD_CCtx* context = ZSTD_createCCtx();
    Check(context != nullptr, "zstd test context");
    ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, contentSize ? 1 : 0);
    Bytes result(ZSTD_compressBound(raw.size()));
    const size_t stored = ZSTD_compress2(context, result.data(), result.size(), raw.data(), raw.size());
    ZSTD_freeCCtx(context);
    Check(!ZSTD_isError(stored), "test frame encoding");
    result.resize(stored);
    return result;
}
static Bytes Sidecar(const Bytes& raw, const Bytes& payload)
{
    Bytes result(32);
    SetWord(result, 0, TextureCacheCodec::Magic);
    SetWord(result, 1, raw.size());
    SetWord(result, 2, payload.size());
    SetWord(result, 3, TextureCacheCodec::Hash(raw.data(), raw.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
static void Reject(const Bytes& bytes, const ScratchImage& sentinel)
{
    const filesystem::path raw = "invalid.dds";
    Write(TextureCacheStorage::CompressedPath(raw), bytes);
    ScratchImage image;
    Check(SUCCEEDED(image.InitializeFromImage(*sentinel.GetImages())), "sentinel copy");
    TexMetadata metadata = image.GetMetadata();
    Check(!TextureCacheStorage::Load(raw, metadata, image), "malformed sidecar must miss");
    Equal(image, sentinel);
    Check(metadata.format == sentinel.GetMetadata().format && metadata.width == sentinel.GetMetadata().width,
          "failed load must preserve caller metadata");
}
int main(int argc, char**)
{
    const auto originalDirectory = filesystem::current_path();
    try
    {
        const auto directory = originalDirectory / "bin/validation/technology" /
            ("texture-cache-tests-" + to_string(GetCurrentProcessId()));
        filesystem::create_directories(directory);
        filesystem::current_path(directory);
        auto image = MakeBc4Image();
        const auto raw = TextureCompressionDetail::CacheFile(1);
        filesystem::create_directories(raw.parent_path());
        Check(SUCCEEDED(SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DDS_FLAGS_NONE, raw.c_str())), "legacy fixture write");
        ScratchImage loaded;
        TexMetadata metadata{};
        Check(TextureCompression::TryLoadCached(1, metadata, loaded), "production legacy hit");
        Equal(image, loaded);
        Check(TextureCompression::GetStatistics().CacheHits == 1, "one hit per adopted image");
        TextureCompression::GetSettings().Enabled = false;
        Check(!TextureCompression::TryLoadCached(1, metadata, loaded), "disabled cache misses");
        auto disabledImage = MakeBc4Image();
        auto disabledMetadata = disabledImage.GetMetadata();
        Check(!TextureCompression::CompressAndCache(20, TextureUsage::Normal, disabledMetadata, disabledImage), "disabled setting blocks BC work");
        TextureCompressionDetail::SaveCache(0, image);
        TextureCompression::GetSettings().Enabled = true;
        Check(!TextureCompression::TryLoadCached(0, metadata, loaded), "key0 misses");
        Check(!filesystem::exists(TextureCompressionDetail::CacheFile(0)) &&
              !filesystem::exists(TextureCacheStorage::CompressedPath(TextureCompressionDetail::CacheFile(0))), "key0 writes no cache");
        Check(TextureCompression::GetStatistics().CacheHits == 1, "disabled/key0 cannot increment hits");
        if (argc > 1)
        {
            TextureCompressionDetail::SaveCache(2, image);
            const auto compressed = TextureCacheStorage::CompressedPath(TextureCompressionDetail::CacheFile(2));
            Check(filesystem::exists(compressed), "compressible BC cache must save sidecar");
            Check(TextureCompression::TryLoadCached(2, metadata, loaded), "sidecar hit");
            Equal(image, loaded);
            Check(!filesystem::exists(TextureCompressionDetail::CacheFile(2)), "compressed save need not duplicate legacy DDS");
            const Bytes valid = Read(compressed);
            Blob dds;
            Check(SUCCEEDED(SaveToDDSMemory(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DDS_FLAGS_NONE, dds)), "DDS fixture memory");
            const Bytes rawDds(dds.GetBufferPointer(), dds.GetBufferPointer() + dds.GetBufferSize());
            Check(valid.size() < rawDds.size(), "complete sidecar must shrink DDS");
            for (size_t length = 0; length < valid.size(); ++length) Reject(Bytes(valid.begin(), valid.begin() + length), image);
            for (size_t field = 0; field < 4; ++field)
            {
                Bytes broken = valid;
                SetWord(broken, field, ~0ull);
                Reject(broken, image);
            }
            for (uint64_t size : {0ull, 135ull, TextureCacheCodec::MaxDdsBytes + 1})
            {
                Bytes broken = valid; SetWord(broken, 1, size); Reject(broken, image);
            }
            Bytes broken = valid; SetWord(broken, 2, 0); Reject(broken, image);
            broken = valid; SetWord(broken, 3, 0); Reject(broken, image);
            broken = valid; broken.back() ^= 1; Reject(broken, image);
            broken = valid; broken.push_back(0); Reject(broken, image);
            auto frame = Frame(rawDds);
            Bytes concatenated = frame; concatenated.insert(concatenated.end(), frame.begin(), frame.end());
            Reject(Sidecar(rawDds, concatenated), image);
            frame.push_back(0); Reject(Sidecar(rawDds, frame), image);
            Reject(Sidecar(rawDds, Frame(rawDds, false)), image);
            Bytes changed = rawDds; changed.push_back(0); Reject(Sidecar(rawDds, Frame(changed)), image);
            Reject(Sidecar(rawDds, Bytes{0x50, 0x2a, 0x4d, 0x18, 0, 0, 0, 0}), image);
            Reject(Sidecar(rawDds, Bytes(32, 'z')), image);
            Bytes notDds(rawDds.size(), 0); Reject(Sidecar(notDds, Frame(notDds)), image);

            Write(TextureCacheStorage::CompressedPath(raw), Bytes{1, 2, 3});
            const auto hits = TextureCompression::GetStatistics().CacheHits;
            Check(TextureCompression::TryLoadCached(1, metadata, loaded), "corrupt sidecar must fall back to valid DDS");
            Equal(image, loaded);
            Check(TextureCompression::GetStatistics().CacheHits == hits + 1, "fallback increments once");
            Check(filesystem::exists(raw), "fallback cannot remove valid legacy DDS");
            Check(!filesystem::exists(TextureCacheStorage::CompressedPath(raw)), "corrupt sidecar is evicted");

            Write(TextureCacheStorage::CompressedPath(raw), valid);
            failAllocationSize = rawDds.size();
            const bool allocationFallback = TextureCompression::TryLoadCached(1, metadata, loaded);
            const bool injected = failAllocationSize.exchange(0) == 0;
            Check(injected && allocationFallback, "decode allocation failure must use legacy DDS");
            Equal(image, loaded);
            Check(filesystem::exists(TextureCacheStorage::CompressedPath(raw)), "allocation failure cannot evict valid sidecar");
            TextureProbeFailNextZstdAllocation();
            const bool zstdAllocationFallback = TextureCompression::TryLoadCached(1, metadata, loaded);
            const bool zstdFailureInjected = !TextureProbeZstdFailurePending();
            cout << "zstd malloc injection: consumed=" << zstdFailureInjected << " retained="
                 << filesystem::exists(TextureCacheStorage::CompressedPath(raw)) << '\n';
            Check(zstdFailureInjected && zstdAllocationFallback && filesystem::exists(TextureCacheStorage::CompressedPath(raw)),
                  "zstd malloc failure must use legacy DDS without evicting a valid sidecar");
            Equal(image, loaded);
            failAllocationSize = ZSTD_compressBound(rawDds.size());
            const bool savedAfterAllocationFailure = TextureCacheStorage::Store("allocation-save.dds", image);
            const bool encodeFailureInjected = failAllocationSize.exchange(0) == 0;
            Check(encodeFailureInjected && savedAfterAllocationFailure && filesystem::exists("allocation-save.dds") &&
                  !filesystem::exists("allocation-save.dds.zst"), "compression allocation failure must save legacy DDS");

            Check(SUCCEEDED(SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DDS_FLAGS_NONE, L"e.dds")), "legacy resource fixture");
            failAllocationSize = sizeof(DirectX::Image);
            bool resourceHit = false, allocationThrew = false;
            try { resourceHit = TextureCacheStorage::Load("e.dds", metadata, loaded); }
            catch (const bad_alloc&) { allocationThrew = true; }
            const bool legacyFailureInjected = failAllocationSize.exchange(0) == 0;
            cout << "Legacy allocation injection: consumed=" << legacyFailureInjected << " hit=" << resourceHit
                 << " threw=" << allocationThrew << " retained=" << filesystem::exists("e.dds") << '\n';
            Check(legacyFailureInjected && !resourceHit && !allocationThrew && filesystem::exists("e.dds"),
                  "legacy allocation failure must miss without throwing or evicting valid DDS");

            const filesystem::path blocked = "blocked.dds";
            filesystem::create_directory(TextureCacheStorage::CompressedPath(blocked));
            Check(TextureCacheStorage::Store(blocked, image) && filesystem::exists(blocked), "sidecar write failure falls back to DDS");
            auto noise = MakeBc4Image(true);
            Check(TextureCacheStorage::Store("noise.dds", noise), "noise Store");
            Check(filesystem::exists("noise.dds") && !filesystem::exists("noise.dds.zst"), "non-beneficial DDS keeps raw");
            Check(TextureCacheStorage::Load("noise.dds", metadata, loaded), "noise raw load");
            Equal(noise, loaded);

            // The codec accounts for the full header, even when the frame alone is smaller.
            bool boundaryFound = false;
            for (size_t size = 136; size < 320; ++size)
            {
                Bytes input(size);
                mt19937 random(19);
                for (size_t index = 0; index < size; ++index) input[index] = index < 50 ? 0 : static_cast<uint8_t>(random());
                const auto encoded = Frame(input);
                if (encoded.size() < size && encoded.size() + 32 >= size)
                {
                    TextureCacheCodec::Header header; Bytes payload;
                    Check(!TextureCacheCodec::TryEncode(input.data(), input.size(), payload, header), "header cost prevents sidecar expansion");
                    boundaryFound = true; break;
                }
            }
            Check(boundaryFound, "header boundary fixture exists");
            array<uint64_t, 4> words{TextureCacheCodec::Magic, TextureCacheCodec::MaxDdsBytes, 1, 0};
            TextureCacheCodec::Header header;
            Check(TextureCacheCodec::ParseHeader(words, 33, header), "exact raw cap accepted without allocation");
            Bytes payload;
            Check(!TextureCacheCodec::TryEncode(rawDds.data(), static_cast<size_t>(TextureCacheCodec::MaxDdsBytes + 1), payload, header), "oversized raw input rejected before access");

            // Exercise the real BC encoder and preserve resident-byte statistics.
            ScratchImage rgba;
            Check(SUCCEEDED(rgba.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1)), "RGBA fixture");
            memset(rgba.GetPixels(), 128, rgba.GetPixelsSize());
            const auto before = TextureCompression::GetStatistics();
            TexMetadata rgbaMetadata = rgba.GetMetadata();
            Check(TextureCompression::CompressAndCache(10, TextureUsage::Normal, rgbaMetadata, rgba), "production normal BC encoding");
            const auto after = TextureCompression::GetStatistics();
            Check(rgbaMetadata.format == DXGI_FORMAT_BC5_UNORM && after.Compressed == before.Compressed + 1 &&
                  after.CompressedBytes - before.CompressedBytes == rgba.GetPixelsSize(), "BC counters and normal mapping unchanged");
            Check(TextureCompression::TryLoadCached(10, metadata, loaded), "actual encoded cache hit");
            Equal(rgba, loaded);
            struct FormatCase { TextureUsage usage; DXGI_FORMAT source; DXGI_FORMAT target; };
            const FormatCase formatCases[] = {
                {TextureUsage::Color, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC7_UNORM},
                {TextureUsage::Color, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_BC7_UNORM_SRGB},
                {TextureUsage::TwoChannel, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC5_UNORM},
                {TextureUsage::SingleChannel, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC4_UNORM}};
            uint64_t formatKey = 30;
            for (const auto& formatCase : formatCases)
            {
                ScratchImage source;
                Check(SUCCEEDED(source.Initialize2D(formatCase.source, 16, 16, 1, 1)), "format fixture");
                memset(source.GetPixels(), 128, source.GetPixelsSize());
                auto info = source.GetMetadata();
                Check(TextureCompression::CompressAndCache(formatKey, formatCase.usage, info, source), "production BC usage case");
                Check(info.format == formatCase.target, "usage/sRGB format contract");
                Check(TextureCompression::TryLoadCached(formatKey++, metadata, loaded), "format cache hit");
                Equal(source, loaded);
            }
            ScratchImage uncompressed;
            Check(SUCCEEDED(uncompressed.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1)), "uncompressed DDS fixture");
            memset(uncompressed.GetPixels(), 0, uncompressed.GetPixelsSize());
            Blob uncompressedDds;
            Check(SUCCEEDED(SaveToDDSMemory(uncompressed.GetImages(), uncompressed.GetImageCount(), uncompressed.GetMetadata(), DDS_FLAGS_NONE, uncompressedDds)), "uncompressed DDS serialization");
            Bytes uncompressedBytes(uncompressedDds.GetBufferPointer(), uncompressedDds.GetBufferPointer() + uncompressedDds.GetBufferSize());
            Reject(Sidecar(uncompressedBytes, Frame(uncompressedBytes)), image);

            Check(TextureCacheStorage::Store("concurrent.dds", image), "concurrency initial cache");
            atomic<bool> bad{false};
            atomic<unsigned> failedStores{0};
            vector<thread> workers;
            for (int worker = 0; worker < 3; ++worker) workers.emplace_back([&] {
                for (int iteration = 0; iteration < 20; ++iteration)
                {
                    if (!TextureCacheStorage::Store("concurrent.dds", image)) ++failedStores;
                    ScratchImage read; TexMetadata info{};
                    if (TextureCacheStorage::Load("concurrent.dds", info, read) &&
                        (read.GetPixelsSize() != image.GetPixelsSize() || memcmp(read.GetPixels(), image.GetPixels(), image.GetPixelsSize()) != 0)) bad = true;
                }
            });
            for (auto& worker : workers) worker.join();
            cout << "Concurrent cache best-effort write misses: " << failedStores.load() << '\n';
            Check(!bad, "concurrent writes/reads publish only complete images");
            Check(TextureCacheStorage::Load("concurrent.dds", metadata, loaded), "final concurrent cache valid");
            Equal(image, loaded);
        }
        filesystem::current_path(originalDirectory);
        cout << "PASS texture cache storage " << checks << " checks\n";
        return 0;
    }
    catch (const exception& error)
    {
        filesystem::current_path(originalDirectory);
        cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
