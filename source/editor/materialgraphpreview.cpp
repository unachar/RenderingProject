#include "pch.h"
#include "materialgraphpreview.h"
#include "materialgraph.h"
#include "materialgraphmath.h"
#include "materialgraphtextures.h"
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
        namespace GraphMath = MaterialGraphMath;
        constexpr int kNodeResolution = 64;   // ノードプレビューの評価解像度
        constexpr int kOutputResolution = 96; // Material Output の球
        constexpr double kAnimatedInterval = 1.0 / 15.0;
        constexpr float kPi = 3.14159265f;

        struct Value
        {
            float V[4]{};
            int N = 1; // 成分数（1〜4）
        };

        struct Context
        {
            float UV[2]{};
            float Position[3]{};
            float Normal[3]{0.0f, 0.0f, -1.0f};
            float Camera[3]{0.0f, 0.0f, -1.0f}; // 表面 → カメラ
            float Tangent[3]{1.0f, 0.0f, 0.0f};
            float Bitangent[3]{0.0f, 1.0f, 0.0f};
            float Time = 0.0f;
        };

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
            texture->RefCount = 1;
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
            ImTextureData* Texture = nullptr;
        };

        struct GraphCache
        {
            uint64_t Signature = ~0ull;
            int SignatureFrame = -1;
            double LastEvaluation = -1.0;
            bool Animated = false;
            std::unordered_map<uint64_t, size_t> Index; // ノード ID → Nodes の添字
            std::vector<uint8_t> Spatial;               // 法線・位置・カメラに依存する（球で表示）
            std::vector<uint8_t> TimeDependent;         // Time に依存する（アニメーション中はこれだけ再評価）
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

        // ---------------------------------------------------------------- 評価器
        float Saturate(float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        float LinearToSrgb(float v)
        {
            v = Saturate(v);
            return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
        }

        class Evaluator
        {
          public:
            Evaluator(const MaterialGraph& graph, const GraphCache& cache) : m_Graph(graph), m_Cache(cache)
            {
                m_Memo.resize(graph.Nodes.size());
                m_Done.resize(graph.Nodes.size());
            }

            void Reset(const Context& context)
            {
                m_Context = context;
                std::fill(m_Done.begin(), m_Done.end(), uint8_t{0});
            }

            Value Node(size_t index, int depth = 0)
            {
                if (m_Done[index] == 1)
                {
                    return m_Memo[index];
                }
                if (m_Done[index] == 2 || depth > 256)
                {
                    return {}; // 循環（通常は Connect で防がれる）
                }
                m_Done[index] = 2;
                Value result = Compute(m_Graph.Nodes[index], depth);
                for (float& v : result.V)
                {
                    if (!std::isfinite(v))
                    {
                        v = 0.0f;
                    }
                }
                m_Memo[index] = result;
                m_Done[index] = 1;
                return result;
            }

            const MaterialGraphNode* SourceNode(const MaterialGraphNode& node, size_t input) const
            {
                if (input < node.InputPins.size())
                {
                    if (const auto* link = m_Graph.InputLink(node.InputPins[input]))
                    {
                        return m_Graph.FindPinNode(link->SourcePin);
                    }
                }
                return nullptr;
            }

            Value Input(const MaterialGraphNode& node, size_t input, int depth)
            {
                if (const auto* source = SourceNode(node, input))
                {
                    const auto found = m_Cache.Index.find(source->Id);
                    if (found != m_Cache.Index.end())
                    {
                        return Node(found->second, depth + 1);
                    }
                }
                switch (GraphMath::DefaultInput(node.Kind, input))
                {
                    case GraphMath::SpecialInput::UV:
                        return Make(m_Context.UV, 2);
                    case GraphMath::SpecialInput::Time:
                        return Make(&m_Context.Time, 1);
                    case GraphMath::SpecialInput::WorldPosition:
                        return Make(m_Context.Position, 3);
                    default:
                        break;
                }
                const auto& definition = MaterialGraph::Definition(node.Kind);
                Value value;
                if (input < definition.Inputs.size())
                {
                    const auto& pin = definition.Inputs[input];
                    value.N = std::clamp(pin.Type == MaterialGraphType::Numeric ? 1 : static_cast<int>(pin.Type), 1, 4);
                    for (int i = 0; i < 4; ++i)
                    {
                        value.V[i] = pin.Default[i];
                    }
                }
                return value;
            }

            const Context& Ctx() const
            {
                return m_Context;
            }

          private:
            static Value Make(const float* v, int count)
            {
                Value value;
                for (int i = 0; i < count; ++i)
                {
                    value.V[i] = v[i];
                }
                value.N = count;
                return value;
            }

            static Value Promote(Value value, int count)
            {
                if (value.N == count)
                {
                    return value;
                }
                if (value.N == 1)
                {
                    value.V[1] = value.V[2] = value.V[3] = value.V[0];
                }
                else
                {
                    for (int i = value.N; i < 4; ++i)
                    {
                        value.V[i] = 0.0f;
                    }
                }
                value.N = count;
                return value;
            }

            static float SafeDivide(float a, float b)
            {
                return fabsf(b) < 1e-8f ? 0.0f : a / b;
            }

            static float Dot3(const float* a, const float* b)
            {
                return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            }

            // 入力をすべて読み、最大の成分数に揃える（スカラーは全成分へ複製）
            int Gather(const MaterialGraphNode& node, int depth, Value* inputs, size_t count, size_t promoteCount)
            {
                int width = 1;
                for (size_t i = 0; i < count; ++i)
                {
                    inputs[i] = Input(node, i, depth);
                    if (i < promoteCount)
                    {
                        width = (std::max)(width, inputs[i].N);
                    }
                }
                for (size_t i = 0; i < promoteCount; ++i)
                {
                    inputs[i] = Promote(inputs[i], width);
                }
                return width;
            }

            Value Compute(const MaterialGraphNode& node, int depth)
            {
                Value result;
                const Kind kind = node.Kind;
                if (GraphMath::IsConstantNode(kind))
                {
                    const auto type = MaterialGraph::Definition(kind).OutputType;
                    return Make(node.Value.data(), type == MaterialGraphType::Numeric ? 1 : static_cast<int>(type));
                }
                if (GraphMath::IsUnary(kind))
                {
                    result = Input(node, 0, depth);
                    for (int c = 0; c < result.N; ++c)
                    {
                        result.V[c] = GraphMath::Unary(kind, result.V[c]);
                    }
                    return result;
                }
                if (GraphMath::IsBinary(kind))
                {
                    Value inputs[2];
                    const bool power = kind == Kind::Power;
                    const int width = Gather(node, depth, inputs, 2, power ? 1 : 2);
                    result.N = width;
                    for (int c = 0; c < width; ++c)
                    {
                        result.V[c] = GraphMath::Binary(kind, inputs[0].V[c], power ? inputs[1].V[0] : inputs[1].V[c]);
                    }
                    return result;
                }
                switch (kind)
                {
                    case Kind::Reroute:
                        return Input(node, 0, depth);
                    case Kind::TextureSample: {
                        const Value uv = Promote(Input(node, 0, depth), 2);
                        result.N = 4;
                        if (node.Texture != 0 && MaterialGraphTextures::Sample(node.Texture, uv.V[0], uv.V[1], result.V))
                        {
                            return result;
                        }
                        // 読み込み中 / 未設定はチェッカー
                        const int cx = static_cast<int>(floorf(uv.V[0] * 8.0f));
                        const int cy = static_cast<int>(floorf(uv.V[1] * 8.0f));
                        const float c = ((cx + cy) & 1) ? 0.45f : 0.12f;
                        result.V[0] = result.V[1] = result.V[2] = c;
                        result.V[3] = 1.0f;
                        return result;
                    }
                    case Kind::Add:
                    case Kind::Subtract:
                    case Kind::Multiply:
                    case Kind::Divide:
                    case Kind::Lerp:
                    case Kind::Clamp:
                    case Kind::Smoothstep: {
                        Value inputs[3];
                        const size_t count = (std::min)(node.InputPins.size(), size_t{3});
                        // Lerp の Alpha はスカラー入力
                        const int width = Gather(node, depth, inputs, count, kind == Kind::Lerp ? 2 : count);
                        if (kind == Kind::Lerp)
                        {
                            inputs[2] = Promote(inputs[2], width);
                        }
                        result.N = width;
                        for (int c = 0; c < 4; ++c)
                        {
                            const float a = inputs[0].V[c];
                            const float b = inputs[1].V[c];
                            const float z = inputs[2].V[c];
                            float v = 0.0f;
                            switch (kind)
                            {
                                case Kind::Add:
                                    v = a + b;
                                    break;
                                case Kind::Subtract:
                                    v = a - b;
                                    break;
                                case Kind::Multiply:
                                    v = a * b;
                                    break;
                                case Kind::Divide:
                                    v = SafeDivide(a, b);
                                    break;
                                case Kind::Lerp:
                                    v = a + (b - a) * z;
                                    break;
                                case Kind::Clamp:
                                    v = (std::min)((std::max)(a, b), z);
                                    break;
                                default: {
                                    const float t = Saturate(SafeDivide(z - a, b - a));
                                    v = t * t * (3.0f - 2.0f * t);
                                    break;
                                }
                            }
                            result.V[c] = v;
                        }
                        return result;
                    }
                    case Kind::ConstantBiasScale: {
                        result = Input(node, 0, depth);
                        const float bias = Input(node, 1, depth).V[0];
                        const float scale = Input(node, 2, depth).V[0];
                        for (int c = 0; c < result.N; ++c)
                        {
                            result.V[c] = (result.V[c] + bias) * scale;
                        }
                        return result;
                    }
                    case Kind::Normalize: {
                        result = Promote(Input(node, 0, depth), 3);
                        const float length = sqrtf(Dot3(result.V, result.V));
                        for (int i = 0; i < 3; ++i)
                        {
                            result.V[i] = SafeDivide(result.V[i], length);
                        }
                        return result;
                    }
                    case Kind::Dot: {
                        const Value a = Promote(Input(node, 0, depth), 3);
                        const Value b = Promote(Input(node, 1, depth), 3);
                        result.V[0] = Dot3(a.V, b.V);
                        return result;
                    }
                    case Kind::CrossProduct: {
                        const Value a = Promote(Input(node, 0, depth), 3);
                        const Value b = Promote(Input(node, 1, depth), 3);
                        result.V[0] = a.V[1] * b.V[2] - a.V[2] * b.V[1];
                        result.V[1] = a.V[2] * b.V[0] - a.V[0] * b.V[2];
                        result.V[2] = a.V[0] * b.V[1] - a.V[1] * b.V[0];
                        result.N = 3;
                        return result;
                    }
                    case Kind::Distance: {
                        const Value a = Promote(Input(node, 0, depth), 3);
                        const Value b = Promote(Input(node, 1, depth), 3);
                        const float d[3] = {a.V[0] - b.V[0], a.V[1] - b.V[1], a.V[2] - b.V[2]};
                        result.V[0] = sqrtf(Dot3(d, d));
                        return result;
                    }
                    case Kind::Length: {
                        const Value a = Input(node, 0, depth);
                        float sum = 0.0f;
                        for (int c = 0; c < a.N; ++c)
                        {
                            sum += a.V[c] * a.V[c];
                        }
                        result.V[0] = sqrtf(sum);
                        return result;
                    }
                    case Kind::AppendVector: {
                        const Value a = Input(node, 0, depth);
                        const Value b = Input(node, 1, depth);
                        int n = 0;
                        for (int c = 0; c < a.N && n < 4; ++c)
                        {
                            result.V[n++] = a.V[c];
                        }
                        for (int c = 0; c < b.N && n < 4; ++c)
                        {
                            result.V[n++] = b.V[c];
                        }
                        result.N = (std::max)(n, 1);
                        return result;
                    }
                    case Kind::ComponentMask: {
                        const Value a = Input(node, 0, depth);
                        int n = 0;
                        for (int c = 0; c < 4; ++c)
                        {
                            if (node.Value[c] > 0.5f)
                            {
                                result.V[n++] = a.V[(std::min)(c, a.N - 1)];
                            }
                        }
                        if (n == 0)
                        {
                            result.V[n++] = a.V[0];
                        }
                        result.N = n;
                        return result;
                    }
                    case Kind::Fresnel: {
                        const float exponent = (std::max)(Input(node, 0, depth).V[0], 0.0f);
                        const float d = Saturate(Dot3(m_Context.Normal, m_Context.Camera));
                        result.V[0] = powf((std::max)(1.0f - d, 0.0f), exponent);
                        return result;
                    }
                    case Kind::UV:
                        return Make(m_Context.UV, 2);
                    case Kind::TextureCoordinate: {
                        const float uv[2] = {m_Context.UV[0] * node.Value[0], m_Context.UV[1] * node.Value[1]};
                        return Make(uv, 2);
                    }
                    case Kind::Panner: {
                        const Value coordinate = Promote(Input(node, 0, depth), 2);
                        const float time = Input(node, 1, depth).V[0];
                        const Value speed = Promote(Input(node, 2, depth), 2);
                        const float uv[2] = {coordinate.V[0] + speed.V[0] * time, coordinate.V[1] + speed.V[1] * time};
                        return Make(uv, 2);
                    }
                    case Kind::Rotator: {
                        const Value coordinate = Promote(Input(node, 0, depth), 2);
                        const float angle = Input(node, 1, depth).V[0] * Input(node, 2, depth).V[0];
                        const Value center = Promote(Input(node, 3, depth), 2);
                        const float s = sinf(angle), c = cosf(angle);
                        const float dx = coordinate.V[0] - center.V[0], dy = coordinate.V[1] - center.V[1];
                        const float uv[2] = {center.V[0] + dx * c - dy * s, center.V[1] + dx * s + dy * c};
                        return Make(uv, 2);
                    }
                    case Kind::WorldPosition:
                        return Make(m_Context.Position, 3);
                    case Kind::WorldNormal:
                        return Make(m_Context.Normal, 3);
                    case Kind::CameraVector:
                        return Make(m_Context.Camera, 3);
                    case Kind::ReflectionVector: {
                        // reflect(-V, N) = -V + 2 * dot(V, N) * N
                        const float* n = m_Context.Normal;
                        const float* v = m_Context.Camera;
                        const float d = Dot3(v, n);
                        const float r[3] = {-v[0] + 2.0f * d * n[0], -v[1] + 2.0f * d * n[1], -v[2] + 2.0f * d * n[2]};
                        return Make(r, 3);
                    }
                    case Kind::Time:
                        return Make(&m_Context.Time, 1);
                    case Kind::Desaturation: {
                        const Value color = Promote(Input(node, 0, depth), 3);
                        const float fraction = Input(node, 1, depth).V[0];
                        const float luminance = color.V[0] * 0.3f + color.V[1] * 0.59f + color.V[2] * 0.11f;
                        result.N = 3;
                        for (int c = 0; c < 3; ++c)
                        {
                            result.V[c] = color.V[c] + (luminance - color.V[c]) * fraction;
                        }
                        return result;
                    }
                    case Kind::If: {
                        const float a = Input(node, 0, depth).V[0];
                        const float b = Input(node, 1, depth).V[0];
                        Value branches[3] = {Input(node, 2, depth), Input(node, 3, depth), Input(node, 4, depth)};
                        int width = 1;
                        for (const auto& branch : branches)
                        {
                            width = (std::max)(width, branch.N);
                        }
                        return Promote(a > b ? branches[0] : (a == b ? branches[1] : branches[2]), width);
                    }
                    case Kind::SphereMask: {
                        const Value a = Promote(Input(node, 0, depth), 3);
                        const Value b = Promote(Input(node, 1, depth), 3);
                        const float d[3] = {a.V[0] - b.V[0], a.V[1] - b.V[1], a.V[2] - b.V[2]};
                        result.V[0] = GraphMath::SphereMask(sqrtf(Dot3(d, d)), Input(node, 2, depth).V[0],
                                                            Input(node, 3, depth).V[0]);
                        return result;
                    }
                    case Kind::CheapContrast: {
                        const float in = Input(node, 0, depth).V[0];
                        const float contrast = Input(node, 1, depth).V[0];
                        result.V[0] = Saturate(-contrast + (1.0f + 2.0f * contrast) * in);
                        return result;
                    }
                    case Kind::Noise: {
                        const Value position = Promote(Input(node, 0, depth), 3);
                        const float scale = Input(node, 1, depth).V[0];
                        result.V[0] =
                            GraphMath::Noise(position.V[0] * scale, position.V[1] * scale, position.V[2] * scale);
                        return result;
                    }
                    default:
                        return result;
                }
            }

            const MaterialGraph& m_Graph;
            const GraphCache& m_Cache;
            Context m_Context;
            std::vector<Value> m_Memo;
            std::vector<uint8_t> m_Done;
        };

        // ---------------------------------------------------------------- ユーティリティ
        ImU32 PackLinear(float r, float g, float b, float a = 1.0f)
        {
            auto encode = [](float v) {
                return static_cast<int>(LinearToSrgb(std::isfinite(v) ? v : 0.0f) * 255.0f + 0.5f);
            };
            return IM_COL32(encode(r), encode(g), encode(b), static_cast<int>(Saturate(a) * 255.0f + 0.5f));
        }

        ImU32 PackValue(const Value& value, bool color)
        {
            // スカラーはグレー、Vector2 は RG、Vector3/4 は RGB（0〜1）。テクスチャ由来の色は sRGB で表示
            auto encode = [color](float v) {
                v = std::isfinite(v) ? v : 0.0f;
                return static_cast<int>((color ? LinearToSrgb(v) : Saturate(v)) * 255.0f + 0.5f);
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
            // オブジェクト空間へ（pitch → yaw の逆回転）して球面 UV を求める
            const float cp = cosf(-pitch), sp = sinf(-pitch);
            const float oy = n[1] * cp - n[2] * sp;
            const float oz1 = n[1] * sp + n[2] * cp;
            const float cy = cosf(-yaw), sy = sinf(-yaw);
            const float ox = n[0] * cy + oz1 * sy;
            const float oz = -n[0] * sy + oz1 * cy;
            context.UV[0] = atan2f(ox, -oz) / (2.0f * kPi) + 0.5f;
            context.UV[1] = acosf(std::clamp(oy, -1.0f, 1.0f)) / kPi;
            // 接線（U 方向）と従法線（-V 方向）
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
        }

        // 中継ノードを飛ばして、入力ピンに実際に繋がっているノードを返す
        const MaterialGraphNode* ResolveSource(const MaterialGraph& graph, const MaterialGraphNode& node, size_t input)
        {
            if (input >= node.InputPins.size())
            {
                return nullptr;
            }
            const MaterialGraphNode* source = nullptr;
            if (const auto* link = graph.InputLink(node.InputPins[input]))
            {
                source = graph.FindPinNode(link->SourcePin);
            }
            for (int guard = 0; source && source->Kind == Kind::Reroute && guard < 256; ++guard)
            {
                const auto* link = graph.InputLink(source->InputPins[0]);
                source = link ? graph.FindPinNode(link->SourcePin) : nullptr;
            }
            return source;
        }

        // 色として表示する（sRGB 変換する）ノードか：テクスチャ / カラーパラメーター由来
        bool IsColorNode(const MaterialGraph& graph, const MaterialGraphNode& node)
        {
            if (node.Kind == Kind::TextureSample || node.Kind == Kind::ColorParameter)
            {
                return true;
            }
            if (node.Kind == Kind::Multiply || node.Kind == Kind::Lerp || node.Kind == Kind::Add ||
                node.Kind == Kind::Desaturation || node.Kind == Kind::Reroute)
            {
                for (size_t i = 0; i < (std::min)(node.InputPins.size(), size_t{2}); ++i)
                {
                    const auto* source = ResolveSource(graph, node, i);
                    if (source && (source->Kind == Kind::TextureSample || source->Kind == Kind::ColorParameter))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // ---------------------------------------------------------------- 依存関係の解析
        void RebuildTopology(const MaterialGraph& graph, GraphCache& cache)
        {
            cache.Index.clear();
            cache.Index.reserve(graph.Nodes.size());
            for (size_t i = 0; i < graph.Nodes.size(); ++i)
            {
                cache.Index[graph.Nodes[i].Id] = i;
            }
            // bit0: 空間（法線・位置・カメラ）依存、bit1: 時間依存
            std::vector<uint8_t> flags(graph.Nodes.size(), 0);
            std::vector<uint8_t> state(graph.Nodes.size(), 0);
            std::function<uint8_t(size_t, int)> visit = [&](size_t index, int depth) -> uint8_t {
                if (state[index] == 1 || depth > 256)
                {
                    return flags[index];
                }
                if (state[index] == 2)
                {
                    return 0;
                }
                state[index] = 2;
                const auto& node = graph.Nodes[index];
                uint8_t value = 0;
                switch (node.Kind)
                {
                    case Kind::WorldPosition:
                    case Kind::WorldNormal:
                    case Kind::CameraVector:
                    case Kind::Fresnel:
                    case Kind::ReflectionVector:
                        value |= 1;
                        break;
                    case Kind::Time:
                        value |= 2;
                        break;
                    default:
                        break;
                }
                for (size_t i = 0; i < node.InputPins.size(); ++i)
                {
                    const auto* link = graph.InputLink(node.InputPins[i]);
                    const auto* source = link ? graph.FindPinNode(link->SourcePin) : nullptr;
                    if (source)
                    {
                        const auto found = cache.Index.find(source->Id);
                        if (found != cache.Index.end())
                        {
                            value |= visit(found->second, depth + 1);
                        }
                    }
                    else
                    {
                        const auto special = GraphMath::DefaultInput(node.Kind, i);
                        value |= special == GraphMath::SpecialInput::WorldPosition ? 1 : 0;
                        value |= special == GraphMath::SpecialInput::Time ? 2 : 0;
                    }
                }
                flags[index] = value;
                state[index] = 1;
                return value;
            };
            cache.Spatial.assign(graph.Nodes.size(), 0);
            cache.TimeDependent.assign(graph.Nodes.size(), 0);
            cache.Animated = false;
            for (size_t i = 0; i < graph.Nodes.size(); ++i)
            {
                const uint8_t value = visit(i, 0);
                cache.Spatial[i] = value & 1;
                cache.TimeDependent[i] = (value & 2) ? 1 : 0;
                cache.Animated |= (value & 2) != 0;
            }
        }

        // ---------------------------------------------------------------- Material Output の簡易 PBR
        ImU32 ShadeOutput(Evaluator& evaluator, const MaterialGraph& graph, const MaterialGraphNode& output, int x,
                          int y, int resolution)
        {
            const Context& c = evaluator.Ctx();
            auto in = [&](size_t index) {
                Value v = evaluator.Input(output, index, 0);
                if (v.N == 1)
                {
                    v.V[1] = v.V[2] = v.V[3] = v.V[0];
                }
                return v;
            };
            const Value baseValue = in(0);
            const float base[3] = {Saturate(baseValue.V[0]), Saturate(baseValue.V[1]), Saturate(baseValue.V[2])};
            const float metallic = Saturate(in(1).V[0]);
            const float roughness = std::clamp(in(2).V[0], 0.04f, 1.0f);
            const Value emissive = in(4);
            const float opacity = Saturate(in(5).V[0]);
            const float mask = in(6).V[0];
            const float ao = Saturate(in(7).V[0]);
            if (ResolveSource(graph, output, 6) && mask < 0.3333f)
            {
                return 0; // Opacity Mask（UE 既定のクリップ値 0.3333）
            }

            // 法線：接続されていれば接空間の値を適用。テクスチャの場合は sRGB → 0〜1 → -1〜1 に戻す
            float n[3] = {c.Normal[0], c.Normal[1], c.Normal[2]};
            if (const auto* normalSource = ResolveSource(graph, output, 3))
            {
                Value tangent = in(3);
                if (normalSource->Kind == Kind::TextureSample)
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        tangent.V[i] = LinearToSrgb(tangent.V[i]) * 2.0f - 1.0f;
                    }
                }
                float p[3];
                for (int i = 0; i < 3; ++i)
                {
                    p[i] = c.Tangent[i] * tangent.V[0] + c.Bitangent[i] * tangent.V[1] + c.Normal[i] * tangent.V[2];
                }
                const float length = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
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
            const float ll = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            for (float& k : l)
            {
                k /= ll;
            }
            float h[3] = {l[0] + v[0], l[1] + v[1], l[2] + v[2]};
            const float hl = (std::max)(sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]), 1e-5f);
            for (float& k : h)
            {
                k /= hl;
            }
            const float nDotL = Saturate(n[0] * l[0] + n[1] * l[1] + n[2] * l[2]);
            const float nDotV = Saturate(n[0] * v[0] + n[1] * v[1] + n[2] * v[2]) + 1e-4f;
            const float nDotH = Saturate(n[0] * h[0] + n[1] * h[1] + n[2] * h[2]);
            const float vDotH = Saturate(v[0] * h[0] + v[1] * h[1] + v[2] * h[2]);
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
                const float f0 = 0.04f + (base[i] - 0.04f) * metallic;
                const float f = f0 + (1.0f - f0) * fresnelWeight;
                const float specular = d * f * g / (4.0f * nDotL * nDotV + 1e-4f);
                const float diffuse = base[i] * (1.0f - metallic) / kPi;
                const float ambientColor = (i == 2 ? 0.30f : 0.22f) * sky + 0.06f * (1.0f - sky);
                const float ambient = ambientColor * ao *
                                      (base[i] * (1.0f - metallic) + (f0 + (1.0f - f0) * ambientWeight) *
                                                                         (1.0f - roughness * 0.7f));
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
            Evaluator evaluator(graph, cache);
            // 1) 値ノード：平面用と球用の 2 パス。各画素で全ノードをメモ化して評価（O(ノード数 × 画素数)）
            for (int pass = 0; pass < 2; ++pass)
            {
                const bool sphere = pass == 1;
                std::vector<std::pair<size_t, NodeImage*>> targets;
                for (size_t i = 0; i < graph.Nodes.size(); ++i)
                {
                    const auto& node = graph.Nodes[i];
                    if (!HasPreview(node) || node.Kind == Kind::Output || (cache.Spatial[i] != 0) != sphere ||
                        cache.Collapsed.contains(node.Id) || (timeOnly && !cache.TimeDependent[i]))
                    {
                        continue;
                    }
                    auto& image = cache.Images[node.Id];
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
                for (size_t t = 0; t < targets.size(); ++t)
                {
                    colorNode[t] = IsColorNode(graph, graph.Nodes[targets[t].first]) ? 1 : 0;
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
                            targets[t].second->Pixels[static_cast<size_t>(y) * kNodeResolution + x] =
                                PackValue(evaluator.Node(targets[t].first), colorNode[t] != 0);
                        }
                    }
                }
            }
            // 2) Material Output：球に簡易 PBR
            for (const auto& node : graph.Nodes)
            {
                if (node.Kind != Kind::Output || cache.Collapsed.contains(node.Id) ||
                    (timeOnly && !cache.TimeDependent[cache.Index.at(node.Id)]))
                {
                    continue;
                }
                auto& image = cache.Images[node.Id];
                image.Sphere = true;
                image.Resolution = kOutputResolution;
                image.Pixels.assign(static_cast<size_t>(kOutputResolution) * kOutputResolution, 0);
                image.Dirty = true;
                for (int y = 0; y < kOutputResolution; ++y)
                {
                    for (int x = 0; x < kOutputResolution; ++x)
                    {
                        Context context;
                        if (SphereContext(x, y, kOutputResolution, 0.6f, 0.25f, time, context))
                        {
                            evaluator.Reset(context);
                            image.Pixels[static_cast<size_t>(y) * kOutputResolution + x] =
                                ShadeOutput(evaluator, graph, node, x, y, kOutputResolution);
                        }
                    }
                }
            }
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
                RebuildTopology(graph, cache);
            }
            if (changed || (cache.Animated && now - cache.LastEvaluation >= kAnimatedInterval))
            {
                cache.Signature = signature;
                cache.LastEvaluation = now;
                // 削除済みノードの画像を捨てる
                for (auto it = cache.Images.begin(); it != cache.Images.end();)
                {
                    if (cache.Index.contains(it->first))
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
        return node.Kind != Kind::Group && node.Kind != Kind::Reroute;
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
        GraphCache& cache = Prepare(graph);
        if (cache.Collapsed.contains(node.Id))
        {
            return;
        }
        const float areaWidth = (std::max)(width, 16.0f);
        const float areaHeight = height > 0.0f ? (std::max)(height, 16.0f) : areaWidth;
        ImGui::Dummy(ImVec2(areaWidth, areaHeight));
        const ImVec2 areaMin = ImGui::GetItemRectMin();
        // 縦横比 1:1 のまま、確保した領域の中央に置く
        const float size = (std::min)(areaWidth, areaHeight);
        const ImVec2 min(areaMin.x + (areaWidth - size) * 0.5f, areaMin.y + (areaHeight - size) * 0.5f);
        const ImVec2 max(min.x + size, min.y + size);
        ImDrawList* draw = ImGui::GetWindowDrawList();

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
            draw->AddImage(ImTextureRef(image.Texture), min, max);
        }
        else
        {
            DrawPixelsFallback(draw, min, size, image);
        }
        draw->AddRect(min, max, IM_COL32(0, 0, 0, 160));
        if (node.Kind == Kind::TextureSample && node.Texture != 0 && !MaterialGraphTextures::IsReady(node.Texture))
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
        }
        hash = Mix(hash, graph.Links.size());
        for (const auto& link : graph.Links)
        {
            hash = Mix(hash, link.SourcePin);
            hash = Mix(hash, link.TargetPin);
        }
        return hash;
    }

    namespace
    {
        const MaterialGraphNode* Source(const MaterialGraph& graph, const MaterialGraphNode& node, size_t input)
        {
            return ResolveSource(graph, node, input);
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
                case Kind::Panner:
                case Kind::Rotator:
                case Kind::WorldPosition:
                case Kind::WorldNormal:
                case Kind::CameraVector:
                case Kind::ReflectionVector:
                case Kind::Fresnel:
                case Kind::Time:
                case Kind::TextureSample:
                case Kind::Noise:
                case Kind::DDX:
                case Kind::DDY:
                    return false;
                default:
                    break;
            }
            for (size_t i = 0; i < node.InputPins.size(); ++i)
            {
                if (const auto* source = Source(graph, node, i); source && !IsUniform(graph, *source, depth + 1))
                {
                    return false;
                }
            }
            return true;
        }

        // UV 未接続（または UV ノード直結）の Texture Sample
        const MaterialGraphNode* PlainTexture(const MaterialGraph& graph, const MaterialGraphNode* node)
        {
            if (!node || node->Kind != Kind::TextureSample || node->Texture == 0)
            {
                return nullptr;
            }
            const auto* uv = Source(graph, *node, 0);
            const bool plainCoordinate = uv && uv->Kind == Kind::TextureCoordinate && uv->Value[0] == 1.0f &&
                                         uv->Value[1] == 1.0f;
            return (!uv || uv->Kind == Kind::UV || plainCoordinate) ? node : nullptr;
        }

        struct InputBake
        {
            bool Linked = false;
            bool Constant = false;
            Value Uniform;                 // Constant 時の値（テクスチャ時は乗算する色）
            AssetHandle Texture = 0;       // テクスチャ（× Uniform）
        };
    } // namespace

    BakeResult BakeToMaterial(const MaterialGraph& graph, const MaterialAssetData& current, uint32_t managedSlots)
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

        GraphCache cache;
        RebuildTopology(graph, cache);
        Evaluator evaluator(graph, cache);
        Context neutral;
        neutral.UV[0] = neutral.UV[1] = 0.5f;

        const auto& definition = MaterialGraph::Definition(Kind::Output);
        auto bake = [&](size_t input) {
            InputBake bakeResult;
            const MaterialGraphNode* source = Source(graph, *output, input);
            bakeResult.Linked = source != nullptr;
            evaluator.Reset(neutral);
            if (!source || IsUniform(graph, *source))
            {
                bakeResult.Constant = true;
                bakeResult.Uniform = evaluator.Input(*output, input, 0);
                return bakeResult;
            }
            if (const auto* texture = PlainTexture(graph, source))
            {
                bakeResult.Texture = texture->Texture;
                bakeResult.Uniform.V[0] = bakeResult.Uniform.V[1] = bakeResult.Uniform.V[2] =
                    bakeResult.Uniform.V[3] = 1.0f;
                bakeResult.Uniform.N = 4;
                return bakeResult;
            }
            if (source->Kind == Kind::Multiply)
            {
                const auto* a = Source(graph, *source, 0);
                const auto* b = Source(graph, *source, 1);
                const MaterialGraphNode* texture = PlainTexture(graph, a);
                const MaterialGraphNode* tint = b;
                size_t tintInput = 1;
                if (!texture)
                {
                    texture = PlainTexture(graph, b);
                    tint = a;
                    tintInput = 0;
                }
                if (texture && (!tint || IsUniform(graph, *tint)))
                {
                    bakeResult.Texture = texture->Texture;
                    evaluator.Reset(neutral);
                    bakeResult.Uniform = evaluator.Input(*source, tintInput, 0);
                    if (bakeResult.Uniform.N == 1)
                    {
                        bakeResult.Uniform.V[1] = bakeResult.Uniform.V[2] = bakeResult.Uniform.V[0];
                        bakeResult.Uniform.V[3] = 1.0f;
                    }
                    else if (bakeResult.Uniform.N < 4)
                    {
                        bakeResult.Uniform.V[3] = 1.0f;
                    }
                    return bakeResult;
                }
            }
            result.Notes.push_back(std::string(definition.Inputs[input].Name) +
                                   ": ピクセルごとに変わる式（UV / Fresnel / Time 等）はエンジン側が未対応のため反映していません");
            return bakeResult;
        };
        auto scalar = [](const Value& v) {
            return std::isfinite(v.V[0]) ? v.V[0] : 0.0f;
        };
        auto setSlot = [&](MaterialTextureSlot slot, AssetHandle texture) {
            const uint32_t bit = 1u << static_cast<uint32_t>(slot);
            if (texture != 0)
            {
                result.Data.Textures[static_cast<size_t>(slot)] = texture;
                result.ManagedSlots |= bit;
            }
            else if (managedSlots & bit)
            {
                result.Data.Textures[static_cast<size_t>(slot)] = 0; // グラフが設定したものだけ解除する
            }
        };

        // 0: Base Color
        {
            const InputBake b = bake(0);
            if (b.Constant || b.Texture)
            {
                Value color = b.Uniform;
                if (color.N == 1)
                {
                    color.V[1] = color.V[2] = color.V[0];
                }
                for (int i = 0; i < 3; ++i)
                {
                    result.Data.BaseColor[i] = Saturate(color.V[i]);
                }
                if (color.N == 4 && !(output->InputPins.size() > 5 && Source(graph, *output, 5)))
                {
                    result.Data.BaseColor[3] = Saturate(color.V[3]);
                }
            }
            setSlot(MaterialTextureSlot::BaseColor, b.Texture);
        }
        // 1: Metallic / 2: Roughness（テクスチャはエンジンの MetallicRoughness スロット：G=Roughness, B=Metallic）
        {
            const InputBake metallic = bake(1);
            const InputBake roughness = bake(2);
            if (metallic.Constant)
            {
                result.Data.Metallic = Saturate(scalar(metallic.Uniform));
            }
            else if (metallic.Texture)
            {
                result.Data.Metallic = Saturate(scalar(metallic.Uniform));
            }
            if (roughness.Constant || roughness.Texture)
            {
                result.Data.Roughness = Saturate(scalar(roughness.Uniform));
            }
            AssetHandle texture = metallic.Texture ? metallic.Texture : roughness.Texture;
            if (metallic.Texture && roughness.Texture && metallic.Texture != roughness.Texture)
            {
                result.Notes.push_back("Metallic と Roughness に別のテクスチャが接続されています。"
                                       "エンジンは 1 枚（G=Roughness, B=Metallic）なので Metallic 側を使います");
            }
            setSlot(MaterialTextureSlot::MetallicRoughness, texture);
        }
        // 3: Normal（テクスチャのみ）
        {
            const InputBake normal = bake(3);
            setSlot(MaterialTextureSlot::Normal, normal.Texture);
            if (normal.Linked && normal.Constant)
            {
                result.Notes.push_back("Normal: 定数の法線はエンジンに対応する項目が無いため反映していません");
            }
        }
        // 4: Emissive（色 × 強度に分解）
        {
            const InputBake emissive = bake(4);
            if (emissive.Constant || emissive.Texture)
            {
                Value color = emissive.Uniform;
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
                    result.Data.EmissiveIntensity = peak;
                }
                else
                {
                    result.Data.EmissiveIntensity = 0.0f;
                }
            }
            setSlot(MaterialTextureSlot::Emissive, emissive.Texture);
        }
        // 5: Opacity（接続したときだけ。1 未満なら半透明）
        {
            const InputBake opacity = bake(5);
            if (opacity.Linked && opacity.Constant)
            {
                const float alpha = Saturate(scalar(opacity.Uniform));
                result.Data.BaseColor[3] = alpha;
                if (alpha < 0.999f)
                {
                    result.Data.Surface = MaterialSurface::Transparent;
                    result.Data.DepthWrite = false;
                }
            }
        }
        // 6: Opacity Mask（ピクセルごとのマスクはエンジンではテクスチャのアルファで行うため、定数は反映しない）
        {
            const InputBake mask = bake(6);
            if (mask.Linked && mask.Constant)
            {
                result.Notes.push_back("Opacity Mask: 定数は反映しません（Masked はベースカラーテクスチャのアルファを使います）");
            }
        }
        // 7: AO（テクスチャはスロット、定数は強度）
        {
            const InputBake ao = bake(7);
            setSlot(MaterialTextureSlot::AO, ao.Texture);
            if (ao.Linked && ao.Constant)
            {
                result.Data.AOStrength = Saturate(scalar(ao.Uniform));
            }
        }
        return result;
    }

    void Forget(const MaterialGraph& graph)
    {
        g_Caches.erase(&graph);
    }
} // namespace MaterialGraphPreview
