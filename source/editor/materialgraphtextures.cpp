#include "pch.h"
#include "materialgraphtextures.h"
#include "materialmanager.h"
#include "proceduralio.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <objbase.h>

namespace MaterialGraphTextures
{
    namespace
    {
        constexpr int kMaxSize = 256;

        struct CpuTexture
        {
            int Width = 0;
            int Height = 0;
            std::vector<float> Pixels; // RGBA 線形
        };

        struct Entry
        {
            std::shared_ptr<const CpuTexture> Texture;
            std::future<std::shared_ptr<const CpuTexture>> Job;
            bool Failed = false;
        };

        std::mutex g_Mutex;
        std::unordered_map<AssetHandle, Entry> g_Entries;
        std::atomic<uint64_t> g_Generation{1};

        std::shared_ptr<const CpuTexture> Load(std::filesystem::path path)
        {
            // WIC を使うので、このワーカースレッドで COM を初期化する
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            struct Scope
            {
                bool Active;
                ~Scope()
                {
                    if (Active)
                    {
                        CoUninitialize();
                    }
                }
            } scope{SUCCEEDED(apartment)};

            ProcStudio::Image image;
            std::string error;
            if (path.empty() || !ProceduralIO::LoadImageFile(path, image, error) || image.Width <= 0 ||
                image.Height <= 0)
            {
                return nullptr;
            }
            auto result = std::make_shared<CpuTexture>();
            const float scale = (std::min)(1.0f, static_cast<float>(kMaxSize) / (std::max)(image.Width, image.Height));
            result->Width = (std::max)(1, static_cast<int>(image.Width * scale));
            result->Height = (std::max)(1, static_cast<int>(image.Height * scale));
            result->Pixels.resize(static_cast<size_t>(result->Width) * result->Height * 4);
            // ボックスフィルターで縮小（縮小しない場合は 1 画素ずつコピー）
            for (int y = 0; y < result->Height; ++y)
            {
                const int y0 = y * image.Height / result->Height;
                const int y1 = (std::max)(y0 + 1, (y + 1) * image.Height / result->Height);
                for (int x = 0; x < result->Width; ++x)
                {
                    const int x0 = x * image.Width / result->Width;
                    const int x1 = (std::max)(x0 + 1, (x + 1) * image.Width / result->Width);
                    float sum[4]{};
                    int count = 0;
                    for (int sy = y0; sy < y1; ++sy)
                    {
                        for (int sx = x0; sx < x1; ++sx)
                        {
                            const auto& p = image.Pixels[static_cast<size_t>(sy) * image.Width + sx];
                            sum[0] += p.X;
                            sum[1] += p.Y;
                            sum[2] += p.Z;
                            sum[3] += p.W;
                            ++count;
                        }
                    }
                    float* out = &result->Pixels[(static_cast<size_t>(y) * result->Width + x) * 4];
                    for (int c = 0; c < 4; ++c)
                    {
                        out[c] = sum[c] / static_cast<float>((std::max)(count, 1));
                    }
                }
            }
            return result;
        }

        std::shared_ptr<const CpuTexture> Acquire(AssetHandle texture)
        {
            if (texture == 0)
            {
                return nullptr;
            }
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto& entry = g_Entries[texture];
            if (entry.Texture)
            {
                return entry.Texture;
            }
            if (entry.Failed)
            {
                return nullptr;
            }
            if (entry.Job.valid())
            {
                if (entry.Job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                {
                    return nullptr;
                }
                entry.Texture = entry.Job.get();
                entry.Failed = !entry.Texture;
                ++g_Generation;
                return entry.Texture;
            }
            // パスの解決はメインスレッドで行い、読み込みだけをワーカーに渡す
            const auto path = MaterialManager::Registry().Resolve(texture);
            entry.Job = std::async(std::launch::async, Load, path);
            return nullptr;
        }
    } // namespace

    bool Sample(AssetHandle texture, float u, float v, float out[4])
    {
        const auto data = Acquire(texture);
        if (!data)
        {
            return false;
        }
        // 折り返し + 双線形補間
        const float fx = (u - floorf(u)) * data->Width - 0.5f;
        const float fy = (v - floorf(v)) * data->Height - 0.5f;
        const int x0 = static_cast<int>(floorf(fx));
        const int y0 = static_cast<int>(floorf(fy));
        const float tx = fx - x0;
        const float ty = fy - y0;
        auto texel = [&](int x, int y, int c) {
            x = ((x % data->Width) + data->Width) % data->Width;
            y = ((y % data->Height) + data->Height) % data->Height;
            return data->Pixels[(static_cast<size_t>(y) * data->Width + x) * 4 + c];
        };
        for (int c = 0; c < 4; ++c)
        {
            const float a = texel(x0, y0, c) + (texel(x0 + 1, y0, c) - texel(x0, y0, c)) * tx;
            const float b = texel(x0, y0 + 1, c) + (texel(x0 + 1, y0 + 1, c) - texel(x0, y0 + 1, c)) * tx;
            out[c] = a + (b - a) * ty;
        }
        return true;
    }

    bool IsReady(AssetHandle texture)
    {
        return Acquire(texture) != nullptr;
    }

    uint64_t Generation()
    {
        // 読み込み中のジョブの完了を拾う（Sample が呼ばれないフレームでも世代を進める）
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& [handle, entry] : g_Entries)
        {
            if (!entry.Texture && entry.Job.valid() &&
                entry.Job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                entry.Texture = entry.Job.get();
                entry.Failed = !entry.Texture;
                ++g_Generation;
            }
        }
        return g_Generation.load();
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& [handle, entry] : g_Entries)
        {
            if (entry.Job.valid())
            {
                entry.Job.wait();
            }
        }
        g_Entries.clear();
        ++g_Generation;
    }
} // namespace MaterialGraphTextures
