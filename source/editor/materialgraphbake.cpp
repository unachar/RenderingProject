#include "pch.h"
#include "backgroundtasks.h"
#include "materialgraphbake.h"
#include "materialgraphtextures.h"
#include "proceduralio.h"
#include "materialmanager.h"
#include "texturemanager.h"
#include "world.h"
#include <algorithm>
#include <future>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
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
        const bool legacyColorNormal = normalSource && normalSource->SplitPins.empty() &&
                                       normalSource->Kind == Kind::TextureSample &&
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
        // ライブ更新はベイク時の最大値（マテリアルの強度）に合わせ、明るさがフレームごとに揺れないようにする
        if (request.Mask & Bit(MaterialTextureSlot::Emissive))
        {
            const float peak = request.EmissiveScale > 0.0f
                                   ? request.EmissiveScale
                                   : (std::min)(*std::max_element(peaks.begin(), peaks.end()), 65504.0f);
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
        Images images;
        // ---- GPU で評価（エンジンの描画と同じ命令列）。待ちきれない / 失敗したときは CPU で評価し直す
        if (request.Gpu)
        {
            std::array<MaterialGraphGpu::Texels, 6> slices;
            if (MaterialGraphGpu::Wait(request.Gpu, 30.0, slices))
            {
                images.Resolution = result.Resolution;
                images.Mask = request.Mask;
                images.EmissivePeak = 1.0f;
                for (size_t slot = 0; slot < kSlotCount && slot < slices.size(); ++slot)
                {
                    if (request.Mask & (1u << slot))
                    {
                        images.Pixels[slot] = std::move(slices[slot]);
                    }
                }
                if (request.Mask & Bit(MaterialTextureSlot::Emissive))
                {
                    auto& pixels = images.Pixels[static_cast<size_t>(MaterialTextureSlot::Emissive)];
                    float peak = request.EmissiveScale;
                    if (peak <= 0.0f)
                    {
                        peak = 0.0f;
                        for (const auto& value : pixels)
                        {
                            peak = (std::max)({peak, value[0], value[1], value[2]});
                        }
                        peak = (std::min)(peak, 65504.0f);
                    }
                    images.EmissivePeak = peak;
                    for (auto& value : pixels)
                    {
                        for (int c = 0; c < 3; ++c)
                        {
                            value[c] = peak > 1e-6f ? Saturate(value[c] / peak) : 0.0f;
                        }
                    }
                }
                result.Gpu = true;
            }
            else
            {
                result.Warnings.push_back("GPU で評価できなかったため CPU で評価しました");
            }
        }
        if (!result.Gpu)
        {
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
            if (!Evaluate(request, sampler, images, result.Error))
            {
                return result;
            }
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

    namespace
    {
        using TextureCache = std::unordered_map<AssetHandle, std::shared_ptr<const MaterialGraphTextures::CpuTexture>>;

        void LoadTextures(const std::unordered_map<AssetHandle, std::filesystem::path>& paths, int limit,
                          TextureCache& textures, std::vector<std::string>* warnings)
        {
            for (const auto& [handle, path] : paths)
            {
                if (textures.contains(handle))
                {
                    continue;
                }
                auto texture = MaterialGraphTextures::LoadForBake(path, limit);
                if (!texture)
                {
                    if (warnings)
                    {
                        warnings->push_back("テクスチャを読み込めません: " + ProceduralIO::ToUtf8(path));
                    }
                    continue;
                }
                textures[handle] = std::move(texture);
            }
        }

        MaterialGraphEval::TextureSampler MakeSampler(const TextureCache& textures)
        {
            return [&textures](AssetHandle handle, float u, float v, bool raw, float out[4]) {
                const auto found = textures.find(handle);
                if (found == textures.end())
                {
                    return false;
                }
                MaterialGraphTextures::SampleTexture(*found->second, u, v, raw, out);
                return true;
            };
        }

        // グラフが参照するテクスチャ（Texture Sample / Convert 系 / 変換済み Convert To Texture）
        std::unordered_map<AssetHandle, std::filesystem::path> GraphTexturePaths(const MaterialGraph& graph)
        {
            std::unordered_map<AssetHandle, std::filesystem::path> paths;
            for (const auto& node : graph.Nodes)
            {
                if (UsesNodeTexture(node.Kind) &&
                    node.Texture != 0 && !paths.contains(node.Texture))
                {
                    const auto path = MaterialManager::Registry().Resolve(node.Texture);
                    if (!path.empty())
                    {
                        paths[node.Texture] = path;
                    }
                }
            }
            return paths;
        }
    } // namespace

    namespace
    {
        void MakeSeamless(ProcStudio::Image& image)
        {
            const int width = image.Width;
            const int height = image.Height;
            if (width < 4 || height < 4)
            {
                return;
            }
            // 平均（分散を保つ混ぜ方の中心）
            double mean[4]{};
            for (const auto& p : image.Pixels)
            {
                mean[0] += p.X;
                mean[1] += p.Y;
                mean[2] += p.Z;
                mean[3] += p.W;
            }
            for (double& m : mean)
            {
                m /= static_cast<double>(image.Pixels.size());
            }
            const auto source = image.Pixels;
            const auto at = [&](int x, int y) -> const ProcStudio::Float4& {
                return source[static_cast<size_t>((y % height + height) % height) * width + (x % width + width) % width];
            };
            constexpr float kPi = 3.14159265f;
            for (int y = 0; y < height; ++y)
            {
                const float wy = std::pow(std::sin(kPi * (y + 0.5f) / height), 2.0f); // 中央 1、端 0
                for (int x = 0; x < width; ++x)
                {
                    const float wx = std::pow(std::sin(kPi * (x + 0.5f) / width), 2.0f);
                    const float w[4] = {wx * wy, (1 - wx) * wy, wx * (1 - wy), (1 - wx) * (1 - wy)};
                    const ProcStudio::Float4* p[4] = {&at(x, y), &at(x + width / 2, y), &at(x, y + height / 2),
                                                      &at(x + width / 2, y + height / 2)};
                    const float norm = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2] + w[3] * w[3]);
                    float out[4]{};
                    for (int i = 0; i < 4; ++i)
                    {
                        const float c[4] = {p[i]->X, p[i]->Y, p[i]->Z, p[i]->W};
                        for (int k = 0; k < 4; ++k)
                        {
                            out[k] += w[i] * (c[k] - static_cast<float>(mean[k]));
                        }
                    }
                    auto& target = image.Pixels[static_cast<size_t>(y) * width + x];
                    target.X = Saturate(out[0] / norm + static_cast<float>(mean[0]));
                    target.Y = Saturate(out[1] / norm + static_cast<float>(mean[1]));
                    target.Z = Saturate(out[2] / norm + static_cast<float>(mean[2]));
                    target.W = Saturate(out[3] / norm + static_cast<float>(mean[3]));
                }
            }
        }
    } // namespace

    uint32_t TimeDependentSlots(const MaterialGraph& graph, MaterialGraphPreview::AlphaSource alpha)
    {
        const MaterialGraphNode* output = nullptr;
        for (const auto& node : graph.Nodes)
        {
            if (node.Kind == Kind::Output)
            {
                output = &node;
                break;
            }
        }
        if (!output)
        {
            return 0;
        }
        MaterialGraphEval::Topology topology;
        MaterialGraphEval::BuildTopology(graph, topology);
        if (!topology.Animated)
        {
            return 0;
        }
        const auto timed = [&](Pin pin) {
            const auto* source = MaterialGraphEval::ResolveSource(graph, *output, PinIndex(pin));
            if (!source)
            {
                return false;
            }
            const auto found = topology.Index.find(source->Id);
            return found != topology.Index.end() && topology.TimeDependent[found->second] != 0;
        };
        uint32_t mask = 0;
        if (timed(Pin::BaseColor) || (alpha == MaterialGraphPreview::AlphaSource::Opacity && timed(Pin::Opacity)) ||
            (alpha == MaterialGraphPreview::AlphaSource::OpacityMask && timed(Pin::OpacityMask)))
        {
            mask |= Bit(MaterialTextureSlot::BaseColor);
        }
        mask |= timed(Pin::Normal) ? Bit(MaterialTextureSlot::Normal) : 0;
        mask |= (timed(Pin::Metallic) || timed(Pin::Roughness)) ? Bit(MaterialTextureSlot::MetallicRoughness) : 0;
        mask |= timed(Pin::AO) ? Bit(MaterialTextureSlot::AO) : 0;
        mask |= timed(Pin::Emissive) ? Bit(MaterialTextureSlot::Emissive) : 0;
        mask |= timed(Pin::Height) ? Bit(MaterialTextureSlot::Height) : 0;
        return mask;
    }

    NodeResult RunNode(const NodeRequest& request)
    {
        const auto start = std::chrono::steady_clock::now();
        NodeResult result;
        result.Node = request.Node;
#ifdef _WIN32
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
        const auto* node = request.Graph.FindNode(request.Node);
        if (!node || (node->Kind != Kind::ConvertToTexture && node->Kind != Kind::ConvertToSolid))
        {
            result.Error = "Convert To Texture / Convert To Solid ノードが見つかりません";
            return result;
        }
        if (!request.Graph.Validate(result.Error))
        {
            return result;
        }
        const int resolution = std::clamp(request.Resolution, 16, 4096);
        ProcStudio::Image image;
        image.Width = resolution;
        image.Height = resolution;
        image.Pixels.resize(static_cast<size_t>(resolution) * resolution);
        bool gpuDone = false;
        if (request.Gpu)
        {
            std::array<MaterialGraphGpu::Texels, 6> slices;
            if (MaterialGraphGpu::Wait(request.Gpu, 30.0, slices) && slices[0].size() == image.Pixels.size())
            {
                for (size_t i = 0; i < image.Pixels.size(); ++i)
                {
                    const auto& p = slices[0][i];
                    image.Pixels[i] = {p[0], p[1], p[2], p[3]};
                }
                gpuDone = true;
            }
        }
        TextureCache textures;
        std::vector<std::string> warnings;
        if (!gpuDone)
        {
            LoadTextures(request.TexturePaths, std::clamp(resolution * 2, 256, 4096), textures, &warnings);
        }
        const auto sampler = MakeSampler(textures);
        MaterialGraphEval::Topology topology;
        MaterialGraphEval::BuildTopology(request.Graph, topology);
        const int threads =
            gpuDone ? 1 : std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 32);
        std::atomic<int> nextRow{gpuDone ? resolution : 0}; // GPU で済んだら CPU では評価しない
        const auto worker = [&]() {
            Evaluator evaluator(request.Graph, topology, sampler);
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
                    context.Camera[2] = -1.0f;
                    context.Time = request.Time;
                    context.PixelSize = 1.0f / resolution;
                    evaluator.Reset(context);
                    Value value = Expand(evaluator.Input(*node, 0, 0), 4);
                    if (request.Sampler == MaterialSamplerType::Normal)
                    {
                        for (int c = 0; c < 3; ++c)
                        {
                            value.V[c] = value.V[c] * 0.5f + 0.5f;
                        }
                        value.V[3] = 1.0f;
                    }
                    image.Pixels[static_cast<size_t>(y) * resolution + x] = {Saturate(value.V[0]), Saturate(value.V[1]),
                                                                            Saturate(value.V[2]), Saturate(value.V[3])};
                }
            }
        };
        std::vector<std::thread> pool;
        for (int i = 1; i < threads; ++i)
        {
            pool.emplace_back(worker);
        }
        worker();
        for (auto& thread : pool)
        {
            thread.join();
        }
        // Convert To Solid：元画像を継ぎ目なしにする（ストキャスティック / トライプラナーは画像を折り返して読むため、
        // 端の不連続がそのまま縞になる）。半周期ずらした 4 枚を、端で 0 になる重みで混ぜる（分散を保つ混ぜ方）
        if (node->Kind == Kind::ConvertToSolid)
        {
            MakeSeamless(image);
        }
        std::error_code filesystemError;
        std::filesystem::create_directories(request.File.parent_path(), filesystemError);
        const auto temporary = std::filesystem::path(request.File).replace_extension(".baking.png");
        const auto encoding = request.Sampler == MaterialSamplerType::Color ? ProceduralIO::Encoding::Color
                                                                            : ProceduralIO::Encoding::Linear;
        if (!ProceduralIO::SaveImageFile(temporary, image, encoding, result.Error))
        {
            std::filesystem::remove(temporary, filesystemError);
            return result;
        }
        std::filesystem::rename(temporary, request.File, filesystemError);
        if (filesystemError)
        {
            std::filesystem::remove(request.File, filesystemError);
            std::filesystem::rename(temporary, request.File, filesystemError);
        }
        if (filesystemError)
        {
            result.Error = filesystemError.message();
            std::filesystem::remove(temporary, filesystemError);
            return result;
        }
        result.File = request.File;
        result.Success = true;
        result.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return result;
    }
} // namespace MaterialGraphBake

