// texturemanager.cpp の末尾から取り込まれる実装（プロジェクトへのファイル追加を不要にするため .inl）。
#include "DirectXTex.h"
#include "texturecompression.h"
#include "texturecachestorage.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <cwctype>

// texturemanager.cpp の LoadImageFromFile / LoadImageFromMemory / EnsureMipChain をそのまま使い、
// 通常読み込みと同じ画像からキャッシュを作る。

namespace TextureCompressionDetail
{
    // エンコーダーや判定規則を変えたら上げる（古いキャッシュを自動的に無効化）。
    constexpr uint64_t kEncoderVersion = 1;

    TextureCompression::Settings g_Settings;

    struct Counters
    {
        std::atomic<uint32_t> CacheHits{0};
        std::atomic<uint32_t> Compressed{0};
        std::atomic<uint32_t> Skipped{0};
        std::atomic<uint32_t> BackgroundPending{0};
        std::atomic<uint64_t> SourceBytes{0};
        std::atomic<uint64_t> CompressedBytes{0};
    };

    Counters& GetCounters()
    {
        static Counters* counters = new Counters();
        return *counters;
    }

    // BC 圧縮は内部で並列化されるため、同時に 1 枚だけ圧縮して CPU の過剰な取り合いを避ける。
    std::mutex& CompressionGate()
    {
        static std::mutex* gate = new std::mutex();
        return *gate;
    }

    uint64_t Fnv(const void* data, size_t size, uint64_t hash = 14695981039346656037ull)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    bool IsFloatFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
            case DXGI_FORMAT_R32G32B32_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R32G32_FLOAT:
            case DXGI_FORMAT_R11G11B10_FLOAT:
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_R16_FLOAT:
            case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
                return true;
            default:
                return false;
        }
    }

    DXGI_FORMAT TargetFormat(TextureUsage usage, DXGI_FORMAT source)
    {
        switch (usage)
        {
            case TextureUsage::Normal:
            case TextureUsage::TwoChannel:
                return DXGI_FORMAT_BC5_UNORM;
            case TextureUsage::SingleChannel:
                return DXGI_FORMAT_BC4_UNORM;
            case TextureUsage::Color:
            default:
                return IsSRGB(source) ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        }
    }

    std::filesystem::path CacheFile(uint64_t cacheKey)
    {
        char name[32]{};
        sprintf_s(name, "%016llx.dds", static_cast<unsigned long long>(cacheKey));
        return TextureCompression::CacheDirectory() / name;
    }

    void SaveCache(uint64_t cacheKey, const ScratchImage& image)
    {
        if (cacheKey == 0)
        {
            return;
        }
        TextureCacheStorage::Store(CacheFile(cacheKey), image);
    }

    // バックグラウンド圧縮用の単一ワーカー。終了時に破棄しないようにヒープへ置き、スレッドは切り離す。
    struct BackgroundQueue
    {
        std::mutex Mutex;
        std::condition_variable Ready;
        std::deque<std::function<void()>> Jobs;
        std::unordered_set<uint64_t> Queued;
        bool Started = false;
    };

    BackgroundQueue& GetQueue()
    {
        static BackgroundQueue* queue = new BackgroundQueue();
        return *queue;
    }

    void Enqueue(uint64_t cacheKey, std::function<void()> job)
    {
        auto& queue = GetQueue();
        {
            std::lock_guard<std::mutex> lock(queue.Mutex);
            if (cacheKey == 0 || !queue.Queued.insert(cacheKey).second)
            {
                return;
            }
            ++GetCounters().BackgroundPending;
            queue.Jobs.push_back([cacheKey, job = std::move(job)]() {
                job();
                auto& owner = GetQueue();
                std::lock_guard<std::mutex> done(owner.Mutex);
                owner.Queued.erase(cacheKey);
                --GetCounters().BackgroundPending;
            });
            if (!queue.Started)
            {
                queue.Started = true;
                std::thread([] {
                    auto& owner = GetQueue();
                    for (;;)
                    {
                        std::function<void()> next;
                        {
                            std::unique_lock<std::mutex> lock(owner.Mutex);
                            owner.Ready.wait(lock, [&owner] {
                                return !owner.Jobs.empty();
                            });
                            next = std::move(owner.Jobs.front());
                            owner.Jobs.pop_front();
                        }
                        // ワーカーは描画より優先度を下げる。
                        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                        next();
                    }
                }).detach();
            }
        }
        queue.Ready.notify_one();
    }
} // namespace TextureCompressionDetail

