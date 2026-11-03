#include "pch.h"
#include "materialgraphevaluator.h"
#include "materialgraphmath.h"
#include "procedural/noise.h"
#include "procedural/proceduralstudio.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace MaterialGraphEval
{
    namespace
    {
        using Kind = MaterialNodeKind;
        namespace GraphMath = MaterialGraphMath;
        constexpr float kPi = 3.14159265f;
        constexpr int kMaxChildLevel = 4; // 差分のネスト（DDX の中の DDX など）の上限

        Value Make(const float* v, int count)
        {
            Value value;
            for (int i = 0; i < count; ++i)
            {
                value.V[i] = v[i];
            }
            value.N = count;
            return value;
        }

        Value Make3(float x, float y, float z)
        {
            const float v[3] = {x, y, z};
            return Make(v, 3);
        }

        float SafeDivide(float a, float b)
        {
            return fabsf(b) < 1e-8f ? 0.0f : a / b;
        }

        float Dot3(const float* a, const float* b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        void Normalize3(float* v, const float* fallback)
        {
            const float length = sqrtf(Dot3(v, v));
            if (length < 1e-8f)
            {
                v[0] = fallback[0];
                v[1] = fallback[1];
                v[2] = fallback[2];
                return;
            }
            v[0] /= length;
            v[1] /= length;
            v[2] /= length;
        }
    } // namespace

    float LinearToSrgb(float v)
    {
        v = std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    }

    float SrgbToLinear(float v)
    {
        v = std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f);
        return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    }

    Value Promote(Value value, int count)
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
            const bool opaqueAlpha = value.N == 3 && count == 4;
            for (int i = value.N; i < 4; ++i)
            {
                value.V[i] = 0.0f;
            }
            if (opaqueAlpha)
            {
                value.V[3] = 1.0f;
            }
        }
        value.N = count;
        return value;
    }

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

    void BuildTopology(const MaterialGraph& graph, Topology& topology)
    {
        topology.Index.clear();
        topology.Index.reserve(graph.Nodes.size());
        for (size_t i = 0; i < graph.Nodes.size(); ++i)
        {
            topology.Index[graph.Nodes[i].Id] = i;
        }
        // bit0: 空間（法線・位置・カメラ）依存、bit1: 時間依存、bit2: カメラ方向依存
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
                    value |= 1;
                    break;
                case Kind::SolidCoordinate:
                case Kind::SolidTexture:
                case Kind::SolidPattern:
                case Kind::LocalPosition:
                case Kind::SlopeMask:
                case Kind::TriplanarTexture:
                case Kind::BoundingBoxUVW:
                case Kind::HeightGradient:
                    value |= 1; // Position / 法線を使う
                    break;
                case Kind::ConvertToSolid:
                    value |= node.Texture != 0 ? 1 : 0; // 位置 / 法線を使う
                    break;
                case Kind::CameraVector:
                case Kind::Fresnel:
                case Kind::ReflectionVector:
                case Kind::BumpOffset:
                    value |= 1 | 4;
                    break;
                case Kind::Time:
                    value |= 2;
                    break;
                default:
                    break;
            }
            for (size_t i = 0; i < node.InputPins.size(); ++i)
            {
                // テクスチャに変換済みの Convert To Texture は入力（時間 / 位置）に依存しない
                if ((node.Kind == Kind::ConvertToTexture || node.Kind == Kind::ConvertToSolid) && node.Texture != 0 &&
                    i == 0)
                {
                    continue;
                }
                if (node.Kind == Kind::StaticSwitch)
                {
                    const size_t selected = node.Value[0] > 0.5f ? 0 : 1;
                    if (i != selected)
                    {
                        continue;
                    }
                }
                const auto* link = graph.InputLink(node.InputPins[i]);
                const auto* source = link ? graph.FindPinNode(link->SourcePin) : nullptr;
                if (source)
                {
                    const auto found = topology.Index.find(source->Id);
                    if (found != topology.Index.end())
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
        topology.Spatial.assign(graph.Nodes.size(), 0);
        topology.TimeDependent.assign(graph.Nodes.size(), 0);
        topology.ViewDependent.assign(graph.Nodes.size(), 0);
        topology.Animated = false;
        for (size_t i = 0; i < graph.Nodes.size(); ++i)
        {
            const uint8_t value = visit(i, 0);
            topology.Spatial[i] = value & 1;
            topology.TimeDependent[i] = (value & 2) ? 1 : 0;
            topology.ViewDependent[i] = (value & 4) ? 1 : 0;
            topology.Animated |= (value & 2) != 0;
        }
    }

    Evaluator::Evaluator(const MaterialGraph& graph, const Topology& topology, TextureSampler sampler)
        : m_Graph(graph), m_Topology(topology), m_Sampler(std::move(sampler))
    {
        m_Memo.resize(graph.Nodes.size());
        m_Done.resize(graph.Nodes.size());
    }

    Evaluator::~Evaluator() = default;

    void Evaluator::Reset(const Context& context)
    {
        m_Context = context;
        std::fill(m_Done.begin(), m_Done.end(), uint8_t{0});
    }

    Value Evaluator::Node(size_t index, int depth)
    {
        if (index >= m_Done.size())
        {
            return {};
        }
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

    Value Evaluator::Input(const MaterialGraphNode& node, size_t input, int depth)
    {
        if (input < node.InputPins.size())
        {
            if (const auto* link = m_Graph.InputLink(node.InputPins[input]))
            {
                size_t outputIndex = 0;
                if (const auto* source = m_Graph.FindPinNode(link->SourcePin, nullptr, &outputIndex))
                {
                    const auto found = m_Topology.Index.find(source->Id);
                    if (found != m_Topology.Index.end())
                    {
                        Value value = Node(found->second, depth + 1);
                        if (outputIndex > 0)
                        {
                            // 分割した出力（X / Y / Z / W）：成分を取り出す。無い成分は 0（スカラーは全成分同じ）
                            const int component = static_cast<int>(outputIndex) - 1;
                            const float scalar = value.N == 1 ? value.V[0]
                                                 : component < value.N ? value.V[component]
                                                                       : 0.0f;
                            value = Value{};
                            value.V[0] = scalar;
                            value.N = 1;
                        }
                        return value;
                    }
                }
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
            const auto defaults = MaterialGraph::UnconnectedInputValue(node, input); // ピン横のボックスの値
            for (int i = 0; i < 4; ++i)
            {
                value.V[i] = defaults[i];
            }
        }
        return value;
    }

    Value Evaluator::InputAt(const MaterialGraphNode& node, size_t input, float u, float v)
    {
        Context context = m_Context;
        context.UV[0] = u;
        context.UV[1] = v;
        Reset(context);
        return Input(node, input, 0);
    }

    // UV を 1 画素（axis 0 = U、1 = V）ずらした位置で入力を評価する（前進差分用）
    Value Evaluator::Shifted(const MaterialGraphNode& node, size_t input, int axis, int depth)
    {
        if (m_Level >= kMaxChildLevel)
        {
            return Input(node, input, depth);
        }
        if (!m_Child)
        {
            m_Child = std::make_unique<Evaluator>(m_Graph, m_Topology, m_Sampler);
            m_Child->m_Level = m_Level + 1;
        }
        Context context = m_Context;
        const float step = (std::max)(context.PixelSize, 1e-5f);
        context.UV[axis] += step;
        // 位置も UV の向き（T / B）へ同じだけ動かす（World Position を使う式の差分）
        const float* direction = axis == 0 ? context.Tangent : context.Bitangent;
        for (int i = 0; i < 3; ++i)
        {
            context.Position[i] += direction[i] * step * 2.0f;
        }
        m_Child->Reset(context);
        return m_Child->Input(node, input, depth);
    }

    int Evaluator::Gather(const MaterialGraphNode& node, int depth, Value* inputs, size_t count, size_t promoteCount)
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

    Value Evaluator::Compute(const MaterialGraphNode& node, int depth)
    {
        Value result;
        const Kind kind = node.Kind;
        if (GraphMath::IsConstantNode(kind))
        {
            const auto type = MaterialGraph::Definition(kind).OutputType;
            return Make(node.Value.data(), type == MaterialGraphType::Numeric ? 1 : static_cast<int>(type));
        }
        if (kind == Kind::DDX || kind == Kind::DDY)
        {
            // 1 画素ずらした位置との差（GPU の ddx / ddy と同じく「隣の画素 - 自分」）
            const Value here = Input(node, 0, depth);
            const Value there = Shifted(node, 0, kind == Kind::DDX ? 0 : 1, depth);
            result.N = here.N;
            for (int c = 0; c < here.N; ++c)
            {
                result.V[c] = there.V[c] - here.V[c];
            }
            return result;
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
            case Kind::BreakVector: // 成分の取り出しは接続先の Input で行う
                return Input(node, 0, depth);
            case Kind::ConvertToTexture:
                if (node.Texture == 0)
                {
                    return Promote(Input(node, 0, depth), 4); // 未変換：入力をそのまま通す
                }
                [[fallthrough]];
            case Kind::TextureSample: {
                const Value uv = Promote(Input(node, node.Kind == Kind::ConvertToTexture ? 1 : 0, depth), 2);
                const auto sampler = static_cast<MaterialSamplerType>(std::clamp(static_cast<int>(node.Value[0]), 0, 2));
                result.N = 4;
                if (node.Texture != 0 && m_Sampler &&
                    m_Sampler(node.Texture, uv.V[0], uv.V[1], sampler != MaterialSamplerType::Color, result.V))
                {
                    if (sampler == MaterialSamplerType::Normal)
                    {
                        float n[3] = {result.V[0] * 2.0f - 1.0f, result.V[1] * 2.0f - 1.0f, result.V[2] * 2.0f - 1.0f};
                        if (result.V[2] <= 0.0f) // BC5（2ch）の法線は Z を再構成する
                        {
                            n[2] = sqrtf(std::clamp(1.0f - n[0] * n[0] - n[1] * n[1], 0.0f, 1.0f));
                        }
                        result.V[0] = n[0];
                        result.V[1] = n[1];
                        result.V[2] = n[2];
                        result.V[3] = 1.0f;
                    }
                    return result;
                }
                // 読み込み中 / 未設定はチェッカー（法線サンプラーは平らな法線）
                if (sampler == MaterialSamplerType::Normal)
                {
                    result.V[0] = result.V[1] = 0.0f;
                    result.V[2] = result.V[3] = 1.0f;
                    return result;
                }
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
                            const float t = GraphMath::Saturate(SafeDivide(z - a, b - a));
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
                return Make3(a.V[1] * b.V[2] - a.V[2] * b.V[1], a.V[2] * b.V[0] - a.V[0] * b.V[2],
                             a.V[0] * b.V[1] - a.V[1] * b.V[0]);
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
                const float d = GraphMath::Saturate(Dot3(m_Context.Normal, m_Context.Camera));
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
            // ---------------------------------------------------------------- ソリッドテクスチャ
            case Kind::SolidCoordinate: {
                const Value position = Promote(Input(node, 0, depth), 3);
                const float scale = Input(node, 1, depth).V[0];
                const Value offset = Promote(Input(node, 2, depth), 3);
                const bool raw = m_Graph.InputLink(node.InputPins[0]) != nullptr; // 繋いだ座標はそのまま
                float p[3];
                for (int i = 0; i < 3; ++i)
                {
                    const float base = raw ? position.V[i] : position.V[i] * 0.5f + 0.5f;
                    p[i] = base * scale + offset.V[i];
                }
                return Make(p, 3);
            }
            case Kind::SolidTexture:
            case Kind::SolidPattern: {
                // プロシージャルスタジオの立体テクスチャ（TextureScalar / TextureColor, solid = true）と同じ計算
                static const ProcStudio::TextureKind kinds[] = {
                    ProcStudio::TextureKind::FractalNoise, ProcStudio::TextureKind::Marble,
                    ProcStudio::TextureKind::Wood,         ProcStudio::TextureKind::Granite,
                    ProcStudio::TextureKind::Voronoi,      ProcStudio::TextureKind::FractalCloud};
                Value position = Promote(Input(node, 0, depth), 3);
                if (!m_Graph.InputLink(node.InputPins[0]))
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        position.V[i] = position.V[i] * 0.5f + 0.5f; // スタジオと同じ 0〜1 の座標
                    }
                }
                ProcStudio::TextureParams params;
                params.Kind = kinds[std::clamp(static_cast<int>(node.Value[0]), 0,
                                               static_cast<int>(MaterialSolidKind::Count) - 1)];
                params.Noise.BasisType = static_cast<ProcStudio::Basis>(
                    std::clamp(static_cast<int>(node.Value[1]), 0, GraphMath::kNoiseBasisCount - 1));
                params.Noise.Fractal = static_cast<ProcStudio::FractalType>(
                    std::clamp(static_cast<int>(node.Value[2]), 0, GraphMath::kNoiseFractalCount - 1));
                params.Noise.Seed = static_cast<uint32_t>((std::max)(node.Value[3], 0.0f));
                params.Noise.Frequency = Input(node, 1, depth).V[0];
                params.Noise.Octaves = std::clamp(static_cast<int>(Input(node, 2, depth).V[0]), 1, 12);
                params.Noise.Lacunarity = Input(node, 3, depth).V[0];
                params.Noise.Gain = Input(node, 4, depth).V[0];
                params.Noise.WarpStrength = Input(node, 5, depth).V[0];
                params.Noise.RidgeOffset = Input(node, 6, depth).V[0];
                params.Turbulence = Input(node, 7, depth).V[0];
                params.RingCount = Input(node, 8, depth).V[0];
                params.Coverage = Input(node, 9, depth).V[0];
                params.Softness = Input(node, 10, depth).V[0];
                params.Contrast = Input(node, 11, depth).V[0];
                params.Bias = Input(node, 12, depth).V[0];
                params.Tileable = false;
                float p[3];
                for (int i = 0; i < 3; ++i)
                {
                    p[i] = std::isfinite(position.V[i]) ? std::clamp(position.V[i], -10000.0f, 10000.0f) : 0.0f;
                }
                if (node.Kind == Kind::SolidPattern)
                {
                    result.V[0] = ProcStudio::TextureScalar(params, p[0], p[1], p[2], true);
                    return result;
                }
                for (int color = 0; color < 3; ++color)
                {
                    const Value c = Promote(Input(node, 13 + color, depth), 3);
                    auto& target = color == 0 ? params.ColorA : color == 1 ? params.ColorB : params.ColorC;
                    target = {c.V[0], c.V[1], c.V[2]};
                }
                const ProcStudio::Float3 c = ProcStudio::TextureColor(params, p[0], p[1], p[2], true);
                return Make3(c.X, c.Y, c.Z);
            }
            // ---------------------------------------------------------------- 便利ノード（CPU プレビュー / ベイク）
            case Kind::LocalPosition:
                return Make(m_Context.Position, 3); // プレビュー / ベイクではワールド位置と同じ
            // ---------------------------------------------------------------- オブジェクト / カメラ（プレビュー / ベイク用の値）
            //  プレビューにはオブジェクトが無いので、原点 0・スケール 1・範囲 -1〜1・乱数 0.5 として評価する
            case Kind::ObjectPosition:
                return Make3(0.0f, 0.0f, 0.0f);
            case Kind::ObjectScale:
                return Make3(1.0f, 1.0f, 1.0f);
            case Kind::ObjectOrientation:
                return Make3(0.0f, 1.0f, 0.0f);
            case Kind::ObjectRadius:
                result.V[0] = 1.7320508f;
                return result;
            case Kind::ObjectBounds: {
                const int mode = std::clamp(static_cast<int>(node.Value[0]), 0, 3);
                const float value = mode == 0 ? 2.0f : mode == 1 ? 0.0f : mode == 2 ? -1.0f : 1.0f;
                return Make3(value, value, value);
            }
            case Kind::BoundingBoxUVW:
                return Make3(m_Context.Position[0] * 0.5f + 0.5f, m_Context.Position[1] * 0.5f + 0.5f,
                             m_Context.Position[2] * 0.5f + 0.5f);
            case Kind::PerInstanceRandom:
                result.V[0] = 0.5f;
                return result;
            case Kind::TransformVector:
            case Kind::TransformPosition:
                return Promote(Input(node, 0, depth), 3); // プレビューはローカル = ワールド = ビュー
            case Kind::CameraPosition:
                return Make3(m_Context.Position[0] + m_Context.Camera[0] * 3.0f,
                             m_Context.Position[1] + m_Context.Camera[1] * 3.0f,
                             m_Context.Position[2] + m_Context.Camera[2] * 3.0f);
            case Kind::PixelDepth:
                result.V[0] = 3.0f;
                return result;
            case Kind::ScreenPosition: {
                const float uv[2] = {m_Context.UV[0], m_Context.UV[1]};
                return Make(uv, 2);
            }
            case Kind::DitherOpacity: {
                const float opacity = Input(node, 0, depth).V[0];
                const float pixels = 1.0f / (std::max)(m_Context.PixelSize, 1e-5f);
                const int x = static_cast<int>(floorf(m_Context.UV[0] * pixels));
                const int y = static_cast<int>(floorf(m_Context.UV[1] * pixels));
                result.V[0] = opacity > GraphMath::BayerThreshold(x, y) ? 1.0f : 0.0f;
                return result;
            }
            case Kind::DistanceFade: {
                const float start = Input(node, 0, depth).V[0];
                const float end = Input(node, 1, depth).V[0];
                result.V[0] = GraphMath::Saturate((3.0f - start) / (fabsf(end - start) > 1e-6f ? end - start : 1e-6f));
                return result;
            }
            case Kind::HeightGradient: {
                const float start = Input(node, 0, depth).V[0];
                const float end = Input(node, 1, depth).V[0];
                const int mode = std::clamp(static_cast<int>(node.Value[0]), 0, 2);
                const float y = mode == 2 ? m_Context.Position[1] * 0.5f + 0.5f : m_Context.Position[1];
                result.V[0] = GraphMath::Saturate((y - start) / (fabsf(end - start) > 1e-6f ? end - start : 1e-6f));
                return result;
            }
            case Kind::VertexColor: {
                const float white[4] = {1, 1, 1, 1};
                return Make(white, 4);
            }
            case Kind::CameraDistance:
                result.V[0] = 3.0f; // プレビュー用の固定距離（エンジンでは実際の距離）
                return result;
            case Kind::SlopeMask: {
                Value up = Promote(Input(node, 0, depth), 3);
                const float fallback[3] = {0, 1, 0};
                Normalize3(up.V, fallback);
                const float threshold = Input(node, 1, depth).V[0];
                const float softness = (std::max)(Input(node, 2, depth).V[0], 1e-4f);
                const float d = Dot3(m_Context.Normal, up.V);
                float t = GraphMath::Saturate((d - (threshold - softness)) / (2.0f * softness));
                result.V[0] = t * t * (3.0f - 2.0f * t);
                return result;
            }
            case Kind::HeightLerp: {
                const Value a = Input(node, 0, depth);
                const Value b = Input(node, 1, depth);
                const int count = (std::max)(a.N, b.N);
                const Value pa = Promote(a, count);
                const Value pb = Promote(b, count);
                const float height = Input(node, 2, depth).V[0];
                const float phase = Input(node, 3, depth).V[0];
                const float contrast = Input(node, 4, depth).V[0];
                const float x = GraphMath::Saturate(height + phase * 2.0f - 1.0f);
                const float t = GraphMath::Saturate(-contrast + (1.0f + 2.0f * contrast) * x);
                result.N = count;
                for (int i = 0; i < 4; ++i)
                {
                    result.V[i] = pa.V[i] + (pb.V[i] - pa.V[i]) * t;
                }
                return result;
            }
            case Kind::ColorAdjust: {
                const Value color = Promote(Input(node, 0, depth), 3);
                const float brightness = Input(node, 1, depth).V[0];
                const float contrast = Input(node, 2, depth).V[0];
                const float saturation = Input(node, 3, depth).V[0];
                float c[3];
                for (int i = 0; i < 3; ++i)
                {
                    c[i] = (color.V[i] * brightness - 0.5f) * contrast + 0.5f;
                }
                const float luminance = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                for (int i = 0; i < 3; ++i)
                {
                    c[i] = luminance + (c[i] - luminance) * saturation;
                }
                return Make(c, 3);
            }
            case Kind::TriplanarTexture: {
                const Value position = Promote(Input(node, 0, depth), 3);
                const float scale = Input(node, 1, depth).V[0];
                const float sharpness = (std::max)(Input(node, 2, depth).V[0], 0.01f);
                const auto sampler = static_cast<MaterialSamplerType>(std::clamp(static_cast<int>(node.Value[0]), 0, 2));
                const bool raw = sampler != MaterialSamplerType::Color;
                float p[3];
                for (int i = 0; i < 3; ++i)
                {
                    const float v = position.V[i] * scale;
                    p[i] = std::isfinite(v) ? std::clamp(v, -10000.0f, 10000.0f) : 0.0f;
                }
                float w[3];
                float total = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    w[i] = powf(fabsf(m_Context.Normal[i]) + 1e-5f, sharpness);
                    total += w[i];
                }
                const auto sample = [&](float u, float v, float* out) {
                    if (node.Texture == 0 || !m_Sampler || !m_Sampler(node.Texture, u, v, raw, out))
                    {
                        out[0] = out[1] = out[2] = 0.5f;
                        out[3] = 1.0f;
                    }
                };
                float a[4], b[4], c[4];
                sample(p[1], p[2], a);
                sample(p[0], p[2], b);
                sample(p[0], p[1], c);
                result.N = 4;
                for (int i = 0; i < 4; ++i)
                {
                    result.V[i] = (a[i] * w[0] + b[i] * w[1] + c[i] * w[2]) / total;
                }
                if (sampler == MaterialSamplerType::Normal)
                {
                    float n[3] = {result.V[0] * 2.0f - 1.0f, result.V[1] * 2.0f - 1.0f, result.V[2] * 2.0f - 1.0f};
                    const float up[3] = {0, 0, 1};
                    Normalize3(n, up);
                    result.V[0] = n[0];
                    result.V[1] = n[1];
                    result.V[2] = n[2];
                    result.V[3] = 1.0f;
                }
                return result;
            }
            case Kind::ConvertToSolid: {
                if (node.Texture == 0)
                {
                    return Promote(Input(node, 0, depth), 4); // 元画像なし：入力をそのまま通す
                }
                Value position = Promote(Input(node, 1, depth), 3);
                const bool raw = m_Graph.InputLink(node.InputPins[1]) != nullptr;
                const float scale = Input(node, 2, depth).V[0];
                float p[3];
                for (int i = 0; i < 3; ++i)
                {
                    const float base = raw ? position.V[i] : position.V[i] * 0.5f + 0.5f;
                    const float v = base * scale;
                    p[i] = std::isfinite(v) ? std::clamp(v, -10000.0f, 10000.0f) : 0.0f;
                }
                const bool linear = static_cast<int>(node.Value[1]) != static_cast<int>(MaterialSamplerType::Color);
                const auto sample = [&](float u, float v, float* out) {
                    if (!m_Sampler || !m_Sampler(node.Texture, u, v, linear, out))
                    {
                        out[0] = out[1] = out[2] = 0.5f;
                        out[3] = 1.0f;
                    }
                };
                result.N = 4;
                const int method = std::clamp(static_cast<int>(node.Value[0]), 0,
                                              static_cast<int>(MaterialSolidConvertMethod::Count) - 1);
                if (method == static_cast<int>(MaterialSolidConvertMethod::Stochastic))
                {
                    // 3 方向投影 × ヘックス タイル（タイルごとにランダムな位置 / 回転）。MGVM_SolidStochastic と同じ
                    using ProcStudio::NoiseDetail::Hash;
                    using ProcStudio::NoiseDetail::Unit;
                    const float sharpness = (std::max)(Input(node, 5, depth).V[0], 0.01f);
                    const float randomness = std::clamp(Input(node, 6, depth).V[0], 0.0f, 1.0f);
                    const uint32_t seed = static_cast<uint32_t>((std::max)(node.Value[2], 0.0f));
                    const float uvs[3][2] = {{p[1], p[2]}, {p[0], p[2]}, {p[0], p[1]}};
                    float total = 0.0f;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const float weight = powf(fabsf(m_Context.Normal[axis]) + 1e-5f, sharpness);
                        total += weight;
                        const uint32_t planeSeed = Hash(seed + static_cast<uint32_t>(axis) * 0x9E3779B9u);
                        const auto tiles = GraphMath::HexGrid(uvs[axis][0], uvs[axis][1]);
                        for (int t = 0; t < 3; ++t)
                        {
                            const uint32_t h = Hash(planeSeed ^ Hash(static_cast<uint32_t>(tiles.Vertex[t][0]) * 0x8da6b343u ^
                                                                     Hash(static_cast<uint32_t>(tiles.Vertex[t][1]) * 0xd8163841u)));
                            const float angle = Unit(Hash(h ^ 0x68bc21ebu)) * 6.28318531f * randomness;
                            const float c = cosf(angle), sn = sinf(angle);
                            const float u = uvs[axis][0] * c - uvs[axis][1] * sn + Unit(h);
                            const float v = uvs[axis][0] * sn + uvs[axis][1] * c + Unit(Hash(h));
                            float texel[4];
                            sample(u, v, texel);
                            for (int i = 0; i < 4; ++i)
                            {
                                result.V[i] += texel[i] * tiles.Weight[t] * weight;
                            }
                        }
                    }
                    for (int i = 0; i < 4; ++i)
                    {
                        result.V[i] /= (std::max)(total, 1e-6f);
                    }
                }
                else if (method == static_cast<int>(MaterialSolidConvertMethod::Triplanar))
                {
                    const float sharpness = (std::max)(Input(node, 5, depth).V[0], 0.01f);
                    float w[3];
                    float total = 0.0f;
                    for (int i = 0; i < 3; ++i)
                    {
                        w[i] = powf(fabsf(m_Context.Normal[i]) + 1e-5f, sharpness);
                        total += w[i];
                    }
                    float a[4], b[4], c[4];
                    sample(p[1], p[2], a);
                    sample(p[0], p[2], b);
                    sample(p[0], p[1], c);
                    for (int i = 0; i < 4; ++i)
                    {
                        result.V[i] = (a[i] * w[0] + b[i] * w[1] + c[i] * w[2]) / total;
                    }
                }
                else
                {
                    // ProcStudio::VolumeFromImage と同じ：x / z の平面を y でずらしながら、オクターブで重ねる
                    using ProcStudio::NoiseDetail::Hash;
                    using ProcStudio::NoiseDetail::Unit;
                    const int octaves = std::clamp(static_cast<int>(Input(node, 3, depth).V[0]), 1, 6);
                    const float variation = Input(node, 4, depth).V[0];
                    const uint32_t seed = static_cast<uint32_t>((std::max)(node.Value[2], 0.0f));
                    float sum[4]{};
                    float norm = 0.0f;
                    float amplitude = 1.0f;
                    for (int o = 0; o < octaves; ++o)
                    {
                        const float octaveScale = static_cast<float>(1 << o);
                        const uint32_t h = Hash(seed + static_cast<uint32_t>(o) * 977u);
                        const float angle = p[1] * 2.0f * kPi;
                        const float swing = std::round(variation * 2.0f) * 0.5f / octaveScale;
                        const float ox = swing * cosf(angle) + Unit(h);
                        const float oz = swing * sinf(angle) + Unit(Hash(h));
                        float a[4], b[4];
                        sample(p[0] * octaveScale + ox, p[2] * octaveScale + oz, a);
                        sample(p[2] * octaveScale - oz + 0.37f, p[1] * octaveScale + ox, b);
                        for (int i = 0; i < 4; ++i)
                        {
                            sum[i] += (a[i] * 0.7f + b[i] * 0.3f) * amplitude;
                        }
                        norm += amplitude;
                        amplitude *= 0.5f;
                    }
                    for (int i = 0; i < 4; ++i)
                    {
                        result.V[i] = sum[i] / norm;
                    }
                }
                if (node.Value[3] > 0.5f) // グレースケール（スタジオのボリュームと同じ RGB 平均）
                {
                    const float gray = (result.V[0] + result.V[1] + result.V[2]) / 3.0f;
                    result.V[0] = result.V[1] = result.V[2] = gray;
                }
                return result;
            }
            case Kind::ThreeColorRamp: {
                const float t = Input(node, 0, depth).V[0];
                const Value a = Promote(Input(node, 1, depth), 3);
                const Value b = Promote(Input(node, 2, depth), 3);
                const Value c = Promote(Input(node, 3, depth), 3);
                float out[3];
                GraphMath::ThreeColorRamp(t, a.V, b.V, c.V, out);
                return Make(out, 3);
            }
            case Kind::WorldNormal:
                return Make(m_Context.Normal, 3);
            case Kind::CameraVector:
                return Make(m_Context.Camera, 3);
            case Kind::ReflectionVector: {
                // reflect(-V, N) = -V + 2 * dot(V, N) * N
                const float* n = m_Context.Normal;
                const float* v = m_Context.Camera;
                const float d = Dot3(v, n);
                return Make3(-v[0] + 2.0f * d * n[0], -v[1] + 2.0f * d * n[1], -v[2] + 2.0f * d * n[2]);
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
                result.V[0] =
                    GraphMath::SphereMask(sqrtf(Dot3(d, d)), Input(node, 2, depth).V[0], Input(node, 3, depth).V[0]);
                return result;
            }
            case Kind::CheapContrast: {
                const float in = Input(node, 0, depth).V[0];
                const float contrast = Input(node, 1, depth).V[0];
                result.V[0] = GraphMath::Saturate(-contrast + (1.0f + 2.0f * contrast) * in);
                return result;
            }
            case Kind::Noise: {
                const Value position = Promote(Input(node, 0, depth), 3);
                const float scale = Input(node, 1, depth).V[0];
                result.V[0] = GraphMath::Noise(position.V[0] * scale, position.V[1] * scale, position.V[2] * scale);
                return result;
            }
            // ---------------------------------------------------------------- 2026-11 追加ノード
            case Kind::StaticSwitch: {
                // 選ばれなかった側は評価しない（UE の Static Switch と同じくコンパイル時に分岐）
                Value chosen = Input(node, node.Value[0] > 0.5f ? 0 : 1, depth);
                return chosen;
            }
            case Kind::Pi:
                result.V[0] = kPi * node.Value[0];
                return result;
            case Kind::RemapValueRange: {
                result = Input(node, 0, depth);
                const float inLow = Input(node, 1, depth).V[0];
                const float inHigh = Input(node, 2, depth).V[0];
                const float outLow = Input(node, 3, depth).V[0];
                const float outHigh = Input(node, 4, depth).V[0];
                for (int c = 0; c < result.N; ++c)
                {
                    const float t = SafeDivide(result.V[c] - inLow, inHigh - inLow);
                    result.V[c] = outLow + (outHigh - outLow) * t;
                }
                return result;
            }
            case Kind::Posterize: {
                result = Input(node, 0, depth);
                const float steps = (std::max)(Input(node, 1, depth).V[0], 1.0f);
                for (int c = 0; c < result.N; ++c)
                {
                    result.V[c] = floorf(result.V[c] * steps) / steps;
                }
                return result;
            }
            case Kind::HueShift: {
                const Value color = Promote(Input(node, 0, depth), 3);
                const float shift = Input(node, 1, depth).V[0];
                float hsv[3];
                GraphMath::RgbToHsv(color.V, hsv);
                hsv[0] = GraphMath::Frac(hsv[0] + shift);
                result.N = 3;
                GraphMath::HsvToRgb(hsv, result.V);
                return result;
            }
            case Kind::Blend: {
                const Value base = Promote(Input(node, 0, depth), 3);
                const Value blend = Promote(Input(node, 1, depth), 3);
                const float opacity = Input(node, 2, depth).V[0];
                const int mode = static_cast<int>(node.Value[0]);
                result.N = 3;
                for (int c = 0; c < 3; ++c)
                {
                    const float blended = GraphMath::BlendChannel(mode, base.V[c], blend.V[c]);
                    result.V[c] = base.V[c] + (blended - base.V[c]) * opacity;
                }
                return result;
            }
            case Kind::LinearGradient: {
                const Value uv = Promote(Input(node, 0, depth), 2);
                result.V[0] = GraphMath::Saturate(uv.V[node.Value[0] > 0.5f ? 1 : 0]);
                return result;
            }
            case Kind::RadialGradient: {
                const Value uv = Promote(Input(node, 0, depth), 2);
                const Value center = Promote(Input(node, 1, depth), 2);
                const float radius = (std::max)(Input(node, 2, depth).V[0], 1e-5f);
                const float density = Input(node, 3, depth).V[0];
                const float dx = uv.V[0] - center.V[0], dy = uv.V[1] - center.V[1];
                const float t = GraphMath::Saturate(1.0f - sqrtf(dx * dx + dy * dy) / radius);
                result.V[0] = powf(t, (std::max)(density, 0.0f));
                return result;
            }
            // ---------------------------------------------------------------- プロシージャルスタジオのノイズ
            case Kind::ProceduralNoise:
            case Kind::FractalNoise: {
                const bool fractal = node.Kind == Kind::FractalNoise;
                const Value uv = Promote(Input(node, 0, depth), 2);
                const float z = Input(node, 1, depth).V[0];
                ProcStudio::NoiseParams params;
                params.BasisType = static_cast<ProcStudio::Basis>(
                    std::clamp(static_cast<int>(node.Value[0]), 0, GraphMath::kNoiseBasisCount - 1));
                params.Fractal = fractal ? static_cast<ProcStudio::FractalType>(std::clamp(
                                               static_cast<int>(node.Value[1]), 0, GraphMath::kNoiseFractalCount - 1))
                                         : ProcStudio::FractalType::None;
                params.Seed = static_cast<uint32_t>((std::max)(node.Value[fractal ? 2 : 1], 0.0f));
                const bool tileable = node.Value[fractal ? 3 : 2] > 0.5f;
                params.Frequency = (std::max)(Input(node, 2, depth).V[0], 0.001f);
                if (fractal)
                {
                    params.Octaves = std::clamp(static_cast<int>(Input(node, 3, depth).V[0]), 1, 12);
                    params.Lacunarity = Input(node, 4, depth).V[0];
                    params.Gain = Input(node, 5, depth).V[0];
                    params.WarpStrength = Input(node, 6, depth).V[0];
                    params.RidgeOffset = Input(node, 7, depth).V[0];
                }
                else
                {
                    params.Direction = Input(node, 3, depth).V[0];
                    params.Anisotropy = std::clamp(Input(node, 4, depth).V[0], 1.0f, 8.0f);
                    params.KernelFrequency = Input(node, 5, depth).V[0];
                    params.Phase = Input(node, 6, depth).V[0];
                }
                const float x = std::isfinite(uv.V[0]) ? std::clamp(uv.V[0], -10000.0f, 10000.0f) : 0.0f;
                const float y = std::isfinite(uv.V[1]) ? std::clamp(uv.V[1], -10000.0f, 10000.0f) : 0.0f;
                const float w = std::isfinite(z) ? std::clamp(z, -10000.0f, 10000.0f) : 0.0f;
                result.V[0] = ProcStudio::FractalNoise(params, x, y, w, tileable);
                return result;
            }
            case Kind::ConvertToNormalHeight:
            case Kind::ConvertToHeight:
            case Kind::ConvertToNormal: {
                // 色画像 → 高さ / 法線（格納値で計算。スタジオの「画像から高さ・法線」と同じ式）
                const Value uv = Promote(Input(node, 0, depth), 2);
                const float contrast = Input(node, 1, depth).V[0];
                const float bias = Input(node, 2, depth).V[0];
                const int channel = std::clamp(static_cast<int>(node.Value[1]), 0, GraphMath::kConvertChannelCount - 1);
                const bool invert = node.Value[2] > 0.5f;
                const bool normal = node.Kind == Kind::ConvertToNormal ||
                                    (node.Kind == Kind::ConvertToNormalHeight && node.Value[0] > 0.5f);
                const auto tap = [&](float du, float dv, float& height) {
                    float texel[4]{};
                    if (node.Texture == 0 || !m_Sampler || !m_Sampler(node.Texture, uv.V[0] + du, uv.V[1] + dv, true, texel))
                    {
                        return false;
                    }
                    height = GraphMath::ConvertHeight(texel, channel, invert, contrast, bias);
                    return true;
                };
                if (!normal)
                {
                    float h = 0.5f;
                    tap(0.0f, 0.0f, h);
                    result.V[0] = h;
                    return result;
                }
                const float strength = Input(node, 3, depth).V[0];
                const float r = (std::max)(Input(node, 4, depth).V[0], 1e-5f);
                float l = 0, rr = 0, u = 0, d = 0;
                if (!tap(-r, 0, l) || !tap(r, 0, rr) || !tap(0, -r, u) || !tap(0, r, d))
                {
                    return Make3(0.0f, 0.0f, 1.0f); // 読み込み中 / 未設定は平らな法線
                }
                float dx = rr - l;
                float dy = d - u;
                if (node.Value[3] > 0.5f) // Sobel（中央差分と同じスケール）
                {
                    float lu = 0, ru = 0, ld = 0, rd = 0;
                    tap(-r, -r, lu);
                    tap(r, -r, ru);
                    tap(-r, r, ld);
                    tap(r, r, rd);
                    dx = ((ru - lu) + dx * 2.0f + (rd - ld)) * 0.25f;
                    dy = ((ld - lu) + dy * 2.0f + (rd - ru)) * 0.25f;
                }
                float n[3] = {-dx * strength, -dy * strength, 1.0f};
                const float up[3] = {0.0f, 0.0f, 1.0f};
                Normalize3(n, up);
                return Make(n, 3);
            }
            case Kind::NoiseCombine: {
                const float a = Input(node, 0, depth).V[0];
                const float b = Input(node, 1, depth).V[0];
                const float factor = Input(node, 2, depth).V[0];
                result.V[0] = GraphMath::NoiseCombine(static_cast<int>(node.Value[0]), a, b, factor, node.Value[1] > 0.5f);
                return result;
            }
            case Kind::NoiseLayers: {
                // 繋がっている層だけを、重み（0 以下は無視）で平均する（スタジオの Composite と同じ）
                float total = 0.0f;
                float weightSum = 0.0f;
                for (size_t layer = 0; layer < 4; ++layer)
                {
                    const size_t pin = layer * 2;
                    if (pin + 1 >= node.InputPins.size() || !m_Graph.InputLink(node.InputPins[pin]))
                    {
                        continue;
                    }
                    const float weight = Input(node, pin + 1, depth).V[0];
                    if (!std::isfinite(weight) || weight <= 0.0f)
                    {
                        continue;
                    }
                    total += Input(node, pin, depth).V[0] * weight;
                    weightSum += weight;
                }
                result.V[0] = weightSum > 0.0f ? total / weightSum : 0.0f;
                return result;
            }
            case Kind::Voronoi: {
                const Value uv = Promote(Input(node, 0, depth), 2);
                const float scale = Input(node, 1, depth).V[0];
                const float jitter = Input(node, 2, depth).V[0];
                result.V[0] = GraphMath::Voronoi(uv.V[0] * scale, uv.V[1] * scale, jitter, node.Value[0] > 0.5f);
                return result;
            }
            case Kind::NormalFromHeight: {
                // エンジンの ApplyAssetHeightNormal と同じ向き：(-dh/du, -dh/dv, 1) * 強さ
                const float step = (std::max)(m_Context.PixelSize, 1e-5f);
                const float h = Input(node, 0, depth).V[0];
                const float strength = Input(node, 1, depth).V[0];
                const float hu = Shifted(node, 0, 0, depth).V[0];
                const float hv = Shifted(node, 0, 1, depth).V[0];
                float n[3] = {-(hu - h) / step * strength, -(hv - h) / step * strength, 1.0f};
                const float up[3] = {0.0f, 0.0f, 1.0f};
                Normalize3(n, up);
                return Make(n, 3);
            }
            case Kind::BlendAngleCorrectedNormals: {
                const Value base = Promote(Input(node, 0, depth), 3);
                const Value detail = Promote(Input(node, 1, depth), 3);
                const float t[3] = {base.V[0], base.V[1], base.V[2] + 1.0f};
                const float u[3] = {-detail.V[0], -detail.V[1], detail.V[2]};
                const float k = Dot3(t, u) / (std::max)(t[2], 1e-5f);
                float n[3] = {t[0] * k - u[0], t[1] * k - u[1], t[2] * k - u[2]};
                const float up[3] = {0.0f, 0.0f, 1.0f};
                Normalize3(n, up);
                return Make(n, 3);
            }
            case Kind::FlattenNormal: {
                const Value normal = Promote(Input(node, 0, depth), 3);
                const float flatness = Input(node, 1, depth).V[0];
                float n[3] = {normal.V[0] * (1.0f - flatness), normal.V[1] * (1.0f - flatness),
                              normal.V[2] + (1.0f - normal.V[2]) * flatness};
                const float up[3] = {0.0f, 0.0f, 1.0f};
                Normalize3(n, up);
                return Make(n, 3);
            }
            case Kind::BumpOffset: {
                const float height = Input(node, 0, depth).V[0];
                const float ratio = Input(node, 1, depth).V[0];
                const float reference = Input(node, 2, depth).V[0];
                const Value coordinate = Promote(Input(node, 3, depth), 2);
                // カメラ方向を接空間へ（ベイクでは正面から見るのでずれは 0）
                const float cu = Dot3(m_Context.Camera, m_Context.Tangent);
                const float cv = Dot3(m_Context.Camera, m_Context.Bitangent);
                const float offset = (height - reference) * ratio;
                const float uv[2] = {coordinate.V[0] + cu * offset, coordinate.V[1] + cv * offset};
                return Make(uv, 2);
            }
            case Kind::RotateAboutAxis: {
                Value axis = Promote(Input(node, 0, depth), 3);
                const float up[3] = {0.0f, 0.0f, 1.0f};
                Normalize3(axis.V, up);
                const float angle = Input(node, 1, depth).V[0] * 2.0f * kPi;
                const Value pivot = Promote(Input(node, 2, depth), 3);
                const Value position = Promote(Input(node, 3, depth), 3);
                const float d[3] = {position.V[0] - pivot.V[0], position.V[1] - pivot.V[1], position.V[2] - pivot.V[2]};
                const float along = Dot3(axis.V, d);
                const float closest[3] = {pivot.V[0] + axis.V[0] * along, pivot.V[1] + axis.V[1] * along,
                                          pivot.V[2] + axis.V[2] * along};
                const float u[3] = {position.V[0] - closest[0], position.V[1] - closest[1], position.V[2] - closest[2]};
                const float v[3] = {axis.V[1] * u[2] - axis.V[2] * u[1], axis.V[2] * u[0] - axis.V[0] * u[2],
                                    axis.V[0] * u[1] - axis.V[1] * u[0]};
                const float c = cosf(angle), s = sinf(angle);
                return Make3(closest[0] + u[0] * c + v[0] * s - position.V[0],
                             closest[1] + u[1] * c + v[1] * s - position.V[1],
                             closest[2] + u[2] * c + v[2] * s - position.V[2]);
            }
            case Kind::BlackBody: {
                result.N = 3;
                GraphMath::BlackBody(Input(node, 0, depth).V[0], result.V);
                return result;
            }
            default:
                return result;
        }
    }
} // namespace MaterialGraphEval



