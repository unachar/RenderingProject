#include "pch.h"
#include "materialgraphbake.h"
#include "materialgraphtextures.h"
#include "proceduralio.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#ifdef _WIN32
#include <objbase.h>
#endif

namespace MaterialGraphBake
{
    namespace
    {
        using Kind = MaterialNodeKind;
        using Pin = MaterialOutputPin;
        using MaterialGraphEval::Context;
        using MaterialGraphEval::Evaluator;
        using MaterialGraphEval::Value;

        size_t PinIndex(Pin pin)
        {
            return static_cast<size_t>(pin);
        }

        uint32_t Bit(MaterialTextureSlot slot)
        {
            return 1u << static_cast<uint32_t>(slot);
        }

        float Saturate(float v)
        {
            return std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f);
        }

        Value Expand(Value value, int count)
        {
            if (value.N == 1)
            {
                value.V[1] = value.V[2] = value.V[0];
                value.V[3] = 1.0f;
            }
            else if (value.N < 4)
            {
                value.V[3] = 1.0f;
            }
            value.N = count;
            return value;
        }
    } // namespace

    const char* SlotName(MaterialTextureSlot slot)
    {
        switch (slot)
        {
            case MaterialTextureSlot::BaseColor:
                return "BaseColor";
            case MaterialTextureSlot::Normal:
                return "Normal";
            case MaterialTextureSlot::MetallicRoughness:
                return "MetallicRoughness";
            case MaterialTextureSlot::AO:
                return "AO";
            case MaterialTextureSlot::Emissive:
                return "Emissive";
            case MaterialTextureSlot::Height:
                return "Height";
            default:
                return "Unknown";
        }
    }

    bool Evaluate(const Request& request, const MaterialGraphEval::TextureSampler& sampler, Images& images,
                  std::string& error)
    {
        const MaterialGraph& graph = request.Graph;
        const MaterialGraphNode* output = nullptr;
        for (const auto& node : graph.Nodes)
        {
            if (node.Kind == Kind::Output)
            {
                output = &node;
                break;
            }
        }
        if (!output || output->InputPins.size() < PinIndex(Pin::Count))
        {
            error = "Material Output ノードがありません。";
            return false;
        }
        if (!graph.Validate(error))
        {
            return false;
        }
        const int resolution = std::clamp(request.Resolution, 16, 4096);
        images.Resolution = resolution;
        images.Mask = request.Mask;
        images.EmissivePeak = 1.0f;
        const size_t pixelCount = static_cast<size_t>(resolution) * resolution;
        for (size_t slot = 0; slot < kSlotCount; ++slot)
        {
            images.Pixels[slot].clear();
            if (request.Mask & (1u << slot))
            {
                images.Pixels[slot].assign(pixelCount, {0.0f, 0.0f, 0.0f, 1.0f});
            }
        }
        if (request.Mask == 0)
        {
            return true;
        }

        MaterialGraphEval::Topology topology;
        MaterialGraphEval::BuildTopology(graph, topology);
        // 旧グラフの「Color サンプラーの法線マップ」は格納値に戻してから使う（プレビューと同じ）
        const auto* normalSource = MaterialGraphEval::ResolveSource(graph, *output, PinIndex(Pin::Normal));
        const bool legacyColorNormal = normalSource && normalSource->Kind == Kind::TextureSample &&
                                       static_cast<int>(normalSource->Value[0]) ==
                                           static_cast<int>(MaterialSamplerType::Color);

        int threads = request.Threads;
        if (threads <= 0)
        {
            threads = (std::max)(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
        }
        threads = std::clamp(threads, 1, 32);
        threads = (std::min)(threads, resolution);
        std::atomic<int> nextRow{0};
        std::vector<float> peaks(static_cast<size_t>(threads), 0.0f);

        const auto worker = [&](int index) {
            Evaluator evaluator(graph, topology, sampler);
            float peak = 0.0f;
            for (int y = nextRow.fetch_add(1); y < resolution; y = nextRow.fetch_add(1))
            {
                for (int x = 0; x < resolution; ++x)
                {
                    Context context;
                    context.UV[0] = (x + 0.5f) / resolution;
                    context.UV[1] = (y + 0.5f) / resolution;
                    context.Position[0] = context.UV[0] * 2.0f - 1.0f;
                    context.Position[1] = 1.0f - context.UV[1] * 2.0f;
                    context.Position[2] = 0.0f;
                    context.Normal[0] = context.Normal[1] = 0.0f;
                    context.Normal[2] = -1.0f;
                    context.Camera[0] = context.Camera[1] = 0.0f;
                    context.Camera[2] = -1.0f; // 正面から見た値（視線依存の式）
                    context.Time = request.Time;
                    context.PixelSize = 1.0f / resolution;
                    evaluator.Reset(context);
                    const size_t pixel = static_cast<size_t>(y) * resolution + x;
                    const auto in = [&](Pin pin) {
                        return evaluator.Input(*output, PinIndex(pin), 0);
                    };
                    if (request.Mask & Bit(MaterialTextureSlot::BaseColor))
                    {
                        const Value color = Expand(in(Pin::BaseColor), 4);
                        float alpha = Saturate(color.V[3]);
                        if (request.Alpha == MaterialGraphPreview::AlphaSource::Opacity)
                        {
                            alpha *= Saturate(in(Pin::Opacity).V[0]);
                        }
                        else if (request.Alpha == MaterialGraphPreview::AlphaSource::OpacityMask)
                        {
                            alpha = Saturate(in(Pin::OpacityMask).V[0]);
                        }
                        images.Pixels[static_cast<size_t>(MaterialTextureSlot::BaseColor)][pixel] = {
                            Saturate(color.V[0]), Saturate(color.V[1]), Saturate(color.V[2]), alpha};
                    }
                    if (request.Mask & Bit(MaterialTextureSlot::Normal))
                    {
                        Value normal = MaterialGraphEval::Promote(in(Pin::Normal), 3);
                        float n[3] = {normal.V[0], normal.V[1], normal.V[2]};
                        if (legacyColorNormal)
                        {
                            for (float& c : n)
                            {
                                c = MaterialGraphEval::LinearToSrgb(c) * 2.0f - 1.0f;
                            }
                        }
                        const float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        if (length > 1e-6f && std::isfinite(length))
                        {
                            for (float& c : n)
                            {
                                c /= length;
                            }
                        }
                        else
                        {
                            n[0] = n[1] = 0.0f;
                            n[2] = 1.0f;
                        }
                        images.Pixels[static_cast<size_t>(MaterialTextureSlot::Normal)][pixel] = {
                            n[0] * 0.5f + 0.5f, n[1] * 0.5f + 0.5f, n[2] * 0.5f + 0.5f, 1.0f};
                    }
                    if (request.Mask & Bit(MaterialTextureSlot::MetallicRoughness))
                    {
                        images.Pixels[static_cast<size_t>(MaterialTextureSlot::MetallicRoughness)][pixel] = {
                            Saturate(in(Pin::Metallic).V[0]), Saturate(in(Pin::Roughness).V[0]), 0.0f, 1.0f};
                    }
                    if (request.Mask & Bit(MaterialTextureSlot::AO))
                    {
                        const float ao = Saturate(in(Pin::AO).V[0]);
                        images.Pixels[static_cast<size_t>(MaterialTextureSlot::AO)][pixel] = {ao, ao, ao, 1.0f};
                    }
                    if (request.Mask & Bit(MaterialTextureSlot::Emissive))
                    {
                        const Value emissive = Expand(in(Pin::Emissive), 4);
                        std::array<float, 4> value{};
                        for (int c = 0; c < 3; ++c)
                        {
                            value[c] = std::isfinite(emissive.V[c]) ? (std::max)(emissive.V[c], 0.0f) : 0.0f;
                            peak = (std::max)(peak, value[c]);
                        }
                        value[3] = 1.0f;
                        images.Pixels[static_cast<size_t>(MaterialTextureSlot::Emissive)][pixel] = value;
                    }
                    if (request.Mask & Bit(MaterialTextureSlot::Height))
                    {
                        const float h = Saturate(in(Pin::Height).V[0]);
                        images.Pixels[static_cast<size_t>(MaterialTextureSlot::Height)][pixel] = {h, h, h, 1.0f};
                    }
                }
            }
            peaks[static_cast<size_t>(index)] = peak;
        };
        std::vector<std::thread> pool;
        for (int i = 1; i < threads; ++i)
        {
            pool.emplace_back(worker, i);
        }
        worker(0);
        for (auto& thread : pool)
        {
            thread.join();
        }
        // Emissive は最大値で正規化して保存（強度はマテリアルの EmissiveIntensity に入れる）
        if (request.Mask & Bit(MaterialTextureSlot::Emissive))
        {
            const float peak = (std::min)(*std::max_element(peaks.begin(), peaks.end()), 65504.0f);
            images.EmissivePeak = peak;
            auto& pixels = images.Pixels[static_cast<size_t>(MaterialTextureSlot::Emissive)];
            for (auto& value : pixels)
            {
                for (int c = 0; c < 3; ++c)
                {
                    value[c] = peak > 1e-6f ? Saturate(value[c] / peak) : 0.0f;
                }
            }
        }
        return true;
    }

    Result Run(const Request& request)
    {
        const auto start = std::chrono::steady_clock::now();
        Result result;
        result.Signature = request.Signature;
        result.Resolution = std::clamp(request.Resolution, 16, 4096);
#ifdef _WIN32
        // WIC（PNG の読み書き）を使うので、このワーカースレッドで COM を初期化する
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
#endif
        // 参照テクスチャをフル解像度で読む（ベイク解像度の 2 倍まで。プレビュー用の 256px キャッシュは使わない）
        std::unordered_map<AssetHandle, std::shared_ptr<const MaterialGraphTextures::CpuTexture>> textures;
        const int textureLimit = std::clamp(result.Resolution * 2, 256, 4096);
        for (const auto& [handle, path] : request.TexturePaths)
        {
            auto texture = MaterialGraphTextures::LoadForBake(path, textureLimit);
            if (!texture)
            {
                result.Warnings.push_back("テクスチャを読み込めません: " + ProceduralIO::ToUtf8(path));
                continue;
            }
            textures[handle] = std::move(texture);
        }
        const MaterialGraphEval::TextureSampler sampler = [&textures](AssetHandle handle, float u, float v, bool raw,
                                                                      float out[4]) {
            const auto found = textures.find(handle);
            if (found == textures.end())
            {
                return false;
            }
            MaterialGraphTextures::SampleTexture(*found->second, u, v, raw, out);
            return true;
        };
        Images images;
        if (!Evaluate(request, sampler, images, result.Error))
        {
            return result;
        }
        std::error_code filesystemError;
        std::filesystem::create_directories(request.Directory, filesystemError);
        if (filesystemError)
        {
            result.Error = "ベイク先のフォルダを作れません: " + filesystemError.message();
            return result;
        }
        for (size_t slot = 0; slot < kSlotCount; ++slot)
        {
            if (!(request.Mask & (1u << slot)))
            {
                continue;
            }
            const auto slotKind = static_cast<MaterialTextureSlot>(slot);
            ProcStudio::Image image;
            image.Width = images.Resolution;
            image.Height = images.Resolution;
            image.Pixels.resize(images.Pixels[slot].size());
            for (size_t i = 0; i < image.Pixels.size(); ++i)
            {
                const auto& p = images.Pixels[slot][i];
                image.Pixels[i] = {p[0], p[1], p[2], p[3]};
            }
            images.Pixels[slot].clear();
            images.Pixels[slot].shrink_to_fit();
            ProceduralIO::Encoding encoding = ProceduralIO::Encoding::Linear;
            if (slotKind == MaterialTextureSlot::BaseColor || slotKind == MaterialTextureSlot::Emissive)
            {
                encoding = ProceduralIO::Encoding::Color; // 線形 → sRGB 8 ビット（通常のカラーテクスチャと同じ）
            }
            else if (slotKind == MaterialTextureSlot::Height)
            {
                encoding = ProceduralIO::Encoding::Height; // 16 ビット
            }
            const std::string name = request.Stem + "_" + SlotName(slotKind);
            const auto path = request.Directory / (name + ".png");
            const auto temporary = request.Directory / (name + ".baking.png");
            std::string error;
            if (!ProceduralIO::SaveImageFile(temporary, image, encoding, error))
            {
                result.Error = std::string(SlotName(slotKind)) + ": " + error;
                std::filesystem::remove(temporary, filesystemError);
                return result;
            }
            // 書き終えてから置き換える（エンジンが読み込み途中のファイルを読まないように）
            std::filesystem::rename(temporary, path, filesystemError);
            if (filesystemError)
            {
                std::filesystem::remove(path, filesystemError);
                std::filesystem::rename(temporary, path, filesystemError);
            }
            if (filesystemError)
            {
                result.Error = std::string(SlotName(slotKind)) + ": " + filesystemError.message();
                std::filesystem::remove(temporary, filesystemError);
                return result;
            }
            result.Files[slot] = path;
        }
        result.Mask = request.Mask;
        result.EmissivePeak = images.EmissivePeak;
        result.Success = true;
        result.Seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return result;
    }
} // namespace MaterialGraphBake