namespace TextureCompression
{
    using namespace TextureCompressionDetail;

    Settings& GetSettings()
    {
        return g_Settings;
    }

    Statistics GetStatistics()
    {
        const auto& counters = GetCounters();
        Statistics statistics;
        statistics.CacheHits = counters.CacheHits.load();
        statistics.Compressed = counters.Compressed.load();
        statistics.Skipped = counters.Skipped.load();
        statistics.BackgroundPending = counters.BackgroundPending.load();
        statistics.SourceBytes = counters.SourceBytes.load();
        statistics.CompressedBytes = counters.CompressedBytes.load();
        return statistics;
    }

    const char* UsageName(TextureUsage usage)
    {
        switch (usage)
        {
            case TextureUsage::Normal:
                return "BC5 (法線)";
            case TextureUsage::TwoChannel:
                return "BC5 (2ch)";
            case TextureUsage::SingleChannel:
                return "BC4 (1ch)";
            case TextureUsage::Color:
            default:
                return "BC7 (カラー)";
        }
    }

    std::string KeySuffix(TextureUsage usage)
    {
        switch (usage)
        {
            case TextureUsage::Normal:
                return "#tc:normal";
            case TextureUsage::TwoChannel:
                return "#tc:rg";
            case TextureUsage::SingleChannel:
                return "#tc:r";
            case TextureUsage::Color:
            default:
                return {};
        }
    }

    std::string StripKeySuffix(const std::string& key)
    {
        const auto position = key.rfind("#tc:");
        return position == std::string::npos ? key : key.substr(0, position);
    }

    std::filesystem::path CacheDirectory()
    {
        std::error_code error;
        const auto directory = (std::filesystem::current_path(error) / "Data" / "Cache" / "Textures").lexically_normal();
        static std::once_flag cleaned;
        std::call_once(cleaned, [&directory] {
            // 前回の異常終了で残った書きかけファイルを掃除する。
            std::error_code iterateError;
            for (std::filesystem::directory_iterator it(directory, iterateError), end; !iterateError && it != end;
                 it.increment(iterateError))
            {
                if (it->path().extension() == ".tmp")
                {
                    std::error_code removeError;
                    std::filesystem::remove(it->path(), removeError);
                }
            }
        });
        return directory;
    }

    uint64_t FileCacheKey(const std::filesystem::path& source, TextureUsage usage)
    {
        std::error_code error;
        auto absolute = std::filesystem::absolute(source, error);
        if (error)
        {
            absolute = source;
        }
        std::wstring path = absolute.lexically_normal().wstring();
        for (auto& letter : path)
        {
            letter = static_cast<wchar_t>(towlower(letter));
        }
        const auto size = std::filesystem::file_size(source, error);
        if (error)
        {
            return 0;
        }
        const auto modified = std::filesystem::last_write_time(source, error);
        if (error)
        {
            return 0;
        }
        const int64_t time = modified.time_since_epoch().count();
        const uint32_t usageValue = static_cast<uint32_t>(usage);
        uint64_t hash = Fnv(&kEncoderVersion, sizeof(kEncoderVersion));
        hash = Fnv(path.data(), path.size() * sizeof(wchar_t), hash);
        hash = Fnv(&size, sizeof(size), hash);
        hash = Fnv(&time, sizeof(time), hash);
        hash = Fnv(&usageValue, sizeof(usageValue), hash);
        return hash ? hash : 1;
    }

    uint64_t MemoryCacheKey(const void* data, size_t size, TextureUsage usage)
    {
        if (!data || size == 0)
        {
            return 0;
        }
        const uint32_t usageValue = static_cast<uint32_t>(usage);
        const uint64_t marker = 0x4d454d5458ull; // "MEMTX"
        uint64_t hash = Fnv(&kEncoderVersion, sizeof(kEncoderVersion));
        hash = Fnv(&marker, sizeof(marker), hash);
        hash = Fnv(data, size, hash);
        hash = Fnv(&size, sizeof(size), hash);
        hash = Fnv(&usageValue, sizeof(usageValue), hash);
        return hash ? hash : 1;
    }

    bool TryLoadCached(uint64_t cacheKey, TexMetadata& metadata, ScratchImage& image)
    {
        if (!g_Settings.Enabled || cacheKey == 0)
        {
            return false;
        }
        ScratchImage cached;
        TexMetadata cachedMetadata{};
        if (!TextureCacheStorage::Load(CacheFile(cacheKey), cachedMetadata, cached))
        {
            return false;
        }
        metadata = cachedMetadata;
        image = std::move(cached);
        ++GetCounters().CacheHits;
        return true;
    }