namespace MaterialGraphLive
{
    namespace
    {
        using MaterialGraphBake::kSlotCount;
        using Alpha = MaterialGraphPreview::AlphaSource;

        struct Source
        {
            MaterialGraph Graph;
            std::array<std::filesystem::path, kSlotCount> Files;
            uint32_t Mask = 0; // 時間に依存し、ベイク済みのスロット
            float EmissivePeak = 1.0f;
            Alpha AlphaMode = Alpha::BaseColor;
            std::unordered_map<AssetHandle, std::filesystem::path> TexturePaths;
            uint64_t Signature = 0;
        };

        struct Entry
        {
            std::shared_ptr<const Source> Data;
            std::shared_ptr<std::unordered_map<AssetHandle, std::shared_ptr<const MaterialGraphTextures::CpuTexture>>>
                Textures;
            std::future<double> Job;
            double NextTime = 0.0;
            double LastSeconds = 0.0;
            bool FromEditor = false;
        };

        std::unordered_map<AssetHandle, Entry> g_Entries;
        std::unordered_map<AssetHandle, bool> g_Checked; // 自動検出済みのマテリアル（true = グラフあり）
        double g_NextDiscovery = 0.0;

        uint64_t Mix(uint64_t hash, uint64_t value)
        {
            return (hash ^ value) * 1099511628211ull;
        }

