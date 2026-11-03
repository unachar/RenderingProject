#include "pch.h"
#include "materialgraphevaluator.h"
#include "materialgraphmath.h"
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
                if (const auto* source = m_Graph.FindPinNode(link->SourcePin))
                {
                    const auto found = m_Topology.Index.find(source->Id);
                    if (found != m_Topology.Index.end())
                    {
                        return Node(found->second, depth + 1);
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
            for (int i = 0; i < 4; ++i)
            {
                value.V[i] = pin.Default[i];
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
                return Input(node, 0, depth);
            case Kind::TextureSample: {
                const Value uv = Promote(Input(node, 0, depth), 2);
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