    bool IsCompressible(const TexMetadata& metadata)
    {
        // BC は 4x4 ブロック単位。最上位ミップが 4 の倍数でないものと、HDR（BC6H 対象外）・配列・3D は対象外。
        return !IsCompressed(metadata.format) && !IsFloatFormat(metadata.format) && !IsPlanar(metadata.format) &&
               !IsPalettized(metadata.format) && !IsVideo(metadata.format) &&
               metadata.dimension == TEX_DIMENSION_TEXTURE2D && metadata.arraySize == 1 && metadata.depth == 1 &&
               !metadata.IsCubemap() && (metadata.width % 4) == 0 && (metadata.height % 4) == 0 &&
               metadata.width >= 4 && metadata.height >= 4;
    }

    bool CompressAndCache(uint64_t cacheKey, TextureUsage usage, TexMetadata& metadata, ScratchImage& image)
    {
        if (!g_Settings.Enabled)
        {
            return false;
        }
        if (!IsCompressible(metadata))
        {
            ++GetCounters().Skipped;
            return false;
        }
        const DXGI_FORMAT target = TargetFormat(usage, metadata.format);
        // DirectXTex の版によってはフラグ型に |= が無いため、整数で組み立てる。
        unsigned long flagBits = TEX_COMPRESS_PARALLEL;
        if (target == DXGI_FORMAT_BC7_UNORM || target == DXGI_FORMAT_BC7_UNORM_SRGB)
        {
            // モード 6 中心の高速エンコード（初回のみのコスト。結果はキャッシュされる）。
            flagBits |= TEX_COMPRESS_BC7_QUICK;
        }
        else if (IsSRGB(metadata.format))
        {
            // sRGB ソースを非 sRGB の BC4/BC5 にする場合、従来ハードウェアが行っていた sRGB→線形変換を
            // 事前に適用し、サンプリング結果を一致させる。
            flagBits |= TEX_COMPRESS_SRGB_IN;
        }
        const auto flags = static_cast<TEX_COMPRESS_FLAGS>(flagBits);
        ScratchImage compressed;
        HRESULT result = E_FAIL;
        {
            std::lock_guard<std::mutex> lock(CompressionGate());
            result = Compress(image.GetImages(), image.GetImageCount(), image.GetMetadata(), target, flags,
                              TEX_THRESHOLD_DEFAULT, compressed);
        }
        if (FAILED(result))
        {
            Debug::Log("TextureCompression: Compress to %s failed (0x%08X)\n", UsageName(usage),
                       static_cast<unsigned>(result));
            ++GetCounters().Skipped;
            return false;
        }
        GetCounters().SourceBytes += image.GetPixelsSize();
        GetCounters().CompressedBytes += compressed.GetPixelsSize();
        ++GetCounters().Compressed;
        SaveCache(cacheKey, compressed);
        metadata = compressed.GetMetadata();
        image = std::move(compressed);
        return true;
    }

    void CompressFileInBackground(uint64_t cacheKey, TextureUsage usage, const std::filesystem::path& source)
    {
        if (!g_Settings.Enabled || cacheKey == 0)
        {
            return;
        }
        Enqueue(cacheKey, [cacheKey, usage, source]() {
            TexMetadata metadata{};
            ScratchImage image;
            if (FAILED(LoadImageFromFile(source.c_str(), metadata, image)))
            {
                return;
            }
            EnsureMipChain(metadata, image);
            CompressAndCache(cacheKey, usage, metadata, image);
        });
    }

    void CompressMemoryInBackground(uint64_t cacheKey, TextureUsage usage, const void* data, size_t size)
    {
        if (!g_Settings.Enabled || cacheKey == 0 || !data || size == 0)
        {
            return;
        }
        auto copy = std::make_shared<std::vector<uint8_t>>(static_cast<const uint8_t*>(data),
                                                           static_cast<const uint8_t*>(data) + size);
        Enqueue(cacheKey, [cacheKey, usage, copy]() {
            TexMetadata metadata{};
            ScratchImage image;
            if (FAILED(LoadImageFromMemory(copy->data(), copy->size(), metadata, image)))
            {
                return;
            }
            EnsureMipChain(metadata, image);
            CompressAndCache(cacheKey, usage, metadata, image);
        });
    }
} // namespace TextureCompression
