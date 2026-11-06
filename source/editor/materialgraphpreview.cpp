#include "pch.h"
#include "materialgraphpreview.h"
#include "materialgraph.h"
#include "materialgraphmath.h"
#include "materialgraphevaluator.h"
#include "materialgraphtextures.h"
#include "materialgraphcompiler.h"
#include "materialgraphshader.h"
#include "materialmanager.h"
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MaterialGraphPreview
{
    namespace
    {
        using Kind = MaterialNodeKind;
        using Pin = MaterialOutputPin;
        namespace GraphMath = MaterialGraphMath;
        using MaterialGraphEval::Context;
        using MaterialGraphEval::Evaluator;
        using MaterialGraphEval::Topology;
        using MaterialGraphEval::Value;
        constexpr int kNodeResolution = 64;   // ノードプレビューの評価解像度
        constexpr int kOutputResolution = 96; // Material Output の球
        constexpr double kAnimatedInterval = 1.0 / 15.0;
        constexpr float kPi = 3.14159265f;
        constexpr float kDefaultClip = 0.3333f; // UE の Opacity Mask Clip Value の既定値

        size_t PinIndex(Pin pin)
        {
            return static_cast<size_t>(pin);
        }

        // プレビュー用のテクスチャサンプラー（256px の縮小キャッシュ）
        MaterialGraphEval::TextureSampler PreviewSampler()
        {
            return [](AssetHandle texture, float u, float v, bool raw, float out[4]) {
                return MaterialGraphTextures::Sample(texture, u, v, raw, out);
            };
        }

        // ---------------------------------------------------------------- GPU 画像（ImGui 1.92 の管理テクスチャ）
        std::vector<ImTextureData*> g_Graveyard;

        bool BackendHasTextures()
        {
            return (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasTextures) != 0;
        }

        ImTextureData* CreateTexture(int size)
        {
            auto* texture = IM_NEW(ImTextureData)();
            texture->Create(ImTextureFormat_RGBA32, size, size);
            texture->SetStatus(ImTextureStatus_WantCreate);
            ImGui::RegisterUserTexture(texture);
            return texture;
        }

        void Upload(ImTextureData* texture, const std::vector<ImU32>& pixels)
        {
            if (!texture || pixels.size() != static_cast<size_t>(texture->Width) * texture->Height)
            {
                return;
            }
            memcpy(texture->GetPixels(), pixels.data(), pixels.size() * sizeof(ImU32));
            if (texture->Status == ImTextureStatus_OK)
            {
                ImTextureRect rect;
                rect.x = 0;
                rect.y = 0;
                rect.w = static_cast<unsigned short>(texture->Width);
                rect.h = static_cast<unsigned short>(texture->Height);
                texture->UpdateRect = rect;
                texture->Updates.resize(0);
                texture->Updates.push_back(rect);
                texture->SetStatus(ImTextureStatus_WantUpdates);
            }
            // WantCreate / WantUpdates のままなら、バックエンドが次の描画で最新のピクセルを送る
        }

        void ReleaseTexture(ImTextureData* texture)
        {
            if (!texture || !ImGui::GetCurrentContext())
            {
                return;
            }
            if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_Destroyed)
            {
                // まだ GPU に作られていない（または破棄済み）ならすぐに消せる
                ImGui::UnregisterUserTexture(texture);
                IM_DELETE(texture);
                return;
            }
            // SetStatus(Destroyed) otherwise schedules creation again while CPU pixels exist.
            texture->WantDestroyNextFrame = true;
            texture->SetStatus(ImTextureStatus_WantDestroy);
            g_Graveyard.push_back(texture);
        }

        void CollectGraveyard()
        {
            if (!ImGui::GetCurrentContext())
            {
                return;
            }
            for (auto it = g_Graveyard.begin(); it != g_Graveyard.end();)
            {
                ImTextureData* texture = *it;
                if (texture->Status == ImTextureStatus_Destroyed)
                {
                    ImGui::UnregisterUserTexture(texture);
                    IM_DELETE(texture);
                    it = g_Graveyard.erase(it);
                    continue;
                }
                ++texture->UnusedFrames; // フレーム内で使っていない → バックエンドが安全に破棄できる
                ++it;
            }
        }

        struct NodeImage
        {
            std::vector<ImU32> Pixels;
            int Resolution = kNodeResolution;
            bool Sphere = false;
            bool Dirty = true;
            uint64_t Key = 0; // 評価したときのノードのキー（NodeKeys）。同じなら描き直さない
            ImTextureData* Texture = nullptr;
        };

        // GPU で評価するノード（命令列は MaterialGraphVm::Compile。エンジンの描画と同じ）
        struct GpuNode
        {
            MaterialGraphGpu::Texels Program;
            uint64_t Hash = 0;
            MaterialGraphGpu::Mode PreviewMode = MaterialGraphGpu::Mode::NodePlane;
            uint32_t Flags = 0;
            uint32_t OutputPins = 0;
            bool Animated = false;
            int Resolution = 64;
            uint64_t Key = 0; // コンパイルしたときのノードのキー（同じなら命令列を使い回す）
        };

        struct GraphCache
        {
            std::unordered_map<uint64_t, GpuNode> Gpu; // GPU で評価できたノード（無いものは CPU）
            std::unordered_map<uint64_t, uint64_t> GpuFailed; // GPU にできなかったノード → そのときのキー
            std::vector<uint64_t> Keys; // ノードごとのキー（自分 + 上流の内容。Topo と同じ添字）
            uint64_t Signature = ~0ull;
            int SignatureFrame = -1;
            double LastEvaluation = -1.0;
            Topology Topo;
            std::unordered_map<uint64_t, NodeImage> Images;
            std::unordered_set<uint64_t> Collapsed;

            ~GraphCache()
            {
                for (auto& [id, image] : Images)
                {
                    ReleaseTexture(image.Texture);
                }
            }
        };

        std::unordered_map<const MaterialGraph*, GraphCache> g_Caches;

        float Saturate(float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        // ---------------------------------------------------------------- ユーティリティ
        ImU32 PackLinear(float r, float g, float b, float a = 1.0f)
        {
            auto encode = [](float v) {
                return static_cast<int>(MaterialGraphEval::LinearToSrgb(std::isfinite(v) ? v : 0.0f) * 255.0f + 0.5f);
            };
            return IM_COL32(encode(r), encode(g), encode(b), static_cast<int>(Saturate(a) * 255.0f + 0.5f));
        }

        ImU32 PackValue(const Value& value, bool color)
        {
            // スカラーはグレー、Vector2 は RG、Vector3/4 は RGB（0〜1）。テクスチャ由来の色は sRGB で表示
            auto encode = [color](float v) {
                v = std::isfinite(v) ? v : 0.0f;
                return static_cast<int>((color ? MaterialGraphEval::LinearToSrgb(v) : Saturate(v)) * 255.0f + 0.5f);
            };
            if (value.N == 1)
            {
                const int c = encode(value.V[0]);
                return IM_COL32(c, c, c, 255);
            }
            if (value.N == 2)
            {
                return IM_COL32(encode(value.V[0]), encode(value.V[1]), 0, 255);
            }
            return IM_COL32(encode(value.V[0]), encode(value.V[1]), encode(value.V[2]), 255);
        }

        uint64_t Mix(uint64_t hash, uint64_t value)
        {
            hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
            return hash;
        }

        uint64_t FloatBits(float value)
        {
            uint32_t bits = 0;
            memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        float Dot3(const float* a, const float* b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        // 球の画素から評価コンテキストを作る（カメラは -Z 側から +Z を見る）
        bool SphereContext(int x, int y, int resolution, float yaw, float pitch, float time, Context& context)
        {
            const float px = (x + 0.5f) / resolution * 2.0f - 1.0f;
            const float py = 1.0f - (y + 0.5f) / resolution * 2.0f;
            const float r2 = px * px + py * py;
            if (r2 > 1.0f)
            {
                return false;
            }
            const float n[3] = {px, py, -sqrtf(1.0f - r2)};
            for (int i = 0; i < 3; ++i)
            {
                context.Normal[i] = n[i];
                context.Position[i] = n[i];
            }
            context.Camera[0] = 0.0f;
            context.Camera[1] = 0.0f;
            context.Camera[2] = -1.0f;
            context.Time = time;
            context.PixelSize = 1.0f / (static_cast<float>(resolution) * 2.0f);
            // オブジェクト空間へ（pitch → yaw の逆回転）して球面 UV を求める
            const float cp = cosf(-pitch), sp = sinf(-pitch);
            const float oy = n[1] * cp - n[2] * sp;
            const float oz1 = n[1] * sp + n[2] * cp;
            const float cy = cosf(-yaw), sy = sinf(-yaw);
            const float ox = n[0] * cy + oz1 * sy;
            const float oz = -n[0] * sy + oz1 * cy;
            context.UV[0] = atan2f(ox, -oz) / (2.0f * kPi) + 0.5f;
            context.UV[1] = acosf(std::clamp(oy, -1.0f, 1.0f)) / kPi;
            // 接線（+U 方向）と従法線（+V 方向 = 下向き）。エンジンの接空間と同じ向き
            float t[3] = {-n[2], 0.0f, n[0]};
            const float tl = sqrtf(t[0] * t[0] + t[2] * t[2]);
            if (tl > 1e-4f)
            {
                t[0] /= tl;
                t[2] /= tl;
            }
            else
            {
                t[0] = 1.0f;
                t[2] = 0.0f;
            }
            const float b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
            for (int i = 0; i < 3; ++i)
            {
                context.Tangent[i] = t[i];
                context.Bitangent[i] = b[i];
            }
            return true;
        }

        void PlaneContext(int x, int y, int resolution, float time, Context& context)
        {
            const float u = (x + 0.5f) / resolution;
            const float v = (y + 0.5f) / resolution;
            context.UV[0] = u;
            context.UV[1] = v;
            context.Position[0] = u * 2.0f - 1.0f;
            context.Position[1] = 1.0f - v * 2.0f;
            context.Position[2] = 0.0f;
            context.Time = time;
            context.PixelSize = 1.0f / static_cast<float>(resolution);
        }

        const MaterialGraphNode* ResolveSource(const MaterialGraph& graph, const MaterialGraphNode& node, size_t input)
        {
            return MaterialGraphEval::ResolveSource(graph, node, input);
        }

        // 色として表示する（sRGB 変換する）ノードか：テクスチャ（Color サンプラー）/ カラー由来
        bool IsColorNode(const MaterialGraph& graph, const MaterialGraphNode& node)
        {
            const auto colorSource = [](const MaterialGraphNode* source) {
                // 分割したテクスチャは成分（スカラー）として使われるので色扱いしない
                if (!source || !source->SplitPins.empty())
                {
                    return false;
                }
                const bool texture = source->Kind == Kind::TextureSample || source->Kind == Kind::TriplanarTexture ||
                                     (source->Kind == Kind::ConvertToTexture && source->Texture != 0);
                return (texture && static_cast<int>(source->Value[0]) == static_cast<int>(MaterialSamplerType::Color)) ||
                       source->Kind == Kind::ColorParameter || source->Kind == Kind::BlackBody ||
                       source->Kind == Kind::HueShift || source->Kind == Kind::Blend ||
                       source->Kind == Kind::SolidTexture || source->Kind == Kind::ThreeColorRamp ||
                       (source->Kind == Kind::ConvertToSolid && source->Texture != 0 && source->Value[3] <= 0.5f &&
                        static_cast<int>(source->Value[1]) == static_cast<int>(MaterialSamplerType::Color));
            };
            if (colorSource(&node))
            {
                return true;
            }
            if (node.Kind == Kind::Multiply || node.Kind == Kind::Lerp || node.Kind == Kind::Add ||
                node.Kind == Kind::Desaturation || node.Kind == Kind::Reroute || node.Kind == Kind::StaticSwitch ||
                node.Kind == Kind::ComponentMask)
            {
                for (size_t i = 0; i < (std::min)(node.InputPins.size(), size_t{2}); ++i)
                {
                    if (colorSource(ResolveSource(graph, node, i)))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // Normal 入力の値を接空間の法線（-1〜1）に直す。Color サンプラーの法線マップ（旧グラフ）は格納値に戻す
        bool IsLegacyColorNormal(const MaterialGraph& graph, const MaterialGraphNode& output)
        {
            const auto* source = ResolveSource(graph, output, PinIndex(Pin::Normal));
            return source && source->SplitPins.empty() && source->Kind == Kind::TextureSample &&
                   static_cast<int>(source->Value[0]) == static_cast<int>(MaterialSamplerType::Color);
        }

        void DecodeNormalInput(bool legacyColorNormal, Value tangent, float out[3])
        {
            tangent = MaterialGraphEval::Promote(tangent, 3);
            if (legacyColorNormal)
            {
                for (int i = 0; i < 3; ++i)
                {
                    tangent.V[i] = MaterialGraphEval::LinearToSrgb(tangent.V[i]) * 2.0f - 1.0f;
                }
            }
            out[0] = tangent.V[0];
            out[1] = tangent.V[1];
            out[2] = tangent.V[2];
        }

        void DecodeNormalInput(const MaterialGraph& graph, const MaterialGraphNode& output, Value tangent, float out[3])
        {
            DecodeNormalInput(IsLegacyColorNormal(graph, output), tangent, out);
        }

        // Material Output のピンの状態（画素ごとに接続を探さないよう、評価の前に 1 回だけ調べる）
        struct OutputInfo
        {
            bool Height = false;
            bool Normal = false;
            bool OpacityMask = false;
            bool Transmission = false;
            bool LegacyColorNormal = false;
        };

        OutputInfo BuildOutputInfo(const MaterialGraph& graph, const MaterialGraphNode& output)
        {
            const auto used = [&](Pin pin) {
                return ResolveSource(graph, output, PinIndex(pin)) != nullptr ||
                       graph.HasOutputDirectValue(output, PinIndex(pin));
            };
            OutputInfo info;
            info.Height = ResolveSource(graph, output, PinIndex(Pin::Height)) != nullptr;
            info.Normal = used(Pin::Normal);
            info.OpacityMask = used(Pin::OpacityMask);
            info.Transmission = used(Pin::Transmission);
            info.LegacyColorNormal = IsLegacyColorNormal(graph, output);
            return info;
        }

        float ClipValue(const MaterialGraph& graph)
        {
            return graph.Settings.AlphaCutoff >= 0.0f ? graph.Settings.AlphaCutoff : kDefaultClip;
        }

        // ---------------------------------------------------------------- Material Output の簡易 PBR（POM / 高さ法線込み）
        ImU32 ShadeOutput(Evaluator& evaluator, const MaterialGraph& graph, const MaterialGraphNode& output,
                          const OutputInfo& info, const Context& base, int x, int y, int resolution)
        {
            Context c = base;
            const bool heightLinked = info.Height;
            const bool normalLinked = info.Normal;
            float depth = 0.0f;
            if (heightLinked)
            {
                evaluator.Reset(c);
                depth = std::clamp(evaluator.Input(output, PinIndex(Pin::ParallaxDepth), 0).V[0], 0.0f, 0.2f);
            }
            // ---- POM（エンジンの ApplyAssetParallaxEx と同じ向き：uv - (V・T, V・B) / (V・N) * 深さ * 層）
            if (depth > 0.0f)
            {
                const float vt = Dot3(c.Camera, c.Tangent);
                const float vb = Dot3(c.Camera, c.Bitangent);
                const float vn = Dot3(c.Camera, c.Normal);
                if (vn > 0.02f)
                {
                    const float fade = std::clamp((vn - 0.02f) / 0.13f, 0.0f, 1.0f);
                    const float scale = depth * fade * fade * (3.0f - 2.0f * fade) / (std::max)(vn, 0.1f);
                    const float offset[2] = {vt * scale, vb * scale};
                    const int steps = 24;
                    float previous = 0.0f;
                    float hitLayer = 1.0f;
                    for (int i = 0; i <= steps; ++i)
                    {
                        const float layer = static_cast<float>(i) / steps;
                        const float h = Saturate(evaluator
                                                     .InputAt(output, PinIndex(Pin::Height), base.UV[0] - offset[0] * layer,
                                                              base.UV[1] - offset[1] * layer)
                                                     .V[0]);
                        const float difference = (1.0f - h) - layer;
                        if (difference <= 0.0f)
                        {
                            const float t = i > 0 ? previous / (std::max)(previous - difference, 1e-6f) : 0.0f;
                            hitLayer = i > 0 ? (static_cast<float>(i - 1) + t) / steps : 0.0f;
                            break;
                        }
                        previous = difference;
                    }
                    c.UV[0] = base.UV[0] - offset[0] * hitLayer;
                    c.UV[1] = base.UV[1] - offset[1] * hitLayer;
                }
            }

            // ---- 法線：Normal 入力（Normal Intensity で xy を拡大）、無ければ高さの勾配から作る（エンジンと同じ）
            float tangentNormal[3] = {0.0f, 0.0f, 1.0f};
            bool hasTangentNormal = false;
            if (!normalLinked && depth > 0.0f)
            {
                const float e = 1.0f / 256.0f;
                const float h0 = evaluator.InputAt(output, PinIndex(Pin::Height), c.UV[0], c.UV[1]).V[0];
                const float hu = evaluator.InputAt(output, PinIndex(Pin::Height), c.UV[0] + e, c.UV[1]).V[0];
                const float hv = evaluator.InputAt(output, PinIndex(Pin::Height), c.UV[0], c.UV[1] + e).V[0];
                tangentNormal[0] = -(hu - h0) / e * depth;
                tangentNormal[1] = -(hv - h0) / e * depth;
                hasTangentNormal = true;
            }
            evaluator.Reset(c);
            auto in = [&](Pin pin) {
                Value v = evaluator.Input(output, PinIndex(pin), 0);
                if (v.N == 1)
                {
                    v.V[1] = v.V[2] = v.V[3] = v.V[0];
                }
                return v;
            };
            if (normalLinked)
            {
                DecodeNormalInput(info.LegacyColorNormal, evaluator.Input(output, PinIndex(Pin::Normal), 0),
                                  tangentNormal);
                const float intensity = (std::max)(in(Pin::NormalIntensity).V[0], 0.0f);
                tangentNormal[0] *= intensity;
                tangentNormal[1] *= intensity;
                hasTangentNormal = true;
            }
            const Value baseValue = in(Pin::BaseColor);
            const float baseColor[3] = {Saturate(baseValue.V[0]), Saturate(baseValue.V[1]), Saturate(baseValue.V[2])};
            const float metallic = Saturate(in(Pin::Metallic).V[0]);
            const float roughness = std::clamp(in(Pin::Roughness).V[0], 0.04f, 1.0f);
            const Value emissive = in(Pin::Emissive);
            float opacity = Saturate(in(Pin::Opacity).V[0]);
            const float mask = in(Pin::OpacityMask).V[0];
            const float ao = Saturate(in(Pin::AO).V[0]);
            const float transmission = Saturate(in(Pin::Transmission).V[0]);
            if (info.OpacityMask && mask < ClipValue(graph))
            {
                return 0; // Opacity Mask
            }
            if (info.Transmission)
            {
                opacity *= 1.0f - 0.7f * transmission; // 透過の目安（背景が透けて見える）
            }

            float n[3] = {c.Normal[0], c.Normal[1], c.Normal[2]};
            if (hasTangentNormal)
            {
                float p[3];
                for (int i = 0; i < 3; ++i)
                {
                    p[i] = c.Tangent[i] * tangentNormal[0] + c.Bitangent[i] * tangentNormal[1] +
                           c.Normal[i] * tangentNormal[2];
                }
                const float length = sqrtf(Dot3(p, p));
                if (length > 1e-4f)
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        n[i] = p[i] / length;
                    }
                }
            }

            const float v[3] = {c.Camera[0], c.Camera[1], c.Camera[2]}; // 表面 → カメラ
            float l[3] = {-0.45f, 0.65f, -0.6f};
            const float ll = sqrtf(Dot3(l, l));
            for (float& k : l)
            {
                k /= ll;
            }
            float h[3] = {l[0] + v[0], l[1] + v[1], l[2] + v[2]};
            const float hl = (std::max)(sqrtf(Dot3(h, h)), 1e-5f);
            for (float& k : h)
            {
                k /= hl;
            }
            const float nDotL = Saturate(Dot3(n, l));
            const float nDotV = Saturate(Dot3(n, v)) + 1e-4f;
            const float nDotH = Saturate(Dot3(n, h));
            const float vDotH = Saturate(Dot3(v, h));
            const float a = roughness * roughness;
            const float a2 = a * a;
            const float d = a2 / (kPi * powf(nDotH * nDotH * (a2 - 1.0f) + 1.0f, 2.0f) + 1e-6f);
            const float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
            const float g = (nDotL / (nDotL * (1.0f - k) + k)) * (nDotV / (nDotV * (1.0f - k) + k));
            const float fresnelWeight = powf(1.0f - vDotH, 5.0f);
            const float ambientWeight = powf(1.0f - nDotV, 5.0f);
            const float sky = n[1] * 0.5f + 0.5f;
            const float light = 3.0f;

            float color[3];
            for (int i = 0; i < 3; ++i)
            {
                const float f0 = 0.04f + (baseColor[i] - 0.04f) * metallic;
                const float f = f0 + (1.0f - f0) * fresnelWeight;
                const float specular = d * f * g / (4.0f * nDotL * nDotV + 1e-4f);
                const float diffuse = baseColor[i] * (1.0f - metallic) / kPi;
                const float ambientColor = (i == 2 ? 0.30f : 0.22f) * sky + 0.06f * (1.0f - sky);
                const float ambient = ambientColor * ao *
                                      (baseColor[i] * (1.0f - metallic) +
                                       (f0 + (1.0f - f0) * ambientWeight) * (1.0f - roughness * 0.7f));
                color[i] = (diffuse + specular) * nDotL * light + ambient + (std::max)(emissive.V[i], 0.0f);
                color[i] = color[i] / (1.0f + color[i]); // Reinhard
            }
            if (opacity < 0.999f)
            {
                const int cell = (std::max)(resolution / 8, 1);
                const float background = (((x / cell) + (y / cell)) & 1) ? 0.18f : 0.06f;
                for (float& channel : color)
                {
                    channel = channel * opacity + background * (1.0f - opacity);
                }
            }
            return PackLinear(color[0], color[1], color[2]);
        }

        // ---------------------------------------------------------------- 再評価
        // timeOnly: アニメーション更新では Time に依存するノードと Output だけを評価し直す（最適化）
        void EvaluateImages(const MaterialGraph& graph, GraphCache& cache, float time, bool timeOnly)
        {
            Evaluator evaluator(graph, cache.Topo, PreviewSampler());
            const auto& topology = cache.Topo;
            // 1) 値ノード：平面用と球用の 2 パス。各画素で全ノードをメモ化して評価（O(ノード数 × 画素数)）
            for (int pass = 0; pass < 2; ++pass)
            {
                const bool sphere = pass == 1;
                std::vector<std::pair<size_t, NodeImage*>> targets;
                for (size_t i = 0; i < graph.Nodes.size(); ++i)
                {
                    const auto& node = graph.Nodes[i];
                    if (!HasPreview(node) || node.Kind == Kind::Output || (topology.Spatial[i] != 0) != sphere ||
                        cache.Collapsed.contains(node.Id) || (timeOnly && !topology.TimeDependent[i]) ||
                        cache.Gpu.contains(node.Id))
                    {
                        continue;
                    }
                    const uint64_t key = i < cache.Keys.size() ? cache.Keys[i] : 0;
                    if (!timeOnly && key != 0 && !topology.TimeDependent[i])
                    {
                        // 上流（自分 + 入力側）が変わっていないノードは描き直さない（値の変更は下流だけ再評価）
                        const auto existing = cache.Images.find(node.Id);
                        if (existing != cache.Images.end() && existing->second.Key == key &&
                            existing->second.Sphere == sphere && !existing->second.Pixels.empty())
                        {
                            continue;
                        }
                    }
                    auto& image = cache.Images[node.Id];
                    image.Key = key;
                    image.Sphere = sphere;
                    image.Resolution = kNodeResolution;
                    image.Pixels.assign(static_cast<size_t>(kNodeResolution) * kNodeResolution, 0);
                    image.Dirty = true;
                    targets.emplace_back(i, &image);
                }
                if (targets.empty())
                {
                    continue;
                }
                std::vector<uint8_t> colorNode(targets.size());
                std::vector<uint8_t> normalNode(targets.size()); // 法線系ノード（-1〜1 を 0〜1 にして表示）
                for (size_t t = 0; t < targets.size(); ++t)
                {
                    const auto& target = graph.Nodes[targets[t].first];
                    colorNode[t] = IsColorNode(graph, target) ? 1 : 0;
                    const Kind kind = target.Kind;
                    normalNode[t] =
                        (kind == Kind::NormalFromHeight || kind == Kind::BlendAngleCorrectedNormals ||
                         kind == Kind::FlattenNormal || kind == Kind::ConvertToNormal ||
                         (kind == Kind::ConvertToNormalHeight && target.Value[0] > 0.5f) ||
                         ((kind == Kind::TextureSample || (kind == Kind::ConvertToTexture && target.Texture != 0)) &&
                          static_cast<int>(target.Value[0]) == static_cast<int>(MaterialSamplerType::Normal)))
                            ? 1
                            : 0;
                }
                for (int y = 0; y < kNodeResolution; ++y)
                {
                    for (int x = 0; x < kNodeResolution; ++x)
                    {
                        Context context;
                        if (sphere)
                        {
                            if (!SphereContext(x, y, kNodeResolution, 0.6f, 0.25f, time, context))
                            {
                                continue;
                            }
                        }
                        else
                        {
                            PlaneContext(x, y, kNodeResolution, time, context);
                        }
                        evaluator.Reset(context);
                        for (size_t t = 0; t < targets.size(); ++t)
                        {
                            Value value = evaluator.Node(targets[t].first);
                            // 法線系ノードは -1〜1 を 0〜1 に直して表示する（UE と同じ見え方）
                            if (normalNode[t] != 0)
                            {
                                for (int c = 0; c < 3; ++c)
                                {
                                    value.V[c] = value.V[c] * 0.5f + 0.5f;
                                }
                            }
                            targets[t].second->Pixels[static_cast<size_t>(y) * kNodeResolution + x] =
                                PackValue(value, colorNode[t] != 0);
                        }
                    }
                }
            }
            // 2) Material Output：球に簡易 PBR
            for (const auto& node : graph.Nodes)
            {
                if (node.Kind != Kind::Output || cache.Collapsed.contains(node.Id) ||
                    (timeOnly && !topology.TimeDependent[topology.Index.at(node.Id)]) || cache.Gpu.contains(node.Id))
                {
                    continue;
                }
                const size_t outputIndex = topology.Index.at(node.Id);
                const uint64_t key = outputIndex < cache.Keys.size() ? cache.Keys[outputIndex] : 0;
                if (!timeOnly && key != 0 && !topology.TimeDependent[outputIndex])
                {
                    const auto existing = cache.Images.find(node.Id);
                    if (existing != cache.Images.end() && existing->second.Key == key &&
                        !existing->second.Pixels.empty())
                    {
                        continue; // 出力に繋がる式が変わっていない
                    }
                }
                auto& image = cache.Images[node.Id];
                image.Sphere = true;
                image.Resolution = kOutputResolution;
                image.Pixels.assign(static_cast<size_t>(kOutputResolution) * kOutputResolution, 0);
                image.Dirty = true;
                image.Key = key;
                const OutputInfo info = BuildOutputInfo(graph, node);
                for (int y = 0; y < kOutputResolution; ++y)
                {
                    for (int x = 0; x < kOutputResolution; ++x)
                    {
                        Context context;
                        if (SphereContext(x, y, kOutputResolution, 0.6f, 0.25f, time, context))
                        {
                            image.Pixels[static_cast<size_t>(y) * kOutputResolution + x] =
                                ShadeOutput(evaluator, graph, node, info, context, x, y, kOutputResolution);
                        }
                    }
                }
            }
        }

        bool IsNormalNode(const MaterialGraphNode& node)
        {
            const Kind kind = node.Kind;
            return kind == Kind::NormalFromHeight || kind == Kind::BlendAngleCorrectedNormals || kind == Kind::FlattenNormal ||
                   kind == Kind::ConvertToNormal || (kind == Kind::ConvertToNormalHeight && node.Value[0] > 0.5f) ||
                   ((kind == Kind::TextureSample || kind == Kind::TriplanarTexture ||
                     (kind == Kind::ConvertToTexture && node.Texture != 0)) &&
                    static_cast<int>(node.Value[0]) == static_cast<int>(MaterialSamplerType::Normal));
        }

        // プレビューを GPU で評価できるノードの命令列を作る（対応外のノードに依存するものは CPU のまま）
        // ノードごとのキー：自分の内容 + 入力側（上流）のキー + グラフ設定 + テクスチャの世代。
        // 値を 1 つ変えても、キーが変わるのはそのノードと下流だけになる（他のノードは命令列 / 画像を使い回す）
        void BuildNodeKeys(const MaterialGraph& graph, GraphCache& cache)
        {
            const auto& topology = cache.Topo;
            const size_t count = graph.Nodes.size();
            uint64_t global = Mix(0xC0FFEEull, MaterialGraphTextures::Generation());
            const auto& settings = graph.Settings;
            for (const int v : {settings.Mode, settings.Surface, settings.ShadingModel, settings.SolidSpace,
                                settings.RuntimeEvaluation ? 1 : 0})
            {
                global = Mix(global, static_cast<uint64_t>(static_cast<int64_t>(v)));
            }
            global = Mix(global, FloatBits(settings.AlphaCutoff));
            cache.Keys.assign(count, 0);
            std::vector<uint8_t> state(count, 0);
            std::function<uint64_t(size_t, int)> visit = [&](size_t index, int depth) -> uint64_t {
                if (state[index] == 1)
                {
                    return cache.Keys[index];
                }
                if (state[index] == 2 || depth > 256)
                {
                    return 0x5EEDull; // 循環（通常は Connect で防がれる）
                }
                state[index] = 2;
                const auto& node = graph.Nodes[index];
                uint64_t hash = Mix(global, node.Id);
                hash = Mix(hash, static_cast<uint64_t>(node.Kind));
                hash = Mix(hash, node.Texture);
                for (const float v : node.Value)
                {
                    hash = Mix(hash, FloatBits(v));
                }
                hash = Mix(hash, node.SplitPins.size());
                for (size_t p = 0; p < node.PinValues.size(); ++p)
                {
                    if (std::isfinite(node.PinValues[p]))
                    {
                        hash = Mix(Mix(hash, p + 1), FloatBits(node.PinValues[p]));
                    }
                }
                for (size_t p = 0; p < node.PinVectors.size(); ++p)
                {
                    if (MaterialGraph::HasPinVector(node, p))
                    {
                        hash = Mix(hash, (p + 1) << 8);
                        for (const float v : node.PinVectors[p])
                        {
                            hash = Mix(hash, FloatBits(v));
                        }
                    }
                }
                const bool table = topology.InputOffset.size() == count + 1;
                for (size_t p = 0; p < node.InputPins.size(); ++p)
                {
                    if (!table)
                    {
                        hash = Mix(hash, 0xBADull); // 表が無い（通常は無い）：常に作り直す
                        hash = Mix(hash, static_cast<uint64_t>(ImGui::GetFrameCount()));
                        continue;
                    }
                    const auto& source = topology.Inputs[topology.InputOffset[index] + p];
                    hash = Mix(hash, source.Linked ? 1u : 0u);
                    if (source.Node >= 0)
                    {
                        hash = Mix(hash, visit(static_cast<size_t>(source.Node), depth + 1));
                        hash = Mix(hash, static_cast<uint64_t>(source.Output));
                    }
                }
                if (hash == 0)
                {
                    hash = 1;
                }
                cache.Keys[index] = hash;
                state[index] = 1;
                return hash;
            };
            for (size_t i = 0; i < count; ++i)
            {
                visit(i, 0);
            }
        }

        // プレビューを GPU で評価できるノードの命令列を作る（対応外のノードに依存するものは CPU のまま）。
        // キーが変わったノードだけコンパイルし直す（以前は変更のたびに全ノードをコンパイルしていた：O(ノード数²)）
        void CompileGpuPreviews(const MaterialGraph& graph, GraphCache& cache)
        {
            if (!MaterialGraphGpu::IsAvailable())
            {
                cache.Gpu.clear();
                return;
            }
            std::unordered_map<uint64_t, GpuNode> previous;
            previous.swap(cache.Gpu);
            std::unordered_map<uint64_t, uint64_t> failed;
            for (size_t i = 0; i < graph.Nodes.size(); ++i)
            {
                const auto& node = graph.Nodes[i];
                if (!HasPreview(node) || cache.Collapsed.contains(node.Id))
                {
                    continue;
                }
                const uint64_t key = i < cache.Keys.size() ? cache.Keys[i] : 0;
                if (key != 0)
                {
                    if (const auto reuse = previous.find(node.Id); reuse != previous.end() && reuse->second.Key == key)
                    {
                        cache.Gpu.emplace(node.Id, std::move(reuse->second));
                        continue;
                    }
                    if (const auto miss = cache.GpuFailed.find(node.Id); miss != cache.GpuFailed.end() && miss->second == key)
                    {
                        failed.emplace(node.Id, key); // 前回 GPU にできなかった：同じ内容なら試さない
                        continue;
                    }
                }
                MaterialGraphVm::Options options;
                const bool output = node.Kind == Kind::Output;
                options.TargetNode = output ? 0 : node.Id;
                options.IncludeDefaults = output;
                const auto program =
                    MaterialGraphVm::Compile(graph, &MaterialManager::ResolveGraphTexture, options);
                if (!program.Success)
                {
                    if (key != 0)
                    {
                        failed.emplace(node.Id, key);
                    }
                    continue;
                }
                GpuNode gpu;
                gpu.Key = key;
                gpu.Program = program.Texels;
                gpu.Hash = program.Hash;
                gpu.Animated = program.UsesTime;
                if (output)
                {
                    gpu.PreviewMode = MaterialGraphGpu::Mode::OutputSphere;
                    gpu.Resolution = kOutputResolution;
                    // 接続、またはピン横の直値が入っているピン
                    const auto used = [&](Pin pin) {
                        return ResolveSource(graph, node, PinIndex(pin)) != nullptr ||
                               graph.HasOutputDirectValue(node, PinIndex(pin));
                    };
                    gpu.OutputPins = (used(Pin::Normal) ? 1u : 0u) | (used(Pin::OpacityMask) ? 2u : 0u) |
                                     (used(Pin::Transmission) ? 4u : 0u);
                }
                else
                {
                    gpu.PreviewMode = cache.Topo.Spatial[i] != 0 ? MaterialGraphGpu::Mode::NodeSphere
                                                                 : MaterialGraphGpu::Mode::NodePlane;
                    gpu.Resolution = kNodeResolution;
                    gpu.Flags = (IsColorNode(graph, node) ? 1u : 0u) | (IsNormalNode(node) ? 2u : 0u) |
                                (static_cast<uint32_t>(program.TargetDims) << 4u);
                }
                cache.Gpu[node.Id] = std::move(gpu);
            }
            cache.GpuFailed.swap(failed);
        }

        uint64_t ComputeSignature(const MaterialGraph& graph, const GraphCache& cache)
        {
            uint64_t hash = Mix(graph.Revision, graph.ParameterRevision);
            hash = Mix(hash, ContentSignature(graph));
            hash = Mix(hash, MaterialGraphTextures::Generation()); // テクスチャの読み込み完了で再評価
            hash = Mix(hash, cache.Collapsed.size());
            return hash;
        }

        GraphCache& Prepare(const MaterialGraph& graph)
        {
            GraphCache& cache = g_Caches[&graph];
            const int frame = ImGui::GetFrameCount();
            if (cache.SignatureFrame == frame)
            {
                return cache;
            }
            cache.SignatureFrame = frame;
            CollectGraveyard();
            const uint64_t signature = ComputeSignature(graph, cache);
            const double now = ImGui::GetTime();
            const bool changed = signature != cache.Signature;
            // ドラッグ中など毎フレーム値が変わる場合は 30Hz に間引く（最後の変更は次の間隔で必ず反映される）
            if (changed && cache.Signature != ~0ull && now - cache.LastEvaluation < 1.0 / 30.0)
            {
                return cache;
            }
            if (changed)
            {
                MaterialGraphEval::BuildTopology(graph, cache.Topo);
                BuildNodeKeys(graph, cache);
                CompileGpuPreviews(graph, cache);
            }
            if (changed || (cache.Topo.Animated && now - cache.LastEvaluation >= kAnimatedInterval))
            {
                cache.Signature = signature;
                cache.LastEvaluation = now;
                // 削除済みノードの画像を捨てる
                for (auto it = cache.Images.begin(); it != cache.Images.end();)
                {
                    if (cache.Topo.Index.contains(it->first))
                    {
                        ++it;
                        continue;
                    }
                    ReleaseTexture(it->second.Texture);
                    it = cache.Images.erase(it);
                }
                EvaluateImages(graph, cache, static_cast<float>(now), !changed);
            }
            return cache;
        }

        void DrawChecker(ImDrawList* draw, ImVec2 min, ImVec2 max, float cell)
        {
            draw->AddRectFilled(min, max, IM_COL32(32, 32, 32, 255));
            const int columns = static_cast<int>(ceilf((max.x - min.x) / cell));
            const int rows = static_cast<int>(ceilf((max.y - min.y) / cell));
            for (int y = 0; y < rows; ++y)
            {
                for (int x = (y & 1); x < columns; x += 2)
                {
                    draw->AddRectFilled(ImVec2(min.x + x * cell, min.y + y * cell),
                                        ImVec2((std::min)(min.x + (x + 1) * cell, max.x),
                                               (std::min)(min.y + (y + 1) * cell, max.y)),
                                        IM_COL32(48, 48, 48, 255));
                }
            }
        }

        // 管理テクスチャ非対応バックエンド向け：同じ色の横並びをまとめた矩形描画（間引きあり）
        void DrawPixelsFallback(ImDrawList* draw, ImVec2 min, float size, const NodeImage& image)
        {
            const int step = image.Resolution > 48 ? 2 : 1;
            const int resolution = image.Resolution / step;
            const float cell = size / resolution;
            for (int y = 0; y < resolution; ++y)
            {
                int x = 0;
                while (x < resolution)
                {
                    const ImU32 color = image.Pixels[static_cast<size_t>(y * step) * image.Resolution + x * step];
                    int end = x + 1;
                    while (end < resolution &&
                           image.Pixels[static_cast<size_t>(y * step) * image.Resolution + end * step] == color)
                    {
                        ++end;
                    }
                    if ((color & IM_COL32_A_MASK) != 0)
                    {
                        draw->AddRectFilled(ImVec2(min.x + x * cell, min.y + y * cell),
                                            ImVec2(min.x + end * cell + 0.5f, min.y + (y + 1) * cell + 0.5f), color);
                    }
                    x = end;
                }
            }
        }
    } // namespace

    bool HasPreview(const MaterialGraphNode& node)
    {
        switch (node.Kind)
        {
            // プレビューしても意味の無いノード（UE でもプレビューを出さない / 画面全体が 1 色になる）
            case Kind::ToonShading:
            case Kind::LiveToonShading:
            case Kind::Group:
            case Kind::Reroute:
            case Kind::Time:              // 時刻（数値）
            case Kind::Constant:          // 定数（値はノード上に表示している）
            case Kind::Constant2Vector:
            case Kind::Constant3Vector:
            case Kind::Constant4Vector:
            case Kind::ScalarParameter:
            case Kind::VectorParameter:
            case Kind::ColorParameter:    // 色はノード上の色ボタンで見える
            case Kind::Pi:
            case Kind::BreakVector:       // 出力は X / Y / Z / W のピン（元のベクトルを表示しても意味が無い）
            // オブジェクト / カメラの情報：プレビューには対象のオブジェクトが無く、固定値になる
            case Kind::ObjectPosition:
            case Kind::ObjectScale:
            case Kind::ObjectOrientation:
            case Kind::ObjectRadius:
            case Kind::ObjectBounds:
            case Kind::PerInstanceRandom:
            case Kind::CameraPosition:
            case Kind::CameraDistance:
            case Kind::PixelDepth:
            case Kind::VertexColor:       // プレビューでは白
            case Kind::DistanceFade:      // プレビューでは距離一定
                return false;
            default:
                return true;
        }
    }

    bool IsNodePreviewCollapsed(const MaterialGraph& graph, uint64_t node)
    {
        const auto found = g_Caches.find(&graph);
        return found != g_Caches.end() && found->second.Collapsed.contains(node);
    }

    void ToggleNodePreview(const MaterialGraph& graph, uint64_t node)
    {
        auto& cache = g_Caches[&graph];
        if (!cache.Collapsed.erase(node))
        {
            cache.Collapsed.insert(node);
        }
        cache.Signature = ~0ull; // 再評価
    }

    void DrawNodePreview(const MaterialGraph& graph, const MaterialGraphNode& node, float width, float height)
    {
        if (!HasPreview(node))
        {
            return;
        }
        if (IsNodePreviewCollapsed(graph, node.Id))
        {
            return;
        }
        const float areaWidth = (std::max)(width, 16.0f);
        const float areaHeight = height > 0.0f ? (std::max)(height, 16.0f) : areaWidth;
        ImGui::Dummy(ImVec2(areaWidth, areaHeight));
        // レイアウトは維持し、画面外の画像は描画・アップロード・GPU 更新を要求しない。
        if (!ImGui::IsItemVisible())
        {
            return;
        }
        GraphCache& cache = Prepare(graph);
        const ImVec2 areaMin = ImGui::GetItemRectMin();
        // 縦横比 1:1 のまま、確保した領域の中央に置く
        const float size = (std::min)(areaWidth, areaHeight);
        const ImVec2 min(areaMin.x + (areaWidth - size) * 0.5f, areaMin.y + (areaHeight - size) * 0.5f);
        const ImVec2 max(min.x + size, min.y + size);
        ImDrawList* draw = ImGui::GetWindowDrawList();

        // ---- GPU で評価したプレビュー
        if (const auto gpu = cache.Gpu.find(node.Id); gpu != cache.Gpu.end())
        {
            MaterialGraphGpu::PreviewRequest request;
            request.Owner = &graph;
            request.Node = node.Id;
            request.Program = &gpu->second.Program;
            request.ProgramHash = gpu->second.Hash;
            request.PreviewMode = gpu->second.PreviewMode;
            request.Flags = gpu->second.Flags;
            request.OutputPins = gpu->second.OutputPins;
            request.Clip = ClipValue(graph);
            request.Resolution = gpu->second.Resolution;
            request.Animated = gpu->second.Animated;
            request.Time = static_cast<float>(ImGui::GetTime());
            MaterialGraphGpu::PreviewImage image;
            if (MaterialGraphGpu::RequestPreview(request, image))
            {
                if (gpu->second.PreviewMode != MaterialGraphGpu::Mode::NodePlane)
                {
                    DrawChecker(draw, min, max, size / 8.0f);
                }
                if (image.Ready)
                {
                    draw->AddImage(static_cast<ImTextureID>(image.TextureId), min, max, ImVec2(image.U0, image.V0),
                                   ImVec2(image.U1, image.V1));
                }
                else
                {
                    draw->AddRectFilled(min, max, IM_COL32(30, 30, 30, 255));
                }
                draw->AddRect(min, max, IM_COL32(0, 0, 0, 160));
                return;
            }
            // GPU が使えない（初期化失敗 / プレビュー表が一杯）→ 次の評価から CPU に戻す
            cache.GpuFailed[node.Id] = gpu->second.Key;
            cache.Gpu.erase(gpu);
            cache.Signature = ~0ull;
        }
        const auto found = cache.Images.find(node.Id);
        if (found == cache.Images.end() || found->second.Pixels.empty())
        {
            draw->AddRectFilled(min, max, IM_COL32(30, 30, 30, 255));
            return;
        }
        NodeImage& image = found->second;
        if (image.Sphere)
        {
            DrawChecker(draw, min, max, size / 8.0f);
        }
        if (BackendHasTextures())
        {
            if (!image.Texture || image.Texture->Width != image.Resolution)
            {
                ReleaseTexture(image.Texture);
                image.Texture = CreateTexture(image.Resolution);
                image.Dirty = true;
            }
            if (image.Dirty)
            {
                Upload(image.Texture, image.Pixels);
                image.Dirty = false;
            }
            image.Texture->UnusedFrames = 0;
            draw->AddImage(image.Texture->GetTexRef(), min, max);
        }
        else
        {
            DrawPixelsFallback(draw, min, size, image);
        }
        draw->AddRect(min, max, IM_COL32(0, 0, 0, 160));
        if (UsesNodeTexture(node.Kind) &&
            node.Texture != 0 &&
            !MaterialGraphTextures::IsReady(node.Texture))
        {
            draw->AddText(ImVec2(min.x + 4.0f, min.y + 4.0f), IM_COL32(220, 220, 220, 200), "読み込み中...");
        }
    }

    uint64_t ContentSignature(const MaterialGraph& graph)
    {
        uint64_t hash = Mix(0x51ED270B27u, graph.Nodes.size());
        for (const auto& node : graph.Nodes)
        {
            hash = Mix(hash, node.Id);
            hash = Mix(hash, static_cast<uint64_t>(node.Kind));
            hash = Mix(hash, node.Texture);
            for (float v : node.Value)
            {
                hash = Mix(hash, FloatBits(v));
            }
            hash = Mix(hash, node.SplitPins.size());
            for (const auto pin : node.SplitPins)
            {
                hash = Mix(hash, pin);
            }
            for (size_t i = 0; i < node.PinValues.size(); ++i)
            {
                if (std::isfinite(node.PinValues[i]))
                {
                    hash = Mix(hash, i + 1);
                    hash = Mix(hash, FloatBits(node.PinValues[i]));
                }
            }
            for (size_t i = 0; i < node.PinVectors.size(); ++i)
            {
                if (MaterialGraph::HasPinVector(node, i))
                {
                    hash = Mix(hash, (i + 1) << 8);
                    for (const float component : node.PinVectors[i])
                    {
                        hash = Mix(hash, FloatBits(component));
                    }
                }
            }
        }
        hash = Mix(hash, graph.Links.size());
        for (const auto& link : graph.Links)
        {
            hash = Mix(hash, link.SourcePin);
            hash = Mix(hash, link.TargetPin);
        }
        const auto& settings = graph.Settings;
        hash = Mix(hash, static_cast<uint64_t>(settings.Mode + 8));
        hash = Mix(hash, static_cast<uint64_t>(settings.RuntimeEvaluation ? 3 : 5));
        hash = Mix(hash, static_cast<uint64_t>(settings.SolidSpace + 11));
        hash = Mix(hash, static_cast<uint64_t>(settings.Surface + 8));
        hash = Mix(hash, static_cast<uint64_t>(settings.Cull + 8));
        hash = Mix(hash, static_cast<uint64_t>(settings.ShadingModel + 8));
        hash = Mix(hash, static_cast<uint64_t>(settings.ShadowMode + 8));
        hash = Mix(hash, FloatBits(settings.AlphaCutoff));
        hash = Mix(hash, static_cast<uint64_t>(settings.BakeResolution));
        hash = Mix(hash, FloatBits(settings.BakeTime));
        return hash;
    }

    namespace
    {
        const MaterialGraphNode* Source(const MaterialGraph& graph, const MaterialGraphNode& node, size_t input)
        {
            return ResolveSource(graph, node, input);
        }

        int SamplerOf(const MaterialGraphNode& node)
        {
            return std::clamp(static_cast<int>(node.Value[0]), 0, 2);
        }

        // ピクセルに依存しない（＝定数として畳み込める）か
        bool IsUniform(const MaterialGraph& graph, const MaterialGraphNode& node, int depth = 0)
        {
            if (depth > 256)
            {
                return false;
            }
            switch (node.Kind)
            {
                case Kind::UV:
                case Kind::TextureCoordinate:
                case Kind::RenderFlowUV:
                case Kind::RenderRadialPulse:
                case Kind::Panner:
                case Kind::Rotator:
                case Kind::WorldPosition:
                case Kind::WorldNormal:
                case Kind::CameraVector:
                case Kind::ReflectionVector:
                case Kind::SolidCoordinate:
                case Kind::SolidTexture:
                case Kind::SolidPattern:
                case Kind::ConvertToSolid:
                case Kind::LocalPosition:
                case Kind::SlopeMask:
                case Kind::TriplanarTexture:
                case Kind::BoundingBoxUVW:
                case Kind::HeightGradient:
                case Kind::ScreenPosition:
                case Kind::DitherOpacity:
                // オブジェクトごとに変わる値（ベイクでは定数にしない）
                case Kind::ObjectPosition:
                case Kind::ObjectScale:
                case Kind::ObjectOrientation:
                case Kind::ObjectRadius:
                case Kind::ObjectBounds:
                case Kind::PerInstanceRandom:
                case Kind::TransformVector:
                case Kind::TransformPosition:
                case Kind::CameraPosition:
                case Kind::PixelDepth:
                case Kind::DistanceFade:
                case Kind::Fresnel:
                case Kind::Time:
                case Kind::TextureSample:
                case Kind::ConvertToTexture: // 変換済み = テクスチャ、未変換 = UV を既定で使う
                case Kind::Noise:
                case Kind::DDX:
                case Kind::DDY:
                case Kind::LinearGradient:
                case Kind::RadialGradient:
                case Kind::Voronoi:
                case Kind::ProceduralNoise:
                case Kind::FractalNoise:
                case Kind::ConvertToNormalHeight:
                case Kind::ConvertToHeight:
                case Kind::ConvertToNormal:
                case Kind::NormalFromHeight:
                case Kind::BumpOffset:
                    return false;
                case Kind::StaticSwitch: {
                    // 選ばれている側だけが結果に影響する
                    const size_t chosen = node.Value[0] > 0.5f ? 0 : 1;
                    const auto* source = Source(graph, node, chosen);
                    return !source || IsUniform(graph, *source, depth + 1);
                }
                default:
                    break;
            }
            for (size_t i = 0; i < node.InputPins.size(); ++i)
            {
                if (const auto* source = Source(graph, node, i))
                {
                    if (!IsUniform(graph, *source, depth + 1))
                    {
                        return false;
                    }
                }
                else if (GraphMath::DefaultInput(node.Kind, i) != GraphMath::SpecialInput::None)
                {
                    return false; // 未接続でも UV / Time / World Position を使うピン
                }
            }
            return true;
        }

        // UV 未接続（または UV ノード / TexCoord(1,1) 直結）の Texture Sample
        const MaterialGraphNode* PlainTexture(const MaterialGraph& graph, const MaterialGraphNode* node)
        {
            // 分割したテクスチャ（X / Y / Z / W から接続）はテクスチャを直接使えない（ピクセルごとの式として焼く）
            // 変換済みの Convert To Texture もテクスチャとして直接使える（UV は 2 番目の入力）
            if (!node || (node->Kind != Kind::TextureSample && node->Kind != Kind::ConvertToTexture) ||
                node->Texture == 0 || !node->SplitPins.empty())
            {
                return nullptr;
            }
            const auto* uv = Source(graph, *node, node->Kind == Kind::ConvertToTexture ? 1 : 0);
            const bool plainCoordinate =
                uv && uv->Kind == Kind::TextureCoordinate && uv->Value[0] == 1.0f && uv->Value[1] == 1.0f;
            return (!uv || uv->Kind == Kind::UV || plainCoordinate) ? node : nullptr;
        }

        // ノード出力の定数値（IsUniform のノードだけに使う）
        struct UniformReader
        {
            const MaterialGraph& Graph;
            const Topology& Topo;
            Evaluator& Eval;
            Context Neutral;

            Value Of(const MaterialGraphNode& node)
            {
                const auto found = Topo.Index.find(node.Id);
                if (found == Topo.Index.end())
                {
                    return {};
                }
                Eval.Reset(Neutral);
                return Eval.Node(found->second);
            }

            Value PinValue(const MaterialGraphNode& output, Pin pin)
            {
                Eval.Reset(Neutral);
                return Eval.Input(output, PinIndex(pin), 0);
            }

            // ピクセルごとに変わる入力の平均（エンジンが定数しか持たない項目用）
            float Average(const MaterialGraphNode& output, Pin pin)
            {
                double sum = 0.0;
                constexpr int kGrid = 8;
                for (int y = 0; y < kGrid; ++y)
                {
                    for (int x = 0; x < kGrid; ++x)
                    {
                        Context context = Neutral;
                        context.UV[0] = (x + 0.5f) / kGrid;
                        context.UV[1] = (y + 0.5f) / kGrid;
                        context.Position[0] = context.UV[0] * 2.0f - 1.0f;
                        context.Position[1] = 1.0f - context.UV[1] * 2.0f;
                        Eval.Reset(context);
                        sum += Eval.Input(output, PinIndex(pin), 0).V[0];
                    }
                }
                return static_cast<float>(sum / (kGrid * kGrid));
            }
        };

        // 色入力が「テクスチャ（RGB / RGBA）× 定数」の形か（Base Color / Emissive をスロットへ直接割り当てる）
        struct ColorMatch
        {
            AssetHandle Texture = 0;
            Value Tint; // 乗算する色（RGBA）
        };

        bool MatchColorTexture(const MaterialGraph& graph, UniformReader& reader, const MaterialGraphNode* source,
                               ColorMatch& match)
        {
            const auto plainColor = [&](const MaterialGraphNode* node) -> const MaterialGraphNode* {
                if (!node)
                {
                    return nullptr;
                }
                if (node->Kind == Kind::ComponentMask)
                {
                    // R,G,B（,A）の先頭からの連続マスクなら同じテクスチャとして扱う
                    const bool rgb = node->Value[0] > 0.5f && node->Value[1] > 0.5f && node->Value[2] > 0.5f;
                    node = rgb ? Source(graph, *node, 0) : nullptr;
                }
                const auto* texture = PlainTexture(graph, node);
                return texture && SamplerOf(*texture) == static_cast<int>(MaterialSamplerType::Color) ? texture : nullptr;
            };
            if (const auto* texture = plainColor(source))
            {
                match.Texture = texture->Texture;
                match.Tint = Value{{1.0f, 1.0f, 1.0f, 1.0f}, 4};
                return true;
            }
            if (source && source->Kind == Kind::Multiply)
            {
                for (size_t side = 0; side < 2; ++side)
                {
                    const auto* texture = plainColor(Source(graph, *source, side));
                    const auto* tint = Source(graph, *source, 1 - side);
                    if (texture && (!tint || IsUniform(graph, *tint)))
                    {
                        match.Texture = texture->Texture;
                        Value value = tint ? reader.Of(*tint) : Value{{1.0f, 1.0f, 1.0f, 1.0f}, 4};
                        if (value.N == 1)
                        {
                            value.V[1] = value.V[2] = value.V[0];
                            value.V[3] = 1.0f;
                        }
                        else if (value.N < 4)
                        {
                            value.V[3] = 1.0f;
                        }
                        value.N = 4;
                        match.Tint = value;
                        return true;
                    }
                }
            }
            return false;
        }

        // スカラー入力が「テクスチャの 1 チャンネル × 定数」の形か（MR / AO / Height をスロットへ直接割り当てる）
        struct ChannelMatch
        {
            AssetHandle Texture = 0;
            int Channel = 0;
            float Factor = 1.0f;
        };

        bool MatchTextureChannel(const MaterialGraph& graph, UniformReader& reader, const MaterialGraphNode* source,
                                 ChannelMatch& match)
        {
            const auto channelOf = [&](const MaterialGraphNode* node, int& channel) -> const MaterialGraphNode* {
                if (!node)
                {
                    return nullptr;
                }
                channel = 0; // スカラー入力へは .x（R）が入る
                if (node->Kind == Kind::ComponentMask)
                {
                    int count = 0;
                    for (int c = 0; c < 4; ++c)
                    {
                        if (node->Value[c] > 0.5f)
                        {
                            channel = count == 0 ? c : channel;
                            ++count;
                        }
                    }
                    if (count != 1)
                    {
                        return nullptr;
                    }
                    node = Source(graph, *node, 0);
                }
                const auto* texture = PlainTexture(graph, node);
                return texture && SamplerOf(*texture) != static_cast<int>(MaterialSamplerType::Normal) ? texture : nullptr;
            };
            int channel = 0;
            if (const auto* texture = channelOf(source, channel))
            {
                match = {texture->Texture, channel, 1.0f};
                return true;
            }
            if (source && source->Kind == Kind::Multiply)
            {
                for (size_t side = 0; side < 2; ++side)
                {
                    const auto* texture = channelOf(Source(graph, *source, side), channel);
                    const auto* factor = Source(graph, *source, 1 - side);
                    if (texture && (!factor || IsUniform(graph, *factor)))
                    {
                        const Value value = factor ? reader.Of(*factor) : Value{{1.0f}, 1};
                        if (value.N != 1)
                        {
                            return false;
                        }
                        match = {texture->Texture, channel, value.V[0]};
                        return true;
                    }
                }
            }
            return false;
        }
    } // namespace

    BakeResult BakeToMaterial(const MaterialGraph& graph, const MaterialAssetData& current, uint32_t managedSlots,
                              const BakedTextures* baked)
    {
        BakeResult result;
        result.Data = current;
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
            result.Notes.push_back("Material Output ノードがありません。");
            return result;
        }
        result.HasOutput = true;

        Topology topology;
        MaterialGraphEval::BuildTopology(graph, topology);
        Evaluator evaluator(graph, topology, PreviewSampler());
        Context neutral;
        neutral.UV[0] = neutral.UV[1] = 0.5f;
        neutral.Normal[0] = neutral.Normal[1] = 0.0f;
        neutral.Normal[2] = -1.0f;
        neutral.Time = graph.Settings.BakeTime;
        UniformReader reader{graph, topology, evaluator, neutral};

        const auto source = [&](Pin pin) {
            return Source(graph, *output, PinIndex(pin));
        };
        // 接続されている、または未接続でピン横の直値が入っている（直値は定数 = 一様）
        const auto assigned = [&](Pin pin) {
            return source(pin) != nullptr || graph.HasOutputDirectValue(*output, PinIndex(pin));
        };
        const auto uniformPin = [&](Pin pin) {
            const auto* node = source(pin);
            return !node || IsUniform(graph, *node);
        };
        const auto bit = [](MaterialTextureSlot slot) {
            return 1u << static_cast<uint32_t>(slot);
        };
        const auto setSlot = [&](MaterialTextureSlot slot, AssetHandle texture) {
            if (texture != 0)
            {
                result.Data.Textures[static_cast<size_t>(slot)] = texture;
                result.ManagedSlots |= bit(slot);
            }
            else if (managedSlots & bit(slot))
            {
                result.Data.Textures[static_cast<size_t>(slot)] = 0; // グラフが設定したものだけ解除する
            }
        };
        // ピクセルごとの式：ベイク済みテクスチャがあれば割り当てる。無ければ（ベイク中）今のスロットを保つ
        const auto useBaked = [&](MaterialTextureSlot slot) {
            result.TextureBakeMask |= bit(slot);
            if (baked && (baked->Mask & bit(slot)) && baked->Textures[static_cast<size_t>(slot)] != 0)
            {
                setSlot(slot, baked->Textures[static_cast<size_t>(slot)]);
                return true;
            }
            result.ManagedSlots |= managedSlots & bit(slot);
            result.BakeMissing = true;
            return false;
        };
        const auto scalar = [](const Value& v) {
            return std::isfinite(v.V[0]) ? v.V[0] : 0.0f;
        };
        // エンジンが定数しか持たない項目：接続されていれば値（ピクセルごとの式なら平均）を入れる
        const auto scalarPin = [&](Pin pin, float minimum, float maximum, float& target) {
            const auto* node = source(pin);
            if (!node && !assigned(pin))
            {
                return false; // 未接続・直値なし：マテリアルアセットの値のまま
            }
            float value = 0.0f;
            if (!node || IsUniform(graph, *node))
            {
                value = scalar(reader.PinValue(*output, pin));
            }
            else
            {
                value = reader.Average(*output, pin);
                result.Notes.push_back(std::string(MaterialGraph::Definition(Kind::Output).Inputs[PinIndex(pin)].Name) +
                                       ": エンジンは定数のみ対応のため、UV 全体の平均値を使います");
            }
            target = std::clamp(std::isfinite(value) ? value : minimum, minimum, maximum);
            return true;
        };

        // ---- 0 / 5 / 6: Base Color とアルファ（Opacity / Opacity Mask）
        {
            const bool maskPerPixel = source(Pin::OpacityMask) && !uniformPin(Pin::OpacityMask);
            const bool opacityPerPixel = source(Pin::Opacity) && !uniformPin(Pin::Opacity);
            if (maskPerPixel && opacityPerPixel)
            {
                result.Notes.push_back("Opacity と Opacity Mask の両方がピクセルごとの式です。エンジンは 1 つのアルファ"
                                       "なので Opacity Mask（Masked）を優先します");
            }
            result.Alpha = AlphaSource::BaseColor;
            // The engine clips the BaseColor texture alpha, so constant masks must be baked too.
            if (assigned(Pin::OpacityMask))
            {
                result.Alpha = AlphaSource::OpacityMask;
            }
            else if (opacityPerPixel)
            {
                result.Alpha = AlphaSource::Opacity;
            }
            const bool perPixelAlpha = result.Alpha != AlphaSource::BaseColor;
            ColorMatch colorMatch;
            if (!perPixelAlpha && uniformPin(Pin::BaseColor))
            {
                Value color = reader.PinValue(*output, Pin::BaseColor);
                if (color.N == 1)
                {
                    color.V[1] = color.V[2] = color.V[0];
                }
                for (int i = 0; i < 3; ++i)
                {
                    result.Data.BaseColor[i] = Saturate(color.V[i]);
                }
                result.Data.BaseColor[3] = 1.0f;
                if (color.N == 4)
                {
                    result.Data.BaseColor[3] = Saturate(color.V[3]);
                }
                setSlot(MaterialTextureSlot::BaseColor, 0);
            }
            else if (!perPixelAlpha && MatchColorTexture(graph, reader, source(Pin::BaseColor), colorMatch))
            {
                for (int i = 0; i < 3; ++i)
                {
                    result.Data.BaseColor[i] = Saturate(colorMatch.Tint.V[i]);
                }
                result.Data.BaseColor[3] = Saturate(colorMatch.Tint.V[3]);
                setSlot(MaterialTextureSlot::BaseColor, colorMatch.Texture);
            }
            else if (useBaked(MaterialTextureSlot::BaseColor))
            {
                result.Data.BaseColor = {1.0f, 1.0f, 1.0f, 1.0f}; // 色とアルファはテクスチャに焼いてある
            }
            // 半透明 / マスク
            if (result.Alpha == AlphaSource::Opacity)
            {
                result.Data.Surface = MaterialSurface::Transparent;
                result.Data.DepthWrite = false;
            }
            else if (result.Alpha == AlphaSource::OpacityMask)
            {
                result.Data.Surface = MaterialSurface::Masked;
                result.Data.DepthWrite = true;
                result.Data.AlphaCutoff = ClipValue(graph);
            }
            else if (assigned(Pin::Opacity))
            {
                const float alpha = Saturate(scalar(reader.PinValue(*output, Pin::Opacity)));
                result.Data.BaseColor[3] *= alpha;
                if (alpha < 0.999f)
                {
                    result.Data.Surface = MaterialSurface::Transparent;
                    result.Data.DepthWrite = false;
                }
            }
        }
        // ---- 1 / 2: Metallic / Roughness（エンジンの MR テクスチャは R = Metallic, G = Roughness）
        {
            if (uniformPin(Pin::Metallic) && uniformPin(Pin::Roughness))
            {
                result.Data.Metallic = Saturate(scalar(reader.PinValue(*output, Pin::Metallic)));
                result.Data.Roughness = Saturate(scalar(reader.PinValue(*output, Pin::Roughness)));
                setSlot(MaterialTextureSlot::MetallicRoughness, 0);
            }
            else
            {
                ChannelMatch metallic;
                ChannelMatch roughness;
                if (MatchTextureChannel(graph, reader, source(Pin::Metallic), metallic) &&
                    MatchTextureChannel(graph, reader, source(Pin::Roughness), roughness) &&
                    metallic.Texture == roughness.Texture && metallic.Channel == 0 && roughness.Channel == 1)
                {
                    result.Data.Metallic = Saturate(metallic.Factor);
                    result.Data.Roughness = Saturate(roughness.Factor);
                    setSlot(MaterialTextureSlot::MetallicRoughness, metallic.Texture);
                }
                else if (useBaked(MaterialTextureSlot::MetallicRoughness))
                {
                    result.Data.Metallic = 1.0f; // 値はテクスチャ（R / G）に焼いてある
                    result.Data.Roughness = 1.0f;
                }
            }
        }
        // ---- 3 / 9: Normal と Normal Intensity
        {
            const auto* normal = source(Pin::Normal);
            if (!normal && !assigned(Pin::Normal))
            {
                setSlot(MaterialTextureSlot::Normal, 0);
            }
            else if (const auto* texture = normal ? PlainTexture(graph, normal) : nullptr)
            {
                if (SamplerOf(*texture) == static_cast<int>(MaterialSamplerType::Linear))
                {
                    result.Notes.push_back("Normal: 法線マップの Texture Sample は Sampler Type を Normal にしてください");
                }
                setSlot(MaterialTextureSlot::Normal, texture->Texture);
            }
            else if (!normal || IsUniform(graph, *normal))
            {
                float n[3];
                DecodeNormalInput(graph, *output, reader.PinValue(*output, Pin::Normal), n);
                const float length = sqrtf(Dot3(n, n));
                if (length < 1e-4f || (fabsf(n[0] / length) < 1e-3f && fabsf(n[1] / length) < 1e-3f && n[2] > 0.0f))
                {
                    setSlot(MaterialTextureSlot::Normal, 0); // 平らな法線 = 法線マップ無し
                }
                else
                {
                    useBaked(MaterialTextureSlot::Normal);
                }
            }
            else
            {
                useBaked(MaterialTextureSlot::Normal);
            }
            scalarPin(Pin::NormalIntensity, 0.0f, 8.0f, result.Data.NormalScale);
        }
        // ---- 4: Emissive（色 × 強度に分解）
        {
            ColorMatch match;
            if (uniformPin(Pin::Emissive))
            {
                Value color = reader.PinValue(*output, Pin::Emissive);
                if (color.N == 1)
                {
                    color.V[1] = color.V[2] = color.V[0];
                }
                const float peak = (std::max)({color.V[0], color.V[1], color.V[2], 0.0f});
                if (peak > 1e-6f)
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        result.Data.EmissiveColor[i] = (std::max)(color.V[i], 0.0f) / peak;
                    }
                    result.Data.EmissiveIntensity = (std::min)(peak, 65504.0f);
                }
                else
                {
                    result.Data.EmissiveIntensity = 0.0f;
                }
                setSlot(MaterialTextureSlot::Emissive, 0);
            }
            else if (MatchColorTexture(graph, reader, source(Pin::Emissive), match))
            {
                const float peak = (std::max)({match.Tint.V[0], match.Tint.V[1], match.Tint.V[2], 0.0f});
                for (int i = 0; i < 3; ++i)
                {
                    result.Data.EmissiveColor[i] = peak > 1e-6f ? (std::max)(match.Tint.V[i], 0.0f) / peak : 0.0f;
                }
                result.Data.EmissiveIntensity = (std::min)(peak, 65504.0f);
                setSlot(MaterialTextureSlot::Emissive, match.Texture);
            }
            else if (useBaked(MaterialTextureSlot::Emissive))
            {
                result.Data.EmissiveColor = {1.0f, 1.0f, 1.0f};
                result.Data.EmissiveIntensity = std::clamp(baked->EmissivePeak, 0.0f, 65504.0f);
            }
        }
        // ---- 7: AO（エンジンは lerp(1, テクスチャ.r, AO 強度)。定数 1 = 無し）
        {
            const auto* ao = source(Pin::AO);
            ChannelMatch match;
            if (!ao && !assigned(Pin::AO))
            {
                setSlot(MaterialTextureSlot::AO, 0);
            }
            else if ((!ao || IsUniform(graph, *ao)) && scalar(reader.PinValue(*output, Pin::AO)) >= 0.999f)
            {
                setSlot(MaterialTextureSlot::AO, 0);
            }
            else if (MatchTextureChannel(graph, reader, ao, match) && match.Channel == 0 && fabsf(match.Factor - 1.0f) < 1e-4f)
            {
                setSlot(MaterialTextureSlot::AO, match.Texture);
            }
            else if (useBaked(MaterialTextureSlot::AO))
            {
                result.Data.AOStrength = 1.0f;
            }
        }
        // ---- 8 / 10 / 11: Height（POM）と深さ / ステップ
        {
            const auto* height = source(Pin::Height);
            ChannelMatch match;
            if (!height || IsUniform(graph, *height))
            {
                setSlot(MaterialTextureSlot::Height, 0);
            }
            else if (MatchTextureChannel(graph, reader, height, match) && match.Channel == 0 &&
                     fabsf(match.Factor - 1.0f) < 1e-4f)
            {
                setSlot(MaterialTextureSlot::Height, match.Texture);
            }
            else
            {
                useBaked(MaterialTextureSlot::Height);
            }
            const bool depthLinked = scalarPin(Pin::ParallaxDepth, 0.0f, 0.2f, result.Data.ParallaxDepth);
            scalarPin(Pin::ParallaxSteps, 8.0f, 1024.0f, result.Data.ParallaxSteps);
            if (height && !IsUniform(graph, *height) && !depthLinked && result.Data.ParallaxDepth <= 0.0f)
            {
                result.Data.ParallaxDepth = 0.05f;
                result.Notes.push_back("Height: Parallax Depth が 0 だったので 0.05 にしました（Parallax Depth ピンで変更できます）");
            }
        }
        // ---- 12〜17: 屈折 / 透過 / 吸収 / シェーディング強度
        scalarPin(Pin::IOR, 1.0f, 10.0f, result.Data.IOR);
        scalarPin(Pin::Transmission, 0.0f, 1.0f, result.Data.Transmission);
        scalarPin(Pin::TransmissionRoughness, 0.0f, 1.0f, result.Data.TransmissionRoughness);
        scalarPin(Pin::Thickness, 0.0f, 65504.0f, result.Data.Thickness);
        scalarPin(Pin::ShadingStrength, 0.0f, 1.0f, result.Data.ShadingStrength);
        if (assigned(Pin::Absorption))
        {
            const auto* absorption = source(Pin::Absorption);
            if (absorption && !IsUniform(graph, *absorption))
            {
                result.Notes.push_back("Absorption: エンジンは定数のみ対応のため、UV 中央の値を使います");
            }
            Value value = reader.PinValue(*output, Pin::Absorption);
            if (value.N == 1)
            {
                value.V[1] = value.V[2] = value.V[0];
            }
            for (int i = 0; i < 3; ++i)
            {
                result.Data.AbsorptionCoefficient[i] = std::clamp(std::isfinite(value.V[i]) ? value.V[i] : 0.0f, 0.0f, 65504.0f);
            }
        }

        // ---- 焼いたチャンネルの注意（視線 / 時間依存）
        if (result.TextureBakeMask != 0)
        {
            bool view = false;
            bool time = false;
            for (size_t i = 0; i < PinIndex(Pin::Count) && i < output->InputPins.size(); ++i)
            {
                const auto* node = Source(graph, *output, i);
                const auto found = node ? topology.Index.find(node->Id) : topology.Index.end();
                if (found != topology.Index.end())
                {
                    view |= topology.ViewDependent[found->second] != 0;
                    time |= topology.TimeDependent[found->second] != 0;
                }
            }
            if (view)
            {
                result.Notes.push_back("Fresnel / Camera Vector / Bump Offset などの視線依存の式は、正面から見た値で"
                                       "テクスチャに焼かれます（POM は Height ピンを使ってください）");
            }
            if (time)
            {
                result.Notes.push_back("Time を使う式は、グラフ設定の「ベイク時刻」の値でテクスチャに焼かれます");
            }
        }

        graph.ApplyShading(result.Data);

        // ---- グラフ設定（UE のマテリアル Details）で上書き
        const auto& settings = graph.Settings;
        // マテリアルモード：Glass 以外は光学透過を切る（残っているとエンジンはガラスとして描く）
        if (settings.Mode >= 0)
        {
            const auto mode = static_cast<MaterialGraphMode>(std::clamp(settings.Mode, 0, 3));
            if (mode != MaterialGraphMode::Glass)
            {
                result.Data.Transmission = 0.0f;
            }
            else if (!assigned(Pin::Transmission) && result.Data.Transmission <= 0.0f)
            {
                result.Data.Transmission = 1.0f;
                result.Notes.push_back("Glass: Transmission が未接続で 0 だったので 1 にしました");
            }
        }
        if (settings.Surface >= 0)
        {
            result.Data.Surface = static_cast<MaterialSurface>(std::clamp(settings.Surface, 0, 2));
            result.Data.DepthWrite = result.Data.Surface != MaterialSurface::Transparent;
        }
        if (settings.Cull >= 0)
        {
            result.Data.Cull = static_cast<MaterialCull>(std::clamp(settings.Cull, 0, 2));
        }
        if (settings.ShadingModel >= 0)
        {
            result.Data.ShadingModel = static_cast<uint32_t>(std::clamp(settings.ShadingModel, 0, 5));
        }
        if (settings.ShadowMode >= 0)
        {
            result.Data.ShadowMode = static_cast<MaterialShadowMode>(std::clamp(settings.ShadowMode, 0, 4));
        }
        if (settings.AlphaCutoff >= 0.0f)
        {
            result.Data.AlphaCutoff = std::clamp(settings.AlphaCutoff, 0.0f, 1.0f);
        }
        return result;
    }

    void Forget(const MaterialGraph& graph)
    {
        MaterialGraphGpu::ForgetOwner(&graph);
        g_Caches.erase(&graph);
    }
} // namespace MaterialGraphPreview