        uint64_t Signature(const MaterialGraph& graph, const std::array<std::filesystem::path, kSlotCount>& files,
                           float peak, Alpha alpha)
        {
            uint64_t hash = 14695981039346656037ull;
            for (const unsigned char c : graph.Serialize())
            {
                hash = Mix(hash, c);
            }
            for (const auto& file : files)
            {
                for (const unsigned char c : file.generic_string())
                {
                    hash = Mix(hash, c);
                }
                hash = Mix(hash, 0xFF);
            }
            uint32_t bits = 0;
            std::memcpy(&bits, &peak, sizeof(bits));
            hash = Mix(hash, bits);
            return Mix(hash, static_cast<uint64_t>(alpha) + 1);
        }

        void Set(AssetHandle material, const MaterialGraph& graph,
                 const std::array<std::filesystem::path, kSlotCount>& files, float emissivePeak, Alpha alpha,
                 bool fromEditor)
        {
            const uint64_t signature = Signature(graph, files, emissivePeak, alpha);
            auto& entry = g_Entries[material];
            entry.FromEditor |= fromEditor;
            if (entry.Data && entry.Data->Signature == signature)
            {
                return;
            }
            auto source = std::make_shared<Source>();
            source->Graph = graph;
            source->Files = files;
            source->EmissivePeak = emissivePeak > 1e-6f ? emissivePeak : 1.0f;
            source->AlphaMode = alpha;
            source->Signature = signature;
            uint32_t baked = 0;
            for (size_t slot = 0; slot < kSlotCount; ++slot)
            {
                baked |= files[slot].empty() ? 0u : (1u << slot);
            }
            source->Mask = MaterialGraphBake::TimeDependentSlots(graph, alpha) & baked;
            source->TexturePaths = MaterialGraphBake::GraphTexturePaths(graph);
            const bool sameTextures = entry.Data && entry.Data->TexturePaths == source->TexturePaths;
            entry.Data = std::move(source);
            if (!sameTextures || !entry.Textures)
            {
                entry.Textures = std::make_shared<
                    std::unordered_map<AssetHandle, std::shared_ptr<const MaterialGraphTextures::CpuTexture>>>();
            }
            entry.NextTime = 0.0;
        }

