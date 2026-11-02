#include "pch.h" // プロジェクトのプリコンパイル済みヘッダー（テストでは空のスタブ）
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "sequencerwaveform.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace SequencerWaveform
{
    namespace
    {
        struct Entry
        {
            State Status = State::None;
            std::shared_ptr<const Peaks> Result;
            std::filesystem::file_time_type WriteTime{};
            uintmax_t Size = 0;
        };

        // ワーカースレッドが終了処理と競合しないよう、意図的に破棄しない。
        std::mutex& g_Mutex = *new std::mutex;
        std::unordered_map<std::string, Entry>& g_Cache = *new std::unordered_map<std::string, Entry>;

        bool ReadExact(std::ifstream& input, void* data, size_t size)
        {
            return static_cast<bool>(input.read(static_cast<char*>(data), static_cast<std::streamsize>(size)));
        }
    }

    bool Analyze(const std::string& utf8Path, Peaks& peaks, std::string* error)
    {
        auto fail = [&](const char* message) {
            if (error)
            {
                *error = message;
            }
            return false;
        };
#if defined(__cpp_char8_t)
        const std::filesystem::path path(reinterpret_cast<const char8_t*>(utf8Path.c_str()));
#else
        const std::filesystem::path path = std::filesystem::u8path(utf8Path);
#endif
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return fail("cannot open");
        }
        char riff[4]{}, wave[4]{};
        uint32_t fileBytes = 0;
        if (!ReadExact(input, riff, 4) || std::memcmp(riff, "RIFF", 4) != 0 || !ReadExact(input, &fileBytes, 4) ||
            !ReadExact(input, wave, 4) || std::memcmp(wave, "WAVE", 4) != 0)
        {
            return fail("not a RIFF/WAVE file");
        }
        uint16_t formatTag = 0, channels = 0, bits = 0, blockAlign = 0;
        uint32_t sampleRate = 0;
        bool gotFormat = false;
        uint64_t dataBytes = 0;
        bool gotData = false;
        while (input && !gotData)
        {
            char id[4]{};
            uint32_t length = 0;
            if (!ReadExact(input, id, 4) || !ReadExact(input, &length, 4))
            {
                break;
            }
            if (std::memcmp(id, "fmt ", 4) == 0 && length >= 16)
            {
                uint8_t bytes[16]{};
                if (!ReadExact(input, bytes, 16))
                {
                    return fail("broken fmt chunk");
                }
                std::memcpy(&formatTag, bytes + 0, 2);
                std::memcpy(&channels, bytes + 2, 2);
                std::memcpy(&sampleRate, bytes + 4, 4);
                std::memcpy(&blockAlign, bytes + 12, 2);
                std::memcpy(&bits, bytes + 14, 2);
                input.seekg(static_cast<std::streamoff>(length - 16 + (length & 1u)), std::ios::cur);
                gotFormat = true;
            }
            else if (std::memcmp(id, "data", 4) == 0)
            {
                dataBytes = length;
                gotData = true;
            }
            else
            {
                input.seekg(static_cast<std::streamoff>(length + (length & 1u)), std::ios::cur);
            }
        }
        if (!gotFormat || !gotData)
        {
            return fail("missing fmt/data chunk");
        }
        const bool pcm16 = formatTag == 1 && bits == 16;
        const bool float32 = formatTag == 3 && bits == 32;
        if ((!pcm16 && !float32) || channels == 0 || sampleRate == 0 ||
            blockAlign != channels * (bits / 8))
        {
            return fail("unsupported WAV format (PCM16 / float32 only)");
        }
        const uint64_t frames = dataBytes / blockAlign;
        peaks = {};
        peaks.SampleRate = static_cast<int>(sampleRate);
        peaks.Channels = channels;
        peaks.Duration = static_cast<float>(static_cast<double>(frames) / sampleRate);
        const double framesPerBucket = static_cast<double>(sampleRate) / kBucketsPerSecond;
        const size_t buckets = static_cast<size_t>(std::ceil(frames / framesPerBucket));
        peaks.Min.assign(buckets, 0.0f);
        peaks.Max.assign(buckets, 0.0f);

        // 1 MB 単位で読み、フレームごとに全チャンネルの平均をピークに積む。
        std::vector<uint8_t> chunk(static_cast<size_t>(blockAlign) * 65536);
        uint64_t frame = 0;
        while (frame < frames && input)
        {
            const uint64_t count = std::min<uint64_t>(frames - frame, 65536);
            if (!ReadExact(input, chunk.data(), static_cast<size_t>(count * blockAlign)))
            {
                break;
            }
            for (uint64_t local = 0; local < count; ++local)
            {
                const uint8_t* sample = chunk.data() + local * blockAlign;
                float mixed = 0.0f;
                for (uint16_t channel = 0; channel < channels; ++channel)
                {
                    if (pcm16)
                    {
                        int16_t value;
                        std::memcpy(&value, sample + channel * 2, 2);
                        mixed += value / 32768.0f;
                    }
                    else
                    {
                        float value;
                        std::memcpy(&value, sample + channel * 4, 4);
                        mixed += std::isfinite(value) ? value : 0.0f;
                    }
                }
                mixed /= channels;
                const size_t bucket =
                    std::min(buckets - 1, static_cast<size_t>((frame + local) / framesPerBucket));
                peaks.Min[bucket] = std::min(peaks.Min[bucket], mixed);
                peaks.Max[bucket] = std::max(peaks.Max[bucket], mixed);
            }
            frame += count;
        }
        return true;
    }

    State Request(const std::string& utf8Path, std::shared_ptr<const Peaks>& peaks)
    {
        if (utf8Path.empty())
        {
            return State::None;
        }
        std::error_code error;
#if defined(__cpp_char8_t)
        const std::filesystem::path path(reinterpret_cast<const char8_t*>(utf8Path.c_str()));
#else
        const std::filesystem::path path = std::filesystem::u8path(utf8Path);
#endif
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto& entry = g_Cache[utf8Path];
        if (entry.Status == State::Loading)
        {
            return State::Loading;
        }
        // 状態確認は数フレームに 1 回で十分だが、stat は安価なので毎回行う。
        const auto writeTime = std::filesystem::last_write_time(path, error);
        const uintmax_t size = error ? 0 : std::filesystem::file_size(path, error);
        if (error)
        {
            entry.Status = State::Failed;
            entry.Result.reset();
            return State::Failed;
        }
        if (entry.Status != State::None && entry.WriteTime == writeTime && entry.Size == size)
        {
            peaks = entry.Result;
            return entry.Status;
        }
        entry.Status = State::Loading;
        entry.WriteTime = writeTime;
        entry.Size = size;
        std::thread([utf8Path]() {
            auto result = std::make_shared<Peaks>();
            const bool ok = Analyze(utf8Path, *result);
            std::lock_guard<std::mutex> workerLock(g_Mutex);
            auto found = g_Cache.find(utf8Path);
            if (found == g_Cache.end())
            {
                return;
            }
            found->second.Status = ok ? State::Ready : State::Failed;
            found->second.Result = ok ? std::shared_ptr<const Peaks>(std::move(result)) : nullptr;
        }).detach();
        return State::Loading;
    }

    bool Range(const Peaks& peaks, float t0, float t1, float& minimum, float& maximum)
    {
        if (peaks.Min.empty() || t1 <= 0.0f || t0 >= peaks.Duration)
        {
            return false;
        }
        const long long last = static_cast<long long>(peaks.Min.size()) - 1;
        long long begin = static_cast<long long>(std::floor(std::max(0.0f, t0) * kBucketsPerSecond));
        long long end = static_cast<long long>(std::ceil(std::max(0.0f, t1) * kBucketsPerSecond));
        begin = std::clamp(begin, 0ll, last);
        end = std::clamp(std::max(end, begin + 1), 1ll, last + 1);
        minimum = 0.0f;
        maximum = 0.0f;
        for (long long bucket = begin; bucket < end; ++bucket)
        {
            minimum = std::min(minimum, peaks.Min[static_cast<size_t>(bucket)]);
            maximum = std::max(maximum, peaks.Max[static_cast<size_t>(bucket)]);
        }
        return true;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        // 解析中のエントリは残す（ワーカーが結果を書き込む先）。
        for (auto it = g_Cache.begin(); it != g_Cache.end();)
        {
            it = it->second.Status == State::Loading ? std::next(it) : g_Cache.erase(it);
        }
    }
}