        // 1 回分の評価 → テクスチャへ書き込み（ワーカースレッド）
        double Evaluate(std::shared_ptr<const Source> source,
                        std::shared_ptr<std::unordered_map<AssetHandle,
                                                           std::shared_ptr<const MaterialGraphTextures::CpuTexture>>>
                            textures,
                        float time)
        {
            const auto start = std::chrono::steady_clock::now();
#ifdef _WIN32
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
            const auto& settings = source->Graph.Settings;
            const int resolution = std::clamp(settings.LiveResolution, 32, 1024);
            MaterialGraphBake::LoadTextures(source->TexturePaths, std::clamp(resolution * 2, 256, 2048), *textures, nullptr);
            MaterialGraphBake::Request request;
            request.Graph = source->Graph;
            request.Mask = source->Mask;
            request.Alpha = source->AlphaMode;
            request.Resolution = resolution;
            request.Time = time;
            request.EmissiveScale = source->EmissivePeak;
            request.Threads = (std::max)(1, static_cast<int>(std::thread::hardware_concurrency()) / 4);
            MaterialGraphBake::Images images;
            std::string error;
            const auto sampler = MaterialGraphBake::MakeSampler(*textures);
            if (!MaterialGraphBake::Evaluate(request, sampler, images, error))
            {
                return -1.0;
            }
            for (size_t slot = 0; slot < kSlotCount; ++slot)
            {
                if (!(source->Mask & (1u << slot)) || images.Pixels[slot].empty())
                {
                    continue;
                }
                const auto kind = static_cast<MaterialTextureSlot>(slot);
                const bool color = kind == MaterialTextureSlot::BaseColor || kind == MaterialTextureSlot::Emissive;
                const bool wide = kind == MaterialTextureSlot::Height; // ベイクと同じ 16 ビット
                const auto& pixels = images.Pixels[slot];
                std::vector<uint8_t> bytes(pixels.size() * 4 * (wide ? 2 : 1));
                for (size_t i = 0; i < pixels.size(); ++i)
                {
                    for (int c = 0; c < 4; ++c)
                    {
                        float v = std::clamp(std::isfinite(pixels[i][c]) ? pixels[i][c] : 0.0f, 0.0f, 1.0f);
                        if (color && c < 3)
                        {
                            v = MaterialGraphEval::LinearToSrgb(v);
                        }
                        if (wide)
                        {
                            const uint16_t value = static_cast<uint16_t>(std::lround(v * 65535.0f));
                            std::memcpy(&bytes[(i * 4 + c) * 2], &value, sizeof(value));
                        }
                        else
                        {
                            bytes[i * 4 + c] = static_cast<uint8_t>(std::lround(v * 255.0f));
                        }
                    }
                }
                TextureManager::UpdateDynamicPixels(source->Files[slot], resolution, resolution, wide ? 2 : 1, color,
                                                    std::move(bytes));
            }
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }

        Alpha AlphaFor(const MaterialGraph& graph)
        {
            const int mode = graph.Settings.Mode >= 0 ? graph.Settings.Mode : graph.Settings.Surface;
            if (mode == static_cast<int>(MaterialGraphMode::Masked))
            {
                return Alpha::OpacityMask;
            }
            if (mode == static_cast<int>(MaterialGraphMode::Translucent) || mode == static_cast<int>(MaterialGraphMode::Glass))
            {
                return Alpha::Opacity;
            }
            return Alpha::BaseColor;
        }

        // エディターで開いていないマテリアル：シーンで使われているマテリアルの .graph.json を読み、Time を使っていれば登録する
        void Discover()
        {
            std::unordered_map<AssetHandle, bool> seen;
            for (EntityID entity : World::GetView<MaterialComponent>())
            {
                const auto& component = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
                for (size_t slot = 0; slot < component.MaterialSlots.size(); ++slot)
                {
                    const AssetHandle handle = component.MaterialSlots[slot];
                    if (handle == 0 || seen.contains(handle) || g_Checked.contains(handle))
                    {
                        seen[handle] = true;
                        continue;
                    }
                    seen[handle] = true;
                    g_Checked[handle] = false;
                    const auto materialPath = MaterialManager::Registry().Resolve(handle);
                    if (materialPath.empty())
                    {
                        continue;
                    }
                    auto graphPath = materialPath;
                    graphPath += ".graph.json";
                    std::error_code error;
                    if (!std::filesystem::exists(graphPath, error))
                    {
                        continue;
                    }
                    MaterialGraph graph;
                    std::string loadError;
                    if (!MaterialGraph::Load(graphPath, graph, loadError) || !graph.Settings.LiveUpdate)
                    {
                        continue;
                    }
                    const auto* runtime = MaterialManager::FindRuntime(handle);
                    if (!runtime)
                    {
                        continue;
                    }
                    // ベイク済み（<マテリアル名>_GraphBake フォルダの）テクスチャだけを書き換える
                    const auto bakeDirectory = materialPath.parent_path() / (materialPath.stem().string() + "_GraphBake");
                    std::array<std::filesystem::path, kSlotCount> files{};
                    for (size_t textureSlot = 0; textureSlot < kSlotCount; ++textureSlot)
                    {
                        const auto texture = runtime->Editing.Textures[textureSlot];
                        const auto path = texture ? MaterialManager::Registry().Resolve(texture) : std::filesystem::path{};
                        if (!path.empty() && path.parent_path().lexically_normal() == bakeDirectory.lexically_normal())
                        {
                            files[textureSlot] = path;
                        }
                    }
                    const Alpha alpha = AlphaFor(graph);
                    if ((MaterialGraphBake::TimeDependentSlots(graph, alpha) & [&] {
                            uint32_t mask = 0;
                            for (size_t i = 0; i < kSlotCount; ++i)
                            {
                                mask |= files[i].empty() ? 0u : (1u << i);
                            }
                            return mask;
                        }()) == 0)
                    {
                        continue;
                    }
                    g_Checked[handle] = true;
                    if (!g_Entries.contains(handle) || !g_Entries[handle].FromEditor)
                    {
                        Set(handle, graph, files, runtime->Editing.EmissiveIntensity, alpha, false);
                    }
                }
            }
        }
    } // namespace

    void Register(AssetHandle material, const MaterialGraph& graph,
                  const std::array<std::filesystem::path, MaterialGraphBake::kSlotCount>& files, float emissivePeak,
                  MaterialGraphPreview::AlphaSource alpha)
    {
        Set(material, graph, files, emissivePeak, alpha, true);
    }

    void Unregister(AssetHandle material)
    {
        const auto found = g_Entries.find(material);
        if (found == g_Entries.end())
        {
            return;
        }
        if (found->second.Job.valid())
        {
            found->second.Job.wait();
        }
        g_Entries.erase(found);
        g_Checked.erase(material);
    }

    void Update(double time)
    {
        if (time >= g_NextDiscovery)
        {
            g_NextDiscovery = time + 2.0;
            Discover();
        }
        for (auto& [handle, entry] : g_Entries)
        {
            if (entry.Job.valid())
            {
                if (entry.Job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                {
                    continue; // 前回の評価中（追いつかないときは自然に間引かれる）
                }
                entry.LastSeconds = entry.Job.get();
            }
            // 描画時評価（GPU）が動いているマテリアルは、シェーダーが毎フレーム計算するので CPU 更新は不要
            if (!entry.Data || entry.Data->Mask == 0 || !entry.Data->Graph.Settings.LiveUpdate || time < entry.NextTime ||
                MaterialManager::IsGraphProgramActive(handle))
            {
                continue;
            }
            const float rate = std::clamp(entry.Data->Graph.Settings.LiveRate, 1.0f, 120.0f);
            entry.NextTime = time + 1.0 / rate;
            entry.Job = std::async(std::launch::async, [data = entry.Data, textures = entry.Textures, time] {
                BackgroundTasks::Scope task("ライブプレビューの評価");
                return Evaluate(data, textures, static_cast<float>(time));
            });
        }
    }

    void Shutdown()
    {
        for (auto& [handle, entry] : g_Entries)
        {
            if (entry.Job.valid())
            {
                entry.Job.wait();
            }
        }
        g_Entries.clear();
        g_Checked.clear();
    }

    uint32_t ActiveSlots(AssetHandle material)
    {
        const auto found = g_Entries.find(material);
        return found != g_Entries.end() && found->second.Data && found->second.Data->Graph.Settings.LiveUpdate &&
                       !MaterialManager::IsGraphProgramActive(material)
                   ? found->second.Data->Mask
                   : 0;
    }

    double LastSeconds(AssetHandle material)
    {
        const auto found = g_Entries.find(material);
        return found != g_Entries.end() ? found->second.LastSeconds : 0.0;
    }
} // namespace MaterialGraphLive


