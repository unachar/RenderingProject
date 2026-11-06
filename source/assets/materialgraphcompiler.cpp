#include "pch.h"
#include "materialgraphcompiler.h"
#include "materialgraphmath.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <filesystem>
#include <map>

namespace
{
    using Type = MaterialGraphType;
    using Kind = MaterialNodeKind;
    using Float4 = std::array<float, 4>;

    struct Expression
    {
        std::string Code;
        Type ValueType = Type::Scalar;
        std::optional<Float4> Constant;
    };

    std::string Float(float value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(9) << std::scientific << value << 'f';
        return stream.str();
    }

    std::string HlslType(Type type)
    {
        if (type == Type::Scalar || type == Type::Numeric)
        {
            return "float";
        }
        return "float" + std::to_string(static_cast<int>(type));
    }

    Expression Literal(const Float4& value, Type type)
    {
        if (type == Type::Numeric)
        {
            type = Type::Scalar;
        }
        Expression expression;
        expression.Constant = value;
        expression.ValueType = type;
        if (type == Type::Scalar)
        {
            expression.Code = Float(value[0]);
            return expression;
        }
        expression.Code = HlslType(type) + "(";
        for (int i = 0; i < static_cast<int>(type); ++i)
        {
            if (i != 0)
            {
                expression.Code += ",";
            }
            expression.Code += Float(value[i]);
        }
        expression.Code += ")";
        return expression;
    }

    Expression Promote(Expression value, Type target)
    {
        if (value.ValueType == target || target == Type::Numeric)
        {
            return value;
        }
        if (value.ValueType == Type::Vector3 && target == Type::Vector4)
        {
            // float3 → float4 はアルファ 1（Base Color に色を繋いだとき）
            if (value.Constant)
            {
                auto constant = *value.Constant;
                constant[3] = 1.0f;
                return Literal(constant, target);
            }
            value.Code = "float4(" + value.Code + ",1)";
            value.ValueType = target;
            return value;
        }
        if (value.Constant)
        {
            if (value.ValueType == Type::Scalar)
            {
                const auto scalar = (*value.Constant)[0];
                value = Literal({scalar, scalar, scalar, scalar}, target);
            }
            else
            {
                value = Literal(*value.Constant, target);
            }
        }
        else if (static_cast<int>(value.ValueType) > static_cast<int>(target))
        {
            const std::string components = "xyzw";
            value.Code = "(" + value.Code + ")." + components.substr(0, static_cast<size_t>(target));
            value.ValueType = target;
        }
        else
        {
            if (value.ValueType == Type::Scalar)
            {
                value.Code = "(" + value.Code + ")." + std::string(static_cast<size_t>(target), 'x');
            }
            else
            {
                std::string code = HlslType(target) + "(" + value.Code;
                for (int component = static_cast<int>(value.ValueType); component < static_cast<int>(target);
                     ++component)
                {
                    code += ",0";
                }
                value.Code = code + ")";
            }
            value.ValueType = target;
        }
        return value;
    }

    class Generator
    {
      public:
        explicit Generator(const MaterialGraph& graph, bool runtime = false) : m_Graph(graph), m_Runtime(runtime) {}

        MaterialGraphProgram Run(uint64_t target)
        {
            if (!m_Graph.Validate(m_Program.Error))
            {
                return m_Program;
            }
            const MaterialGraphNode* node = nullptr;
            if (target != 0)
            {
                node = m_Graph.FindNode(target);
            }
            else
            {
                for (const auto& item : m_Graph.Nodes)
                {
                    if (item.Kind == Kind::Output)
                    {
                        node = &item;
                        break;
                    }
                }
            }
            if (!node || node->Kind == Kind::Group)
            {
                m_Program.Error = "Select a value node or Material Output to compile.";
                return m_Program;
            }
            m_Program.MaterialOutput = node->Kind == Kind::Output;
            std::string result;
            if (m_Program.MaterialOutput)
            {
                const char* fields[] = {"BaseColor",
                                        "Metallic",
                                        "Roughness",
                                        "Normal",
                                        "Emissive",
                                        "Opacity",
                                        "OpacityMask",
                                        "AO",
                                        "Height",
                                        "NormalIntensity",
                                        "ParallaxDepth",
                                        "ParallaxSteps",
                                        "IOR",
                                        "Transmission",
                                        "TransmissionRoughness",
                                        "Thickness",
                                        "Absorption",
                                        "ShadingStrength"};
                static_assert(sizeof(fields) / sizeof(fields[0]) == static_cast<size_t>(MaterialOutputPin::Count));
                const auto source = "material_node_" + std::to_string(node->Id);
                result = "#line 1 \"" + source + "\"\nGeneratedMaterial result;\n";
                m_Program.Sources.push_back({node->Id, source, 1});
                for (size_t i = 0; i < node->InputPins.size() && i < static_cast<size_t>(MaterialOutputPin::Count); ++i)
                {
                    const auto input = Promote(Input(*node, i), MaterialGraph::Definition(node->Kind).Inputs[i].Type);
                    result += "result." + std::string(fields[i]) + " = " + input.Code + ";\n";
                }
                result += "return result;\n";
                m_Program.DependencyOrder.push_back(node->Id);
            }
            else
            {
                const auto expression = Visit(*node);
                if (expression.ValueType == Type::Scalar)
                {
                    result =
                        "return float4(" + expression.Code + "," + expression.Code + "," + expression.Code + ",1);\n";
                }
                else if (expression.ValueType == Type::Vector2)
                {
                    result = "return float4(" + expression.Code + ",0,1);\n";
                }
                else if (expression.ValueType == Type::Vector3)
                {
                    result = "return float4(" + expression.Code + ",1);\n";
                }
                else
                {
                    result = "return " + expression.Code + ";\n";
                }
            }
            if (!m_Program.Error.empty())
            {
                return m_Program;
            }
            std::string header =
                "// Generated by RenderingProject MaterialGraphCompiler. Parameter values are uploaded separately.\n"
                "struct MaterialGraphContext { float2 UV; float3 WorldPosition; float3 WorldNormal; float3 CameraVector; float Time; float3 Tangent; float3 Bitangent;";
            if (m_Runtime)
            {
                header += " uint ParameterTexture;";
            }
            header +=
                " };\n"
                "struct GeneratedMaterial { float4 BaseColor; float Metallic; float Roughness; float3 Normal; float3 Emissive; float Opacity; float OpacityMask; float AO;\n"
                "  float Height; float NormalIntensity; float ParallaxDepth; float ParallaxSteps; float IOR; float Transmission; float TransmissionRoughness;\n"
                "  float Thickness; float3 Absorption; float ShadingStrength; };\n" +
                std::string(MaterialGraphMath::HlslHelpers());
            if (m_UsesProceduralNoise)
            {
                // プロシージャルスタジオと同じ HLSL ノイズ（shader/hlsl/noise.hlsl：バインドなしのライブラリ）。
                // 生成シェーダーは EditorCache に書かれるので、絶対パスで取り込む
                std::error_code pathError;
                const auto library = std::filesystem::absolute("shader/hlsl/noise.hlsl", pathError);
                if (pathError || !std::filesystem::exists(library, pathError))
                {
                    m_Program.Error =
                        "shader/hlsl/noise.hlsl が見つかりません（Procedural Noise / Fractal Noise に必要）";
                    return m_Program;
                }
                header += "#include \"" + library.generic_string() + "\"\n";
                header +=
                    "float MG_ProcNoise(float3 p, uint basis, uint fractal, uint seed, uint tileable, float frequency, float octaves,\n"
                    "  float lacunarity, float gain, float warp, float ridge, float direction, float anisotropy, float kernelFrequency, float phase) {\n"
                    "  TextureNoise::NoiseSettings s; s.Frequency = max(frequency, 0.001); s.Octaves = clamp((int)octaves, 1, 12);\n"
                    "  s.Lacunarity = lacunarity; s.Gain = gain; s.WarpStrength = warp; s.RidgeOffset = ridge; s.Tileable = tileable;\n"
                    "  s.Direction = direction; s.Anisotropy = clamp(anisotropy, 1, 8); s.KernelFrequency = kernelFrequency; s.Phase = phase;\n"
                    "  p = clamp(p, -10000, 10000);\n"
                    "  return TextureNoise::Noise(p, basis, fractal, seed, s); }\n";
                // Convert To Solid（ストキャスティック：3 方向投影 × ヘックス タイル。MGVM_SolidStochastic と同じ）
                header +=
                    "uint MG_TileHash(int2 v, uint seed) { return TextureNoise::Hash(seed ^ TextureNoise::Hash((uint)v.x * 0x8da6b343u ^\n"
                    "  TextureNoise::Hash((uint)v.y * 0xd8163841u))); }\n"
                    "float4 MG_HexSample(Texture2D<float4> t, SamplerState s, float2 uv, uint seed, float randomness) {\n"
                    "  float2 st = uv * 3.4641016; float2 k = float2(st.x, -0.57735027 * st.x + 1.15470054 * st.y); int2 b = (int2)floor(k);\n"
                    "  float3 f = float3(frac(k), 0); f.z = 1 - f.x - f.y; float sg = f.z < 0 ? 1 : 0; float s2 = 2 * sg - 1; int i = (int)sg;\n"
                    "  float3 w = pow(max(float3(-f.z * s2, sg - f.y * s2, sg - f.x * s2), 0), 4); w /= max(dot(w, 1), 1e-6);\n"
                    "  int2 v[3] = {b + int2(i, i), b + int2(i, 1 - i), b + int2(1 - i, i)}; float4 r = 0; float2 dx = ddx(uv), dy = ddy(uv);\n"
                    "  [unroll] for (int n = 0; n < 3; ++n) { uint h = MG_TileHash(v[n], seed);\n"
                    "    float a = TextureNoise::Unit(TextureNoise::Hash(h ^ 0x68bc21ebu)) * 6.28318531 * randomness; float c = cos(a), sn = sin(a);\n"
                    "    float2x2 m = float2x2(c, -sn, sn, c); float2 q = mul(m, uv) + float2(TextureNoise::Unit(h), TextureNoise::Unit(TextureNoise::Hash(h)));\n"
                    "    r += t.SampleGrad(s, q, mul(m, dx), mul(m, dy)) * w[n]; }\n"
                    "  return r; }\n"
                    "float4 MG_SolidStochastic(Texture2D<float4> t, SamplerState s, float3 p, float3 n, float sharpness, float randomness, uint seed) {\n"
                    "  float3 w = pow(abs(n) + 1e-5, max(sharpness, 0.01)); w /= dot(w, 1);\n"
                    "  return MG_HexSample(t, s, p.yz, TextureNoise::Hash(seed), randomness) * w.x +\n"
                    "         MG_HexSample(t, s, p.xz, TextureNoise::Hash(seed + 0x9E3779B9u), randomness) * w.y +\n"
                    "         MG_HexSample(t, s, p.xy, TextureNoise::Hash(seed + 0x3C6EF372u), randomness) * w.z; }\n";
                // Convert To Solid（ProcStudio::VolumeFromImage と同じ重ね方。勾配はループの外で求める）
                header +=
                    "float4 MG_Gray(float4 c) { float g = (c.r + c.g + c.b) / 3; return float4(g, g, g, c.a); }\n"
                    "float4 MG_SolidVolume(Texture2D<float4> t, SamplerState s, float3 p, float octavesIn, float variation, uint seed) {\n"
                    "  int octaves = clamp((int)octavesIn, 1, 6); float2 dA = ddx(p.xz), eA = ddy(p.xz); float2 dB = ddx(p.zy), eB = ddy(p.zy);\n"
                    "  float4 sum = 0; float norm = 0; float amplitude = 1;\n"
                    "  [unroll] for (int o = 0; o < 6; ++o) { if (o >= octaves) break; float scale = (float)(1 << o);\n"
                    "    uint h = TextureNoise::Hash(seed + (uint)o * 977u); float angle = p.y * 6.28318531;\n"
                    "    float swing = round(variation * 2) * 0.5 / scale;\n"
                    "    float ox = swing * cos(angle) + TextureNoise::Unit(h); float oz = swing * sin(angle) + TextureNoise::Unit(TextureNoise::Hash(h));\n"
                    "    float4 a = t.SampleGrad(s, float2(p.x * scale + ox, p.z * scale + oz), dA * scale, eA * scale);\n"
                    "    float4 b = t.SampleGrad(s, float2(p.z * scale - oz + 0.37, p.y * scale + ox), dB * scale, eB * scale);\n"
                    "    sum += (a * 0.7 + b * 0.3) * amplitude; norm += amplitude; amplitude *= 0.5; }\n"
                    "  return sum / norm; }\n";
                // ソリッドテクスチャ（ProcStudio::TextureScalar / TextureColor の solid = true と同じ式）
                header +=
                    "float MG_SolidScalar(float3 p, int kind, uint basis, uint fractal, uint seed, float frequency, float octaves,\n"
                    "  float lacunarity, float gain, float warp, float ridge, float turbulence, float rings, float coverage, float softness,\n"
                    "  float contrast, float bias) {\n"
                    "  TextureNoise::NoiseSettings s; s.Frequency = max(frequency, 0.001); s.Octaves = clamp((int)octaves, 1, 12);\n"
                    "  s.Lacunarity = lacunarity; s.Gain = gain; s.WarpStrength = warp; s.RidgeOffset = ridge; s.Tileable = 0;\n"
                    "  s.Direction = 0; s.Anisotropy = 4; s.KernelFrequency = 2; s.Phase = 0;\n"
                    "  p = clamp(p, -10000, 10000); float v;\n"
                    "  if (kind == 1) { float n = TextureNoise::Noise(p, basis, fractal, seed, s);\n"
                    "    v = 0.5 + 0.5 * sin((p.x * round(rings) + n * turbulence) * 6.28318531); v = pow(max(v, 0), 0.6); }\n"
                    "  else if (kind == 2) { float n = TextureNoise::Noise(p, basis, fractal, seed, s); float cx = p.x - 0.5; float cz = p.z - 0.5;\n"
                    "    float r = sqrt(cx * cx + cz * cz) * rings * 2 + n * turbulence * 0.5; v = r - floor(r);\n"
                    "    v = TextureNoise::Smooth(0, 0.8, v) * (1 - TextureNoise::Smooth(0.85, 1, v)); }\n"
                    "  else if (kind == 3) { float cells = TextureNoise::Noise(p, 2u, 0u, seed, s);\n"
                    "    float grain = TextureNoise::Noise(p * 4, basis, fractal, seed, s); v = saturate(cells * 0.6 + grain * 0.4);\n"
                    "    v = v > 0.62 ? 1 : (v < 0.3 ? 0 : v); }\n"
                    "  else if (kind == 4) { float2 f = TextureNoise::Worley(p * frequency, seed, 0); v = saturate((f.y - f.x) * 3); }\n"
                    "  else if (kind == 5) { float base = TextureNoise::Noise(p, basis, 0u, seed, s);\n"
                    "    float billows = 1 - TextureNoise::Noise(p, 2u, 0u, seed ^ 0x7f4a7c15u, s); float shape = base * 0.65 + billows * 0.35;\n"
                    "    float threshold = 1 - coverage; v = TextureNoise::Smooth(threshold - softness * 0.5, threshold + softness * 0.5, shape); }\n"
                    "  else { v = TextureNoise::Noise(p, basis, fractal, seed, s); }\n"
                    "  return saturate((v - 0.5) * contrast + 0.5 + bias); }\n"
                    "float3 MG_SolidColor(float v, int kind, float3 a, float3 b, float3 c) {\n"
                    "  if (kind == 5) return lerp(float3(0.32, 0.52, 0.86), lerp(b, 1.0.xxx, 0.5), v);\n"
                    "  return v < 0.5 ? lerp(a, b, v * 2) : lerp(b, c, (v - 0.5) * 2); }\n";
            }
            if (m_Runtime)
            {
                header +=
                    "float4 MG_Parameter(MaterialGraphContext context, uint index) { return LoadMaterialTexture(context.ParameterTexture).Load(int3(index,0,0)); }\n";
                header +=
                    "Texture2D<float4> MG_GraphTexture(MaterialGraphContext context, uint index) { return LoadMaterialTexture(asuint(MG_Parameter(context," +
                    std::to_string(m_Program.Parameters.size()) + "u+index).x)); }\n";
            }
            else if (!m_Program.Parameters.empty())
            {
                header += "cbuffer MaterialGraphParameters : register(b0) { float4 GraphParameters[" +
                          std::to_string(m_Program.Parameters.size()) + "]; };\n";
            }
            if (!m_Runtime && !m_Program.Textures.empty())
            {
                header += "Texture2D<float4> GraphTextures[" + std::to_string(m_Program.Textures.size()) +
                          "] : register(t0);\nSamplerState GraphSampler : register(s0);\n";
            }
            const std::string arguments = m_Runtime ? ", SamplerState GraphSampler" : "";
            if (m_Program.MaterialOutput)
            {
                header += "GeneratedMaterial EvaluateMaterialGraph(MaterialGraphContext context" + arguments + ") {\n";
            }
            else
            {
                header += "float4 EvaluateMaterialNode(MaterialGraphContext context" + arguments + ") {\n";
            }
            m_Program.Hlsl = header + m_Body + result + "}\n";
            uint64_t hash = 14695981039346656037ull;
            for (const unsigned char value : m_Program.Hlsl)
            {
                hash = (hash ^ value) * 1099511628211ull;
            }
            m_Program.ShaderHash = hash;
            m_Program.Success = true;
            return m_Program;
        }

      private:
        Expression Input(const MaterialGraphNode& node, size_t index)
        {
            if (const auto* link = m_Graph.InputLink(node.InputPins[index]))
            {
                size_t outputIndex = 0;
                if (const auto* source = m_Graph.FindPinNode(link->SourcePin, nullptr, &outputIndex))
                {
                    auto expression = Visit(*source);
                    if (outputIndex > 0)
                    {
                        expression = Component(std::move(expression), static_cast<int>(outputIndex) - 1);
                    }
                    return expression;
                }
            }
            switch (MaterialGraphMath::DefaultInput(node.Kind, index))
            {
                case MaterialGraphMath::SpecialInput::UV:
                    return {"context.UV", Type::Vector2, {}};
                case MaterialGraphMath::SpecialInput::Time:
                    m_Program.Animated = true;
                    return {"context.Time", Type::Scalar, {}};
                case MaterialGraphMath::SpecialInput::WorldPosition:
                    return {"context.WorldPosition", Type::Vector3, {}};
                default:
                    break;
            }
            const auto& input = MaterialGraph::Definition(node.Kind).Inputs[index];
            return Literal(MaterialGraph::UnconnectedInputValue(node, index), input.Type); // ピン横のボックスの値
        }

        // 分割した出力（X / Y / Z / W）の成分。無い成分は 0、スカラーは全成分同じ
        Expression Component(Expression value, int component)
        {
            const int count = value.ValueType == Type::Scalar || value.ValueType == Type::Numeric
                                  ? 1
                                  : static_cast<int>(value.ValueType);
            if (value.Constant)
            {
                const auto& constant = *value.Constant;
                const float scalar = count == 1 ? constant[0] : component < count ? constant[component] : 0.0f;
                return Literal({scalar, scalar, scalar, scalar}, Type::Scalar);
            }
            if (count == 1)
            {
                value.ValueType = Type::Scalar;
                return value;
            }
            if (component >= count)
            {
                return Literal({}, Type::Scalar);
            }
            static const char* swizzle[] = {"x", "y", "z", "w"};
            return {"(" + value.Code + ")." + swizzle[component], Type::Scalar, {}};
        }

        Expression Arithmetic(const MaterialGraphNode& node)
        {
            std::vector<Expression> inputs;
            Type type = Type::Scalar;
            for (size_t i = 0; i < node.InputPins.size(); ++i)
            {
                inputs.push_back(Input(node, i));
                type = static_cast<Type>((std::max)(static_cast<int>(type), static_cast<int>(inputs.back().ValueType)));
            }
            bool constant = true;
            for (auto& input : inputs)
            {
                input = Promote(std::move(input), type);
                constant &= input.Constant.has_value();
            }
            const auto& a = inputs[0];
            const auto& b = inputs[1];
            std::string code;
            switch (node.Kind)
            {
                case Kind::Add:
                    code = "(" + a.Code + "+" + b.Code + ")";
                    break;
                case Kind::Subtract:
                    code = "(" + a.Code + "-" + b.Code + ")";
                    break;
                case Kind::Multiply:
                    code = "(" + a.Code + "*" + b.Code + ")";
                    break;
                case Kind::Divide:
                    code = "(" + a.Code + "/" + b.Code + ")";
                    break;
                case Kind::Lerp:
                    code = "lerp(" + a.Code + "," + b.Code + "," + inputs[2].Code + ")";
                    break;
                case Kind::Clamp:
                    code = "clamp(" + a.Code + "," + b.Code + "," + inputs[2].Code + ")";
                    break;
                case Kind::Smoothstep:
                    code = "smoothstep(" + a.Code + "," + b.Code + "," + inputs[2].Code + ")";
                    break;
                default:
                    break;
            }
            if (constant)
            {
                Float4 folded{};
                for (int i = 0; i < static_cast<int>(type); ++i)
                {
                    const float x = (*a.Constant)[i];
                    const float y = (*b.Constant)[i];
                    const float z = inputs.size() > 2 ? (*inputs[2].Constant)[i] : 0;
                    switch (node.Kind)
                    {
                        case Kind::Add:
                            folded[i] = x + y;
                            break;
                        case Kind::Subtract:
                            folded[i] = x - y;
                            break;
                        case Kind::Multiply:
                            folded[i] = x * y;
                            break;
                        case Kind::Divide:
                            if (y == 0)
                            {
                                return {code, type, {}};
                            }
                            folded[i] = x / y;
                            break;
                        case Kind::Lerp:
                            folded[i] = x + (y - x) * z;
                            break;
                        case Kind::Clamp:
                            folded[i] = (std::min)((std::max)(x, y), z);
                            break;
                        case Kind::Smoothstep: {
                            if (x == y)
                            {
                                return {code, type, {}};
                            }
                            const float t = std::clamp((z - x) / (y - x), 0.0f, 1.0f);
                            folded[i] = t * t * (3.0f - 2.0f * t);
                            break;
                        }
                        default:
                            break;
                    }
                    if (!std::isfinite(folded[i]))
                    {
                        return {code, type, {}};
                    }
                }
                return Literal(folded, type);
            }
            return {code, type, {}};
        }

        // 単項 / 二項 / ConstantBiasScale（入力の最大次元に揃え、すべて定数なら畳み込む）
        Expression ExtendedMath(const MaterialGraphNode& node)
        {
            std::vector<Expression> inputs;
            Type type = Type::Scalar;
            for (size_t i = 0; i < node.InputPins.size(); ++i)
            {
                inputs.push_back(Input(node, i));
                type = static_cast<Type>((std::max)(static_cast<int>(type), static_cast<int>(inputs.back().ValueType)));
            }
            const bool biasScale = node.Kind == Kind::ConstantBiasScale;
            bool constant = node.Kind != Kind::DDX && node.Kind != Kind::DDY;
            for (size_t i = 0; i < inputs.size(); ++i)
            {
                // Power の指数 / Bias / Scale はスカラーのまま使う
                if (!(biasScale && i > 0) && !(node.Kind == Kind::Power && i == 1))
                {
                    inputs[i] = Promote(std::move(inputs[i]), type);
                }
                constant &= inputs[i].Constant.has_value();
            }
            std::string code;
            if (MaterialGraphMath::IsUnary(node.Kind))
            {
                code = MaterialGraphMath::UnaryHlsl(node.Kind, inputs[0].Code);
            }
            else if (biasScale)
            {
                code = "((" + inputs[0].Code + "+" + inputs[1].Code + ")*" + inputs[2].Code + ")";
            }
            else
            {
                code = MaterialGraphMath::BinaryHlsl(node.Kind, inputs[0].Code, inputs[1].Code);
            }
            if (constant)
            {
                Float4 folded{};
                for (int i = 0; i < static_cast<int>(type); ++i)
                {
                    const float x = (*inputs[0].Constant)[i];
                    float value = 0.0f;
                    if (MaterialGraphMath::IsUnary(node.Kind))
                    {
                        value = MaterialGraphMath::Unary(node.Kind, x);
                    }
                    else if (biasScale)
                    {
                        value = (x + (*inputs[1].Constant)[0]) * (*inputs[2].Constant)[0];
                    }
                    else
                    {
                        const float y = node.Kind == Kind::Power ? (*inputs[1].Constant)[0] : (*inputs[1].Constant)[i];
                        value = MaterialGraphMath::Binary(node.Kind, x, y);
                    }
                    if (!std::isfinite(value))
                    {
                        return {code, type, {}};
                    }
                    folded[i] = value;
                }
                return Literal(folded, type);
            }
            return {code, type, {}};
        }

        Expression Visit(const MaterialGraphNode& node)
        {
            if (const auto found = m_Expressions.find(node.Id); found != m_Expressions.end())
            {
                return found->second;
            }
            Expression expression;
            if (node.Kind == Kind::Reroute || node.Kind == Kind::BreakVector)
            {
                // 中継ノードは式をそのまま通す
                expression = Input(node, 0);
                m_Expressions[node.Id] = expression;
                return expression;
            }
            if (MaterialGraphMath::IsConstantNode(node.Kind))
            {
                const auto index = m_Program.Parameters.size();
                m_Program.Parameters.push_back(node.Value);
                m_Program.ParameterNodes.push_back(node.Id);
                if (m_Runtime)
                {
                    expression.Code = "MG_Parameter(context," + std::to_string(index) + "u)";
                }
                else
                {
                    expression.Code = "GraphParameters[" + std::to_string(index) + "]";
                }
                expression.ValueType = MaterialGraph::Definition(node.Kind).OutputType;
                if (m_Graph.HasConnectedComponent(node))
                {
                    // 成分ピン：接続した成分は入力（スカラー）、未接続の成分はパラメーター（値の変更で再コンパイル不要）
                    const std::string parameter = expression.Code;
                    const int count = static_cast<int>(expression.ValueType);
                    std::string code = count > 1 ? HlslType(expression.ValueType) + "(" : "(";
                    for (int i = 0; i < count && i < static_cast<int>(node.InputPins.size()); ++i)
                    {
                        if (i != 0)
                        {
                            code += ",";
                        }
                        if (m_Graph.InputLink(node.InputPins[static_cast<size_t>(i)]))
                        {
                            auto component = Input(node, static_cast<size_t>(i));
                            if (component.ValueType == Type::Numeric)
                            {
                                component.ValueType = Type::Scalar;
                            }
                            code += Promote(std::move(component), Type::Scalar).Code;
                        }
                        else
                        {
                            code += parameter + "." + "xyzw"[i];
                        }
                    }
                    expression.Code = code + ")";
                }
                else if (expression.ValueType == Type::Scalar)
                {
                    expression.Code += ".x";
                }
                else if (expression.ValueType == Type::Vector3)
                {
                    expression.Code += ".xyz";
                }
                else if (expression.ValueType == Type::Vector2)
                {
                    expression.Code += ".xy";
                }
            }
            else if (MaterialGraphMath::IsUnary(node.Kind) || MaterialGraphMath::IsBinary(node.Kind) ||
                     node.Kind == Kind::ConstantBiasScale)
            {
                expression = ExtendedMath(node);
            }
            else if (node.Kind == Kind::TextureCoordinate)
            {
                expression = {"(context.UV*float2(" + Float(node.Value[0]) + "," + Float(node.Value[1]) + "))",
                              Type::Vector2,
                              {}};
            }
            else if (node.Kind == Kind::Panner)
            {
                const auto coordinate = Promote(Input(node, 0), Type::Vector2);
                const auto time = Promote(Input(node, 1), Type::Scalar);
                const auto speed = Promote(Input(node, 2), Type::Vector2);
                expression = {"(" + coordinate.Code + "+" + speed.Code + "*" + time.Code + ")", Type::Vector2, {}};
            }
            else if (node.Kind == Kind::Rotator)
            {
                const auto coordinate = Promote(Input(node, 0), Type::Vector2);
                const auto time = Promote(Input(node, 1), Type::Scalar);
                const auto speed = Promote(Input(node, 2), Type::Scalar);
                const auto center = Promote(Input(node, 3), Type::Vector2);
                expression = {"MG_Rotate(" + coordinate.Code + "," + center.Code + "," + time.Code + "*" + speed.Code +
                                  ")",
                              Type::Vector2,
                              {}};
            }
            else if (node.Kind == Kind::CrossProduct)
            {
                const auto a = Promote(Input(node, 0), Type::Vector3);
                const auto b = Promote(Input(node, 1), Type::Vector3);
                expression = {"cross(" + a.Code + "," + b.Code + ")", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::Distance)
            {
                const auto a = Promote(Input(node, 0), Type::Vector3);
                const auto b = Promote(Input(node, 1), Type::Vector3);
                expression = {"distance(" + a.Code + "," + b.Code + ")", Type::Scalar, {}};
            }
            else if (node.Kind == Kind::Length)
            {
                const auto input = Input(node, 0);
                expression = {"length(" + input.Code + ")", Type::Scalar, {}};
            }
            else if (node.Kind == Kind::AppendVector)
            {
                auto a = Input(node, 0);
                auto b = Input(node, 1);
                const auto dims = [](Type type) {
                    return type == Type::Numeric ? 1 : static_cast<int>(type);
                };
                const int count = dims(a.ValueType) + dims(b.ValueType);
                if (count > 4)
                {
                    m_Program.Error = "Append node " + std::to_string(node.Id) + " exceeds 4 components.";
                    expression = Literal({}, Type::Scalar);
                }
                else
                {
                    const auto type = static_cast<Type>(count);
                    expression = {HlslType(type) + "(" + a.Code + "," + b.Code + ")", type, {}};
                }
            }
            else if (node.Kind == Kind::ComponentMask)
            {
                const auto input = Input(node, 0);
                const int available = input.ValueType == Type::Numeric ? 1 : static_cast<int>(input.ValueType);
                std::string swizzle;
                for (int i = 0; i < 4; ++i)
                {
                    if (node.Value[i] > 0.5f)
                    {
                        swizzle += "xyzw"[(std::min)(i, available - 1)];
                    }
                }
                if (swizzle.empty())
                {
                    swizzle = "x";
                }
                std::string code = input.Code;
                if (available == 1)
                {
                    code = "((float)" + code + ")";
                }
                expression = {"(" + code + ")." + swizzle, static_cast<Type>(swizzle.size()), {}};
            }
            else if (node.Kind == Kind::ReflectionVector)
            {
                expression = {
                    "reflect(-normalize(context.CameraVector),normalize(context.WorldNormal))", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::Desaturation)
            {
                const auto color = Promote(Input(node, 0), Type::Vector3);
                const auto fraction = Promote(Input(node, 1), Type::Scalar);
                expression = {"lerp(" + color.Code + ",dot(" + color.Code + ",float3(0.3,0.59,0.11)).xxx," +
                                  fraction.Code + ")",
                              Type::Vector3,
                              {}};
            }
            else if (node.Kind == Kind::If)
            {
                const auto a = Promote(Input(node, 0), Type::Scalar);
                const auto b = Promote(Input(node, 1), Type::Scalar);
                Expression branches[3] = {Input(node, 2), Input(node, 3), Input(node, 4)};
                Type type = Type::Scalar;
                for (const auto& branch : branches)
                {
                    type = static_cast<Type>((std::max)(static_cast<int>(type), static_cast<int>(branch.ValueType)));
                }
                for (auto& branch : branches)
                {
                    branch = Promote(std::move(branch), type);
                }
                expression = {"((" + a.Code + ">" + b.Code + ")?" + branches[0].Code + ":((" + a.Code + "==" + b.Code +
                                  ")?" + branches[1].Code + ":" + branches[2].Code + "))",
                              type,
                              {}};
            }
            else if (node.Kind == Kind::SphereMask)
            {
                const auto a = Promote(Input(node, 0), Type::Vector3);
                const auto b = Promote(Input(node, 1), Type::Vector3);
                const auto radius = Promote(Input(node, 2), Type::Scalar);
                const auto hardness = Promote(Input(node, 3), Type::Scalar);
                expression = {"MG_SphereMask(" + a.Code + "," + b.Code + "," + radius.Code + "," + hardness.Code + ")",
                              Type::Scalar,
                              {}};
            }
            else if (node.Kind == Kind::CheapContrast)
            {
                const auto input = Promote(Input(node, 0), Type::Scalar);
                const auto contrast = Promote(Input(node, 1), Type::Scalar);
                expression = {"saturate(lerp(0-" + contrast.Code + ",1+" + contrast.Code + "," + input.Code + "))",
                              Type::Scalar,
                              {}};
            }
            else if (node.Kind == Kind::Noise)
            {
                const auto position = Promote(Input(node, 0), Type::Vector3);
                const auto scale = Promote(Input(node, 1), Type::Scalar);
                expression = {"MG_Noise(" + position.Code + "*" + scale.Code + ")", Type::Scalar, {}};
            }
            else if (node.Kind >= Kind::Add && node.Kind <= Kind::Smoothstep)
            {
                expression = Arithmetic(node);
            }
            // ---------------------------------------------------------------- 2026-11 追加ノード
            else if (node.Kind == Kind::StaticSwitch)
            {
                // 静的分岐：選ばれた側だけを生成する（UE の Static Switch Parameter と同じ）
                expression = Input(node, node.Value[0] > 0.5f ? 0 : 1);
                m_Expressions[node.Id] = expression;
                return expression;
            }
            else if (node.Kind == Kind::Pi)
            {
                expression = Literal({3.14159265f * node.Value[0]}, Type::Scalar);
            }
            else if (node.Kind == Kind::InverseLerp)
            {
                const auto input = Input(node, 0);
                const auto low = Promote(Input(node, 1), Type::Scalar);
                const auto high = Promote(Input(node, 2), Type::Scalar);
                const auto range = "(" + high.Code + "-" + low.Code + ")";
                expression = {"(abs(" + range + ")>1e-8?(" + input.Code + "-" + low.Code + ")/" + range + ":0)",
                              input.ValueType,
                              {}};
            }
            else if (node.Kind == Kind::ReflectVector)
            {
                expression = {"MG_ReflectVector(" + Promote(Input(node, 0), Type::Vector3).Code + "," +
                                  Promote(Input(node, 1), Type::Vector3).Code + ")",
                              Type::Vector3,
                              {}};
            }
            else if (node.Kind == Kind::RenderSoftBand || node.Kind == Kind::RenderRadialPulse ||
                     node.Kind == Kind::RenderFlowUV)
            {
                const auto type = node.Kind == Kind::RenderSoftBand      ? Type::Scalar
                                  : node.Kind == Kind::RenderRadialPulse ? Type::Vector3
                                                                         : Type::Vector2;
                const auto secondType = node.Kind == Kind::RenderFlowUV ? Type::Vector2 : Type::Scalar;
                const std::string function = node.Kind == Kind::RenderSoftBand      ? "MG_RenderSoftBand"
                                             : node.Kind == Kind::RenderRadialPulse ? "MG_RenderRadialPulse"
                                                                                    : "MG_RenderFlowUV";
                expression = {function + "(" + Promote(Input(node, 0), type).Code + "," +
                                  Promote(Input(node, 1), secondType).Code + "," +
                                  Promote(Input(node, 2), Type::Scalar).Code + "," +
                                  Promote(Input(node, 3), Type::Scalar).Code + ")",
                              node.Kind == Kind::RenderFlowUV ? Type::Vector2 : Type::Scalar,
                              {}};
            }
            else if (node.Kind == Kind::RemapValueRange)
            {
                const auto input = Input(node, 0);
                const auto inLow = Promote(Input(node, 1), Type::Scalar);
                const auto inHigh = Promote(Input(node, 2), Type::Scalar);
                const auto outLow = Promote(Input(node, 3), Type::Scalar);
                const auto outHigh = Promote(Input(node, 4), Type::Scalar);
                expression = {"(" + outLow.Code + "+(" + outHigh.Code + "-" + outLow.Code + ")*((" + input.Code + "-" +
                                  inLow.Code + ")/(" + inHigh.Code + "-" + inLow.Code + ")))",
                              input.ValueType == Type::Numeric ? Type::Scalar : input.ValueType,
                              {}};
            }
            else if (node.Kind == Kind::Posterize)
            {
                const auto input = Input(node, 0);
                const auto steps = Promote(Input(node, 1), Type::Scalar);
                expression = {"(floor((" + input.Code + ")*max(" + steps.Code + ",1))/max(" + steps.Code + ",1))",
                              input.ValueType == Type::Numeric ? Type::Scalar : input.ValueType,
                              {}};
            }
            else if (node.Kind == Kind::HueShift)
            {
                const auto color = Promote(Input(node, 0), Type::Vector3);
                const auto shift = Promote(Input(node, 1), Type::Scalar);
                expression = {"MG_HueShift(" + color.Code + "," + shift.Code + ")", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::Blend)
            {
                const auto base = Promote(Input(node, 0), Type::Vector3);
                const auto blend = Promote(Input(node, 1), Type::Vector3);
                const auto opacity = Promote(Input(node, 2), Type::Scalar);
                expression = {"(" + base.Code + "+(MG_Blend(" + std::to_string(static_cast<int>(node.Value[0])) + "," +
                                  base.Code + "," + blend.Code + ")-" + base.Code + ")*" + opacity.Code + ")",
                              Type::Vector3,
                              {}};
            }
            else if (node.Kind == Kind::LinearGradient)
            {
                const auto uv = Promote(Input(node, 0), Type::Vector2);
                expression = {
                    "saturate((" + uv.Code + ")." + (node.Value[0] > 0.5f ? "y" : "x") + ")", Type::Scalar, {}};
            }
            else if (node.Kind == Kind::RadialGradient)
            {
                const auto uv = Promote(Input(node, 0), Type::Vector2);
                const auto center = Promote(Input(node, 1), Type::Vector2);
                const auto radius = Promote(Input(node, 2), Type::Scalar);
                const auto density = Promote(Input(node, 3), Type::Scalar);
                expression = {"pow(saturate(1-distance(" + uv.Code + "," + center.Code + ")/max(" + radius.Code +
                                  ",1e-5)),max(" + density.Code + ",0))",
                              Type::Scalar,
                              {}};
            }
            else if (node.Kind == Kind::ProceduralNoise || node.Kind == Kind::FractalNoise)
            {
                // CPU（ProcStudio::FractalNoise）と同じ計算。基底 / フラクタル / シードはノードの値（静的）
                m_UsesProceduralNoise = true;
                const bool fractal = node.Kind == Kind::FractalNoise;
                const auto uv = Promote(Input(node, 0), Type::Vector2);
                const auto z = Promote(Input(node, 1), Type::Scalar);
                const auto frequency = Promote(Input(node, 2), Type::Scalar);
                const int basis =
                    std::clamp(static_cast<int>(node.Value[0]), 0, MaterialGraphMath::kNoiseBasisCount - 1);
                const int fractalType =
                    fractal ? std::clamp(static_cast<int>(node.Value[1]), 0, MaterialGraphMath::kNoiseFractalCount - 1)
                            : 6;
                const uint32_t seed = static_cast<uint32_t>((std::max)(node.Value[fractal ? 2 : 1], 0.0f));
                const bool tileable = node.Value[fractal ? 3 : 2] > 0.5f;
                std::string tail;
                if (fractal)
                {
                    tail = Promote(Input(node, 3), Type::Scalar).Code + "," +
                           Promote(Input(node, 4), Type::Scalar).Code + "," +
                           Promote(Input(node, 5), Type::Scalar).Code + "," +
                           Promote(Input(node, 6), Type::Scalar).Code + "," +
                           Promote(Input(node, 7), Type::Scalar).Code + ",0,4,2,0";
                }
                else
                {
                    tail = "1,2,0.5,0.6,1," + Promote(Input(node, 3), Type::Scalar).Code + "," +
                           Promote(Input(node, 4), Type::Scalar).Code + "," +
                           Promote(Input(node, 5), Type::Scalar).Code + "," +
                           Promote(Input(node, 6), Type::Scalar).Code;
                }
                expression = {"MG_ProcNoise(float3(" + uv.Code + "," + z.Code + ")," + std::to_string(basis) + "u," +
                                  std::to_string(fractalType) + "u," + std::to_string(seed) + "u," +
                                  (tileable ? "1u," : "0u,") + frequency.Code + "," + tail + ")",
                              Type::Scalar,
                              {}};
            }
            else if (IsConvertTextureNode(node.Kind))
            {
                if (node.Texture == 0)
                {
                    m_Program.Error = "Convert To Normal / Height node " + std::to_string(node.Id) + " has no texture.";
                }
                const auto uv = Promote(Input(node, 0), Type::Vector2);
                const auto contrast = Promote(Input(node, 1), Type::Scalar);
                const auto bias = Promote(Input(node, 2), Type::Scalar);
                // 格納値で計算するので Linear として登録する（Texture Sample の Linear と同じ）
                const auto sampler = MaterialSamplerType::Linear;
                size_t index = 0;
                for (; index < m_Program.Textures.size(); ++index)
                {
                    if (m_Program.Textures[index] == node.Texture && m_Program.TextureSamplers[index] == sampler)
                    {
                        break;
                    }
                }
                if (index == m_Program.Textures.size())
                {
                    m_Program.Textures.push_back(node.Texture);
                    m_Program.TextureSamplers.push_back(sampler);
                }
                const std::string texture = m_Runtime ? "MG_GraphTexture(context," + std::to_string(index) + "u)"
                                                      : "GraphTextures[" + std::to_string(index) + "]";
                const std::string common = std::to_string(std::clamp(static_cast<int>(node.Value[1]), 0,
                                                                     MaterialGraphMath::kConvertChannelCount - 1)) +
                                           "," + (node.Value[2] > 0.5f ? "1" : "0") + "," + contrast.Code + "," +
                                           bias.Code;
                const bool normal = node.Kind == Kind::ConvertToNormal ||
                                    (node.Kind == Kind::ConvertToNormalHeight && node.Value[0] > 0.5f);
                if (normal)
                {
                    const auto strength = Promote(Input(node, 3), Type::Scalar);
                    const auto radius = Promote(Input(node, 4), Type::Scalar);
                    expression = {"MG_ConvertNormal(" + texture + ",GraphSampler," + uv.Code + "," + common + "," +
                                      strength.Code + "," + radius.Code + "," + (node.Value[3] > 0.5f ? "1" : "0") +
                                      ")",
                                  Type::Vector3,
                                  {}};
                }
                else
                {
                    expression = {
                        "MG_ConvertTap(" + texture + ",GraphSampler," + uv.Code + "," + common + ")", Type::Scalar, {}};
                }
            }
            else if (node.Kind == Kind::SolidCoordinate)
            {
                const bool raw = m_Graph.InputLink(node.InputPins[0]) != nullptr;
                auto position = Promote(Input(node, 0), Type::Vector3);
                const auto scale = Promote(Input(node, 1), Type::Scalar);
                const auto offset = Promote(Input(node, 2), Type::Vector3);
                const std::string base = raw ? position.Code : "(" + position.Code + "*0.5+0.5)";
                expression = {"(" + base + "*" + scale.Code + "+" + offset.Code + ")", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::SolidTexture || node.Kind == Kind::SolidPattern)
            {
                m_UsesProceduralNoise = true;
                const bool raw = m_Graph.InputLink(node.InputPins[0]) != nullptr;
                const auto position = Promote(Input(node, 0), Type::Vector3);
                const int kind =
                    std::clamp(static_cast<int>(node.Value[0]), 0, static_cast<int>(MaterialSolidKind::Count) - 1);
                const int basis =
                    std::clamp(static_cast<int>(node.Value[1]), 0, MaterialGraphMath::kNoiseBasisCount - 1);
                const int fractal =
                    std::clamp(static_cast<int>(node.Value[2]), 0, MaterialGraphMath::kNoiseFractalCount - 1);
                const uint32_t seed = static_cast<uint32_t>((std::max)(node.Value[3], 0.0f));
                std::string code = "MG_SolidScalar(" + (raw ? position.Code : "(" + position.Code + "*0.5+0.5)") + "," +
                                   std::to_string(kind) + "," + std::to_string(basis) + "u," + std::to_string(fractal) +
                                   "u," + std::to_string(seed) + "u";
                for (size_t pin = 1; pin <= 12; ++pin)
                {
                    code += "," + Promote(Input(node, pin), Type::Scalar).Code;
                }
                code += ")";
                if (node.Kind == Kind::SolidPattern)
                {
                    expression = {code, Type::Scalar, {}};
                }
                else
                {
                    expression = {"MG_SolidColor(" + code + "," + std::to_string(kind) + "," +
                                      Promote(Input(node, 13), Type::Vector3).Code + "," +
                                      Promote(Input(node, 14), Type::Vector3).Code + "," +
                                      Promote(Input(node, 15), Type::Vector3).Code + ")",
                                  Type::Vector3,
                                  {}};
                }
            }
            else if (node.Kind == Kind::LocalPosition)
            {
                expression = {"context.WorldPosition", Type::Vector3, {}}; // プレビューではワールド位置
            }
            // ---- オブジェクト / カメラ（プレビューにはオブジェクトが無いので CPU 評価と同じ固定値）
            else if (node.Kind == Kind::ObjectPosition)
            {
                expression = Literal({0, 0, 0, 0}, Type::Vector3);
            }
            else if (node.Kind == Kind::ObjectScale)
            {
                expression = Literal({1, 1, 1, 0}, Type::Vector3);
            }
            else if (node.Kind == Kind::ObjectOrientation)
            {
                expression = Literal({0, 1, 0, 0}, Type::Vector3);
            }
            else if (node.Kind == Kind::ObjectRadius)
            {
                expression = Literal({1.7320508f, 0, 0, 0}, Type::Scalar);
            }
            else if (node.Kind == Kind::ObjectBounds)
            {
                const int mode = std::clamp(static_cast<int>(node.Value[0]), 0, 3);
                const float value = mode == 0 ? 2.0f : mode == 1 ? 0.0f : mode == 2 ? -1.0f : 1.0f;
                expression = Literal({value, value, value, 0}, Type::Vector3);
            }
            else if (node.Kind == Kind::BoundingBoxUVW)
            {
                expression = {"(context.WorldPosition*0.5+0.5)", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::PerInstanceRandom)
            {
                expression = Literal({0.5f, 0, 0, 0}, Type::Scalar);
            }
            else if (node.Kind == Kind::TransformVector || node.Kind == Kind::TransformPosition)
            {
                expression = Promote(Input(node, 0), Type::Vector3);
            }
            else if (node.Kind == Kind::CameraPosition)
            {
                expression = {"(context.WorldPosition+context.CameraVector*3)", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::PixelDepth)
            {
                expression = Literal({3, 0, 0, 0}, Type::Scalar);
            }
            else if (node.Kind == Kind::ScreenPosition)
            {
                expression = {"context.UV", Type::Vector2, {}};
            }
            else if (node.Kind == Kind::DitherOpacity)
            {
                const auto opacity = Promote(Input(node, 0), Type::Scalar);
                expression = {"step(MG_Bayer(context.UV*256)," + opacity.Code + ")", Type::Scalar, {}};
            }
            else if (node.Kind == Kind::DistanceFade)
            {
                const auto start = Promote(Input(node, 0), Type::Scalar);
                const auto end = Promote(Input(node, 1), Type::Scalar);
                expression = {
                    "saturate((3-" + start.Code + ")/(" + end.Code + "-" + start.Code + "))", Type::Scalar, {}};
            }
            else if (node.Kind == Kind::HeightGradient)
            {
                const auto start = Promote(Input(node, 0), Type::Scalar);
                const auto end = Promote(Input(node, 1), Type::Scalar);
                const std::string y = static_cast<int>(node.Value[0]) == 2 ? "(context.WorldPosition.y*0.5+0.5)"
                                                                           : "context.WorldPosition.y";
                expression = {
                    "saturate((" + y + "-" + start.Code + ")/(" + end.Code + "-" + start.Code + "))", Type::Scalar, {}};
            }
            else if (node.Kind == Kind::VertexColor)
            {
                expression = Literal({1, 1, 1, 1}, Type::Vector4);
            }
            else if (node.Kind == Kind::CameraDistance)
            {
                expression = Literal({3, 3, 3, 3}, Type::Scalar);
            }
            else if (node.Kind == Kind::SlopeMask)
            {
                const auto up = Promote(Input(node, 0), Type::Vector3);
                const auto threshold = Promote(Input(node, 1), Type::Scalar);
                const auto softness = Promote(Input(node, 2), Type::Scalar);
                expression = {"MG_SlopeMask(normalize(context.WorldNormal)," + up.Code + "," + threshold.Code + "," +
                                  softness.Code + ")",
                              Type::Scalar,
                              {}};
            }
            else if (node.Kind == Kind::HeightLerp)
            {
                auto a = Input(node, 0);
                auto b = Input(node, 1);
                const Type type =
                    static_cast<Type>((std::max)({static_cast<int>(a.ValueType), static_cast<int>(b.ValueType),
                                                  static_cast<int>(Type::Scalar)}));
                a = Promote(std::move(a), type);
                b = Promote(std::move(b), type);
                const auto height = Promote(Input(node, 2), Type::Scalar);
                const auto phase = Promote(Input(node, 3), Type::Scalar);
                const auto contrast = Promote(Input(node, 4), Type::Scalar);
                expression = {"lerp(" + a.Code + "," + b.Code + ",MG_HeightLerpAlpha(" + height.Code + "," +
                                  phase.Code + "," + contrast.Code + "))",
                              type,
                              {}};
            }
            else if (node.Kind == Kind::ColorAdjust)
            {
                expression = {"MG_ColorAdjust(" + Promote(Input(node, 0), Type::Vector3).Code + "," +
                                  Promote(Input(node, 1), Type::Scalar).Code + "," +
                                  Promote(Input(node, 2), Type::Scalar).Code + "," +
                                  Promote(Input(node, 3), Type::Scalar).Code + ")",
                              Type::Vector3,
                              {}};
            }
            else if (node.Kind == Kind::TriplanarTexture)
            {
                if (node.Texture == 0)
                {
                    m_Program.Error = "Triplanar Texture node " + std::to_string(node.Id) + " has no texture.";
                }
                const auto sampler =
                    static_cast<MaterialSamplerType>(std::clamp(static_cast<int>(node.Value[0]), 0, 2));
                size_t index = 0;
                for (; index < m_Program.Textures.size(); ++index)
                {
                    if (m_Program.Textures[index] == node.Texture && m_Program.TextureSamplers[index] == sampler)
                    {
                        break;
                    }
                }
                if (index == m_Program.Textures.size())
                {
                    m_Program.Textures.push_back(node.Texture);
                    m_Program.TextureSamplers.push_back(sampler);
                }
                const std::string texture = m_Runtime ? "MG_GraphTexture(context," + std::to_string(index) + "u)"
                                                      : "GraphTextures[" + std::to_string(index) + "]";
                const auto position = Promote(Input(node, 0), Type::Vector3);
                const auto scale = Promote(Input(node, 1), Type::Scalar);
                std::string code = "MG_SolidTriplanar(" + texture + ",GraphSampler,(" + position.Code + "*" +
                                   scale.Code + "),normalize(context.WorldNormal)," +
                                   Promote(Input(node, 2), Type::Scalar).Code + ")";
                if (sampler == MaterialSamplerType::Normal)
                {
                    code = "MG_UnpackNormal(" + code + ")";
                }
                expression = {code, Type::Vector4, {}};
            }
            else if (node.Kind == Kind::ConvertToSolid && node.Texture == 0)
            {
                expression = Promote(Input(node, 0), Type::Vector4); // 元画像なし：入力をそのまま通す
            }
            else if (node.Kind == Kind::ConvertToSolid)
            {
                m_UsesProceduralNoise = true; // TextureNoise::Hash / Unit（スタジオと同じハッシュ）
                const auto sampler = static_cast<int>(node.Value[1]) == static_cast<int>(MaterialSamplerType::Color)
                                         ? MaterialSamplerType::Color
                                         : MaterialSamplerType::Linear;
                size_t index = 0;
                for (; index < m_Program.Textures.size(); ++index)
                {
                    if (m_Program.Textures[index] == node.Texture && m_Program.TextureSamplers[index] == sampler)
                    {
                        break;
                    }
                }
                if (index == m_Program.Textures.size())
                {
                    m_Program.Textures.push_back(node.Texture);
                    m_Program.TextureSamplers.push_back(sampler);
                }
                const std::string texture = m_Runtime ? "MG_GraphTexture(context," + std::to_string(index) + "u)"
                                                      : "GraphTextures[" + std::to_string(index) + "]";
                const bool raw = m_Graph.InputLink(node.InputPins[1]) != nullptr;
                const auto position = Promote(Input(node, 1), Type::Vector3);
                const auto scale = Promote(Input(node, 2), Type::Scalar);
                const std::string p =
                    "(" + (raw ? position.Code : "(" + position.Code + "*0.5+0.5)") + "*" + scale.Code + ")";
                std::string code;
                if (static_cast<int>(node.Value[0]) == static_cast<int>(MaterialSolidConvertMethod::Stochastic))
                {
                    const uint32_t seed = static_cast<uint32_t>((std::max)(node.Value[2], 0.0f));
                    code = "MG_SolidStochastic(" + texture + ",GraphSampler," + p + ",normalize(context.WorldNormal)," +
                           Promote(Input(node, 5), Type::Scalar).Code + "," +
                           Promote(Input(node, 6), Type::Scalar).Code + "," + std::to_string(seed) + "u)";
                }
                else if (static_cast<int>(node.Value[0]) == static_cast<int>(MaterialSolidConvertMethod::Triplanar))
                {
                    code = "MG_SolidTriplanar(" + texture + ",GraphSampler," + p + ",normalize(context.WorldNormal)," +
                           Promote(Input(node, 5), Type::Scalar).Code + ")";
                }
                else
                {
                    const uint32_t seed = static_cast<uint32_t>((std::max)(node.Value[2], 0.0f));
                    code = "MG_SolidVolume(" + texture + ",GraphSampler," + p + "," +
                           Promote(Input(node, 3), Type::Scalar).Code + "," +
                           Promote(Input(node, 4), Type::Scalar).Code + "," + std::to_string(seed) + "u)";
                }
                if (node.Value[3] > 0.5f)
                {
                    code = "MG_Gray(" + code + ")";
                }
                expression = {code, Type::Vector4, {}};
            }
            else if (node.Kind == Kind::ThreeColorRamp)
            {
                expression = {"MG_ThreeColorRamp(" + Promote(Input(node, 0), Type::Scalar).Code + "," +
                                  Promote(Input(node, 1), Type::Vector3).Code + "," +
                                  Promote(Input(node, 2), Type::Vector3).Code + "," +
                                  Promote(Input(node, 3), Type::Vector3).Code + ")",
                              Type::Vector3,
                              {}};
            }
            else if (node.Kind == Kind::NoiseCombine)
            {
                const auto a = Promote(Input(node, 0), Type::Scalar);
                const auto b = Promote(Input(node, 1), Type::Scalar);
                const auto factor = Promote(Input(node, 2), Type::Scalar);
                const int mode = std::clamp(static_cast<int>(node.Value[0]), 0,
                                            static_cast<int>(MaterialNoiseCombineMode::Count) - 1);
                expression = {"MG_NoiseCombine(" + std::to_string(mode) + "," + a.Code + "," + b.Code + "," +
                                  factor.Code + "," + (node.Value[1] > 0.5f ? "1" : "0") + ")",
                              Type::Scalar,
                              {}};
            }
            else if (node.Kind == Kind::NoiseLayers)
            {
                // 繋がっている層だけを重み付き平均（重み 0 以下は無視）
                std::string sum = "0";
                std::string weights = "0";
                for (size_t layer = 0; layer < 4; ++layer)
                {
                    const size_t pin = layer * 2;
                    if (pin + 1 >= node.InputPins.size() || !m_Graph.InputLink(node.InputPins[pin]))
                    {
                        continue;
                    }
                    const auto value = Promote(Input(node, pin), Type::Scalar);
                    const auto weight = Promote(Input(node, pin + 1), Type::Scalar);
                    const std::string w = "max(" + weight.Code + ",0)";
                    sum += "+" + value.Code + "*" + w;
                    weights += "+" + w;
                }
                expression = {"((" + weights + ")>0?(" + sum + ")/(" + weights + "):0)", Type::Scalar, {}};
            }
            else if (node.Kind == Kind::Voronoi)
            {
                const auto uv = Promote(Input(node, 0), Type::Vector2);
                const auto scale = Promote(Input(node, 1), Type::Scalar);
                const auto jitter = Promote(Input(node, 2), Type::Scalar);
                expression = {"MG_Voronoi(" + uv.Code + "*" + scale.Code + "," + jitter.Code + "," +
                                  (node.Value[0] > 0.5f ? "1" : "0") + ")",
                              Type::Scalar,
                              {}};
            }
            else if (node.Kind == Kind::NormalFromHeight)
            {
                const auto height = Promote(Input(node, 0), Type::Scalar);
                const auto strength = Promote(Input(node, 1), Type::Scalar);
                expression = {
                    "MG_NormalFromHeight(" + height.Code + ",context.UV," + strength.Code + ")", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::BlendAngleCorrectedNormals)
            {
                const auto base = Promote(Input(node, 0), Type::Vector3);
                const auto detail = Promote(Input(node, 1), Type::Vector3);
                expression = {
                    "MG_BlendAngleCorrectedNormals(" + base.Code + "," + detail.Code + ")", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::FlattenNormal)
            {
                const auto normal = Promote(Input(node, 0), Type::Vector3);
                const auto flatness = Promote(Input(node, 1), Type::Scalar);
                expression = {"normalize(" + normal.Code + "+(float3(0,0,1)-" + normal.Code + ")*" + flatness.Code +
                                  ")",
                              Type::Vector3,
                              {}};
            }
            else if (node.Kind == Kind::BumpOffset)
            {
                const auto height = Promote(Input(node, 0), Type::Scalar);
                const auto ratio = Promote(Input(node, 1), Type::Scalar);
                const auto reference = Promote(Input(node, 2), Type::Scalar);
                const auto coordinate = Promote(Input(node, 3), Type::Vector2);
                expression = {
                    "(" + coordinate.Code +
                        "+float2(dot(context.CameraVector,context.Tangent),dot(context.CameraVector,context.Bitangent))*((" +
                        height.Code + "-" + reference.Code + ")*" + ratio.Code + "))",
                    Type::Vector2,
                    {}};
            }
            else if (node.Kind == Kind::RotateAboutAxis)
            {
                const auto axis = Promote(Input(node, 0), Type::Vector3);
                const auto angle = Promote(Input(node, 1), Type::Scalar);
                const auto pivot = Promote(Input(node, 2), Type::Vector3);
                const auto position = Promote(Input(node, 3), Type::Vector3);
                expression = {"MG_RotateAboutAxis(" + axis.Code + "," + angle.Code + "," + pivot.Code + "," +
                                  position.Code + ")",
                              Type::Vector3,
                              {}};
            }
            else if (node.Kind == Kind::BlackBody)
            {
                const auto temperature = Promote(Input(node, 0), Type::Scalar);
                expression = {"MG_BlackBody(" + temperature.Code + ")", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::ConvertToTexture && node.Texture == 0)
            {
                expression = Promote(Input(node, 0), Type::Vector4); // 未変換：入力をそのまま通す
            }
            else if (node.Kind == Kind::TextureSample || node.Kind == Kind::ConvertToTexture)
            {
                if (node.Texture == 0)
                {
                    m_Program.Error = "Texture Sample node " + std::to_string(node.Id) + " has no texture.";
                }
                const auto uv = Promote(Input(node, node.Kind == Kind::ConvertToTexture ? 1 : 0), Type::Vector2);
                const auto sampler =
                    static_cast<MaterialSamplerType>(std::clamp(static_cast<int>(node.Value[0]), 0, 2));
                size_t index = 0;
                for (; index < m_Program.Textures.size(); ++index)
                {
                    if (m_Program.Textures[index] == node.Texture && m_Program.TextureSamplers[index] == sampler)
                    {
                        break;
                    }
                }
                if (index == m_Program.Textures.size())
                {
                    m_Program.Textures.push_back(node.Texture);
                    m_Program.TextureSamplers.push_back(sampler);
                }
                std::string texture = "GraphTextures[" + std::to_string(index) + "]";
                if (m_Runtime)
                {
                    texture = "MG_GraphTexture(context," + std::to_string(index) + "u)";
                }
                std::string sample = texture + ".Sample(GraphSampler," + uv.Code + ")";
                if (static_cast<int>(node.Value[0]) == static_cast<int>(MaterialSamplerType::Normal))
                {
                    sample = "MG_UnpackNormal(" + sample + ")";
                }
                expression = {sample, Type::Vector4, {}};
            }
            else if (node.Kind == Kind::Normalize)
            {
                const auto input = Promote(Input(node, 0), Type::Vector3);
                expression = {"normalize(" + input.Code + ")", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::Dot)
            {
                const auto a = Promote(Input(node, 0), Type::Vector3);
                const auto b = Promote(Input(node, 1), Type::Vector3);
                expression = {"dot(" + a.Code + "," + b.Code + ")", Type::Scalar, {}};
            }
            else if (node.Kind == Kind::Fresnel)
            {
                const auto power = Input(node, 0);
                expression = {"pow(1-saturate(dot(normalize(context.WorldNormal),normalize(context.CameraVector)))," +
                                  power.Code + ")",
                              Type::Scalar,
                              {}};
            }
            else if (node.Kind == Kind::UV)
            {
                expression = {"context.UV", Type::Vector2, {}};
            }
            else if (node.Kind == Kind::WorldPosition)
            {
                expression = {"context.WorldPosition", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::WorldNormal)
            {
                expression = {"context.WorldNormal", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::CameraVector)
            {
                expression = {"context.CameraVector", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::Time)
            {
                expression = {"context.Time", Type::Scalar, {}};
                m_Program.Animated = true;
            }
            else
            {
                m_Program.Error = "Unsupported value node: " + std::to_string(node.Id);
                expression = Literal({}, Type::Scalar);
            }
            m_Program.DependencyOrder.push_back(node.Id);
            const std::string file = "material_node_" + std::to_string(node.Id);
            const auto shared = m_CommonExpressions.find(expression.Code);
            if (shared != m_CommonExpressions.end())
            {
                expression.Code = shared->second;
            }
            else if (!expression.Constant)
            {
                const std::string variable = "node_" + std::to_string(node.Id);
                const auto line = 1 + static_cast<size_t>(std::count(m_Body.begin(), m_Body.end(), '\n'));
                m_Program.Sources.push_back({node.Id, file, line});
                m_Body += "#line 1 \"" + file + "\"\n" + HlslType(expression.ValueType) + " " + variable + " = " +
                          expression.Code + ";\n";
                m_CommonExpressions[expression.Code] = variable;
                expression.Code = variable;
            }
            m_Expressions[node.Id] = expression;
            return expression;
        }

        const MaterialGraph& m_Graph;
        bool m_Runtime = false;
        bool m_UsesProceduralNoise = false;
        MaterialGraphProgram m_Program;
        std::string m_Body;
        std::unordered_map<uint64_t, Expression> m_Expressions;
        std::unordered_map<std::string, std::string> m_CommonExpressions;
    };
}

MaterialGraphProgram MaterialGraphCompiler::Generate(const MaterialGraph& graph, uint64_t targetNode)
{
    return Generator(graph).Run(targetNode);
}

MaterialGraphProgram MaterialGraphCompiler::GenerateRuntime(const MaterialGraph& graph, uint64_t targetNode)
{
    return Generator(graph, true).Run(targetNode);
}

// ============================================================================
// 描画時評価（GPU）用プログラム（MaterialGraphVM.hlsli が解釈する）
// ============================================================================
namespace MaterialGraphVm
{
    namespace
    {
        using Type = MaterialGraphType;
        using Kind = MaterialNodeKind;
        using Float4 = std::array<float, 4>;

        // MaterialGraphVM.hlsli の MG_OP_* と同じ番号
        enum Op : int
        {
            OpNop = 0,
            OpConst = 1,
            OpInput = 2,
            OpUnary = 3,
            OpBinary = 4,
            OpArith = 5,
            OpComponent = 6,
            OpPromote4 = 7,
            OpZeroFill = 8,
            OpSwizzle = 9,
            OpAppend = 10,
            OpDot3 = 11,
            OpCross = 12,
            OpLength = 13,
            OpDistance3 = 14,
            OpNormalize3 = 15,
            OpTexCoord = 16,
            OpRotate2 = 17,
            OpDesaturate = 18,
            OpIf = 19,
            OpSphereMask = 20,
            OpCheapContrast = 21,
            OpValueNoise = 22,
            OpRemap = 23,
            OpPosterize = 24,
            OpHueShift = 25,
            OpBlend = 26,
            OpRadialGradient = 27,
            OpNoise = 28,
            OpWorley = 29,
            OpNoiseCombine = 30,
            OpVoronoi = 31,
            OpBlendAngle = 32,
            OpFlatten = 33,
            OpRotateAxis = 34,
            OpBlackBody = 35,
            OpTexSample = 36,
            OpConvertHeight = 37,
            OpConvertNormal = 38,
            OpSolidVolume = 39,
            OpTriplanar = 40,
            OpThreeRamp = 41,
            OpFresnel = 42,
            OpColorAdjust = 43,
            OpSlopeMask = 44,
            OpHeightLerpAlpha = 45,
            OpTransform = 46,
            OpDither = 47,
            OpExpand4 = 48,
            OpSolidStochastic = 49,
        };

        // OpInput の種類（MaterialGraphVM.hlsli の MG_IN_*）
        enum InputKind : int
        {
            InUV = 0,
            InWorldPosition = 1,
            InObjectPosition = 2,
            InSolidPosition = 3, // ソリッド / ノイズの既定座標（オブジェクト or ワールド：グラフ設定）
            InNormal = 4,
            InCameraVector = 5,
            InTime = 6,
            InReflection = 7,
            InVertexColor = 8,
            InCameraDistance = 9,
            InObjectOrigin = 10,
            InObjectScale = 11,
            InObjectOrientation = 12,
            InObjectRadius = 13,
            InBoundsCenter = 14,
            InBoundsSize = 15,
            InBoundsMin = 16,
            InBoundsMax = 17,
            InBoundsUVW = 18,
            InInstanceRandom = 19,
            InCameraPosition = 20,
            InPixelDepth = 21,
            InScreenUV = 22,
        };

        // OpArith の種類
        enum ArithKind : int
        {
            ArAdd = 0,
            ArSub = 1,
            ArMul = 2,
            ArDiv = 3,
            ArLerp = 4,
            ArClamp = 5,
            ArSmoothstep = 6,
            ArBiasScale = 7, // (a + b) * c
            ArSmooth = 8,    // TextureNoise::Smooth(a, b, c)（スタジオと同じ）
        };

        struct Expr
        {
            int Reg = -1;
            Type T = Type::Scalar;
        };

        struct Instruction
        {
            int Code = OpNop;
            int Dst = -1;
            std::vector<int> Args;
            std::vector<float> Imm;
        };

        int Dims(Type type)
        {
            return type == Type::Numeric ? 1 : static_cast<int>(type);
        }

        Type MaxType(std::initializer_list<Type> types)
        {
            int result = 1;
            for (const auto type : types)
            {
                result = (std::max)(result, Dims(type));
            }
            return static_cast<Type>(result);
        }

        class Generator
        {
          public:
            Generator(const MaterialGraph& graph, const TextureResolver& resolve) : m_Graph(graph), m_Resolve(resolve)
            {
            }

            Program Run(const Options& options)
            {
                Program program;
                const MaterialGraphNode* output = nullptr;
                for (const auto& node : m_Graph.Nodes)
                {
                    if (node.Kind == Kind::Output)
                    {
                        output = &node;
                        break;
                    }
                }
                int outputs[kOutputCount];
                for (int& reg : outputs)
                {
                    reg = -1;
                }
                if (options.TargetNode != 0)
                {
                    // ノード単体（ノードプレビュー / Convert の作成）：ノードの出力を Out0.x に入れる
                    const auto* target = m_Graph.FindNode(options.TargetNode);
                    if (!target || target->Kind == Kind::Output || target->Kind == Kind::Group)
                    {
                        program.Error = "対象のノードがありません";
                        return program;
                    }
                    const Expr value = Visit(*target);
                    if (!m_Error.empty())
                    {
                        program.Error = m_Error;
                        return program;
                    }
                    program.TargetDims = std::clamp(Dims(value.T), 1, 4);
                    outputs[0] = value.Reg;
                }
                else
                {
                    if (!output)
                    {
                        program.Error = "Material Output がありません";
                        return program;
                    }

                    // Out0 / Out1：描画時評価で上書きする出力。Out2：ベイク / プレビュー用（Height / Normal Intensity /
                    // Transmission）
                    struct OutputPin
                    {
                        MaterialOutputPin Pin;
                        Type T;
                    };

                    const OutputPin pins[11] = {{MaterialOutputPin::BaseColor, Type::Vector4},
                                                {MaterialOutputPin::Metallic, Type::Scalar},
                                                {MaterialOutputPin::Roughness, Type::Scalar},
                                                {MaterialOutputPin::Normal, Type::Vector3},
                                                {MaterialOutputPin::Emissive, Type::Vector3},
                                                {MaterialOutputPin::Opacity, Type::Scalar},
                                                {MaterialOutputPin::OpacityMask, Type::Scalar},
                                                {MaterialOutputPin::AO, Type::Scalar},
                                                {MaterialOutputPin::Height, Type::Scalar},
                                                {MaterialOutputPin::NormalIntensity, Type::Scalar},
                                                {MaterialOutputPin::Transmission, Type::Scalar}};
                    bool any = false;
                    for (int i = 0; i < 11; ++i)
                    {
                        if (options.EmissiveOnly && pins[i].Pin != MaterialOutputPin::Emissive)
                        {
                            continue;
                        }
                        const size_t index = static_cast<size_t>(pins[i].Pin);
                        const bool linked =
                            index < output->InputPins.size() && m_Graph.InputLink(output->InputPins[index]);
                        // 直値（ピン横のボックス）が入った未接続のピンも、描画時評価の出力に含める
                        const bool direct = m_Graph.HasOutputDirectValue(*output, index);
                        if (!linked && !direct && !(options.IncludeDefaults && index < output->InputPins.size()))
                        {
                            continue;
                        }
                        Expr value = Input(*output, index);
                        if (pins[i].Pin == MaterialOutputPin::BaseColor)
                        {
                            // CPU のベイク（Expand）と同じ：スカラーはグレー、3 成分以下はアルファ 1
                            value = Emit(OpExpand4, Type::Vector4, {value.Reg}, {static_cast<float>(Dims(value.T))});
                        }
                        else
                        {
                            value = Promote(value, pins[i].T);
                        }
                        if (!m_Error.empty())
                        {
                            program.Error = m_Error;
                            return program;
                        }
                        outputs[i] = value.Reg;
                        any |= linked;
                    }
                    if (!any && !options.IncludeDefaults)
                    {
                        program.Error = "描画時評価する出力が繋がっていません";
                        return program;
                    }
                }
                // ---- レジスタ割り当て（生存区間が終わったレジスタを再利用する）
                std::unordered_map<int, size_t> lastUse;
                for (size_t i = 0; i < m_Code.size(); ++i)
                {
                    for (const int arg : m_Code[i].Args)
                    {
                        lastUse[arg] = i;
                    }
                }
                for (const int reg : outputs)
                {
                    if (reg >= 0)
                    {
                        lastUse[reg] = SIZE_MAX;
                    }
                }
                std::unordered_map<int, int> physical;
                std::vector<int> freeList;
                int used = 0;
                for (size_t i = 0; i < m_Code.size(); ++i)
                {
                    auto& instruction = m_Code[i];
                    std::vector<int> released;
                    for (auto& arg : instruction.Args)
                    {
                        const int virtualReg = arg;
                        arg = physical.at(virtualReg);
                        if (lastUse[virtualReg] == i &&
                            std::find(released.begin(), released.end(), virtualReg) == released.end())
                        {
                            released.push_back(virtualReg);
                        }
                    }
                    for (const int virtualReg : released)
                    {
                        freeList.push_back(physical.at(virtualReg));
                    }
                    int reg;
                    if (!freeList.empty())
                    {
                        reg = freeList.back();
                        freeList.pop_back();
                    }
                    else
                    {
                        reg = used++;
                    }
                    const int virtualDst = instruction.Dst;
                    physical[virtualDst] = reg;
                    instruction.Dst = reg;
                    if (!lastUse.contains(virtualDst))
                    {
                        freeList.push_back(reg); // 使われない結果（通常は無い）
                    }
                }
                if (used > kMaxRegisters)
                {
                    program.Error = "グラフが大きすぎます（同時に使う値が " + std::to_string(used) + " 個。上限 " +
                                    std::to_string(kMaxRegisters) + "）";
                    return program;
                }
                // ---- テクセルへ書き出す
                auto& texels = program.Texels;
                texels.assign(kHeaderTexels, Float4{-1, -1, -1, -1});
                for (int i = 0; i < kOutputCount; ++i)
                {
                    const float reg = outputs[i] >= 0 ? static_cast<float>(physical.at(outputs[i])) : -1.0f;
                    texels[1 + i / 4][i % 4] = reg;
                }
                for (const auto& instruction : m_Code)
                {
                    const int argTexels = static_cast<int>((instruction.Args.size() + 3) / 4);
                    const int immTexels = static_cast<int>((instruction.Imm.size() + 3) / 4);
                    const int length = 1 + argTexels + immTexels;
                    texels.push_back({static_cast<float>(instruction.Code), static_cast<float>(instruction.Dst),
                                      static_cast<float>(length), static_cast<float>(argTexels)});
                    for (int t = 0; t < argTexels; ++t)
                    {
                        Float4 value = {-1, -1, -1, -1};
                        for (int c = 0; c < 4; ++c)
                        {
                            const size_t index = static_cast<size_t>(t * 4 + c);
                            if (index < instruction.Args.size())
                            {
                                value[c] = static_cast<float>(instruction.Args[index]);
                            }
                        }
                        texels.push_back(value);
                    }
                    for (int t = 0; t < immTexels; ++t)
                    {
                        Float4 value = {0, 0, 0, 0};
                        for (int c = 0; c < 4; ++c)
                        {
                            const size_t index = static_cast<size_t>(t * 4 + c);
                            if (index < instruction.Imm.size())
                            {
                                value[c] = instruction.Imm[index];
                            }
                        }
                        texels.push_back(value);
                    }
                }
                if (texels.size() > static_cast<size_t>(kMaxTexels))
                {
                    program.Error = "グラフが大きすぎます（命令 " + std::to_string(texels.size()) + " / " +
                                    std::to_string(kMaxTexels) + " テクセル）";
                    program.Texels.clear();
                    return program;
                }
                program.UsesTime = m_UsesTime;
                program.UsesView = m_UsesView;
                program.WorldSolidSpace = m_Graph.Settings.SolidSpace == 1;
                program.Instructions = static_cast<int>(m_Code.size());
                program.Registers = used;
                program.Texels[0] = HeaderTexel(program, 0.0f);
                uint64_t hash = 14695981039346656037ull;
                for (size_t i = 1; i < texels.size(); ++i)
                {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(texels[i].data());
                    for (size_t b = 0; b < sizeof(Float4); ++b)
                    {
                        hash = (hash ^ bytes[b]) * 1099511628211ull;
                    }
                }
                program.Hash = (hash ^ (m_UsesTime ? 1u : 0u) ^ (program.WorldSolidSpace ? 2u : 0u)) * 1099511628211ull;
                program.Success = true;
                return program;
            }

          private:
            Expr Emit(int code, Type type, std::vector<int> args, std::vector<float> imm = {})
            {
                Instruction instruction;
                instruction.Code = code;
                instruction.Dst = m_NextReg++;
                instruction.Args = std::move(args);
                instruction.Imm = std::move(imm);
                m_Code.push_back(std::move(instruction));
                return {m_Code.back().Dst, type == Type::Numeric ? Type::Scalar : type};
            }

            Expr Const(Float4 value, Type type)
            {
                if (type == Type::Numeric)
                {
                    type = Type::Scalar;
                }
                if (type == Type::Scalar)
                {
                    value = {value[0], value[0], value[0], value[0]};
                }
                for (auto& component : value)
                {
                    component = std::isfinite(component) ? component : 0.0f;
                }
                const auto found = m_Constants.find(value);
                if (found != m_Constants.end())
                {
                    return {found->second, type};
                }
                const Expr expr = Emit(OpConst, type, {}, {value[0], value[1], value[2], value[3]});
                m_Constants[value] = expr.Reg;
                return expr;
            }

            Expr Scalar(float value)
            {
                return Const({value, value, value, value}, Type::Scalar);
            }

            Expr Special(int which, Type type)
            {
                const auto found = m_Inputs.find(which);
                if (found != m_Inputs.end())
                {
                    return {found->second, type};
                }
                if (which == InTime)
                {
                    m_UsesTime = true;
                }
                if (which == InCameraDistance || which == InCameraPosition || which == InPixelDepth ||
                    which == InCameraVector || which == InReflection)
                {
                    m_UsesView = true; // 発光の動的ベイク（GI / 反射用）を毎フレーム更新する対象
                }
                const Expr expr = Emit(OpInput, type, {}, {static_cast<float>(which)});
                m_Inputs[which] = expr.Reg;
                return expr;
            }

            Expr Promote(Expr e, Type target)
            {
                if (e.Reg < 0)
                {
                    return e;
                }
                if (e.T == Type::Numeric)
                {
                    e.T = Type::Scalar;
                }
                if (e.T == target || target == Type::Numeric)
                {
                    return e;
                }
                if (e.T == Type::Scalar)
                {
                    e.T = target; // スカラーはレジスタの全成分に入っている
                    return e;
                }
                if (target == Type::Scalar)
                {
                    return Emit(OpComponent, Type::Scalar, {e.Reg}, {0});
                }
                if (e.T == Type::Vector3 && target == Type::Vector4)
                {
                    return Emit(OpPromote4, Type::Vector4, {e.Reg}); // アルファ 1
                }
                if (Dims(e.T) > Dims(target))
                {
                    e.T = target; // 余った成分は使われない
                    return e;
                }
                return Emit(OpZeroFill, target, {e.Reg}, {static_cast<float>(Dims(e.T))});
            }

            Expr Component(Expr e, int component)
            {
                const int count = Dims(e.T);
                if (count == 1)
                {
                    e.T = Type::Scalar;
                    return e;
                }
                if (component >= count)
                {
                    return Scalar(0.0f);
                }
                return Emit(OpComponent, Type::Scalar, {e.Reg}, {static_cast<float>(component)});
            }

            Expr Arith(int kind, Type type, std::vector<int> args)
            {
                return Emit(OpArith, type, std::move(args), {static_cast<float>(kind)});
            }

            Expr Unary(Kind kind, Expr a)
            {
                if (kind == Kind::Logarithm10)
                {
                    const auto positive = BinaryOp(Kind::Max, a.T, a, Scalar(1e-8f));
                    return Arith(ArMul, a.T, {Unary(Kind::Logarithm2, positive).Reg, Scalar(0.301029995664f).Reg});
                }
                if (kind == Kind::Exponential2)
                {
                    const auto bounded = Arith(ArClamp, a.T, {a.Reg, Scalar(-126).Reg, Scalar(126).Reg});
                    const auto exponent = Arith(ArMul, a.T, {bounded.Reg, Scalar(0.69314718056f).Reg});
                    return Unary(Kind::Exponential, exponent);
                }
                return Emit(OpUnary, a.T, {a.Reg},
                            {static_cast<float>(static_cast<int>(kind) - static_cast<int>(Kind::Abs))});
            }

            Expr BinaryOp(Kind kind, Type type, Expr a, Expr b)
            {
                return Emit(OpBinary, type, {a.Reg, b.Reg},
                            {static_cast<float>(static_cast<int>(kind) - static_cast<int>(Kind::Power))});
            }

            // 複合ノードは既存のVM命令へ展開し、共通シェーダーの分岐を増やさない。
            Expr SafeQuotient(Expr numerator, Expr denominator)
            {
                const auto magnitude = Unary(Kind::Abs, denominator);
                const auto safeMagnitude = BinaryOp(Kind::Max, denominator.T, magnitude, Scalar(1e-8f));
                const auto quotient = Arith(ArDiv, numerator.T, {numerator.Reg, safeMagnitude.Reg});
                const auto signedValue = Arith(ArMul, numerator.T, {quotient.Reg, Unary(Kind::Sign, denominator).Reg});
                const auto zero = Scalar(0);
                return Emit(OpIf, numerator.T, {magnitude.Reg, Scalar(1e-8f).Reg, signedValue.Reg, zero.Reg, zero.Reg});
            }

            Expr SoftBand(Expr value, Expr center, Expr halfWidth, Expr feather)
            {
                const auto distance = Unary(Kind::Abs, Arith(ArSub, Type::Scalar, {value.Reg, center.Reg}));
                const auto width = BinaryOp(Kind::Max, Type::Scalar, halfWidth, Scalar(0));
                const auto edge = Arith(ArSub, Type::Scalar, {distance.Reg, width.Reg});
                const auto softness = BinaryOp(Kind::Max, Type::Scalar, feather, Scalar(1e-5f));
                const auto t = Unary(Kind::Saturate, Arith(ArDiv, Type::Scalar, {edge.Reg, softness.Reg}));
                const auto square = Arith(ArMul, Type::Scalar, {t.Reg, t.Reg});
                const auto twice = Arith(ArMul, Type::Scalar, {Scalar(2).Reg, t.Reg});
                const auto factor = Arith(ArSub, Type::Scalar, {Scalar(3).Reg, twice.Reg});
                return Unary(Kind::OneMinus, Arith(ArMul, Type::Scalar, {square.Reg, factor.Reg}));
            }

            void Unsupported(const MaterialGraphNode& node, const char* reason)
            {
                if (m_Error.empty())
                {
                    m_Error = std::string(MaterialGraph::Definition(node.Kind).Name) + ": " + reason;
                }
            }

            int Texture(const MaterialGraphNode& node, MaterialSamplerType sampler)
            {
                if (node.Texture == 0)
                {
                    Unsupported(node, "テクスチャが設定されていません");
                    return 0;
                }
                const int index = m_Resolve ? m_Resolve(node.Texture, sampler) : -1;
                if (index < 0 || index >= 512)
                {
                    Unsupported(node, "テクスチャを読み込めません");
                    return 0;
                }
                return index;
            }

            static bool UsesSolidPosition(Kind kind)
            {
                return kind == Kind::Noise || kind == Kind::SolidTexture || kind == Kind::SolidPattern ||
                       kind == Kind::SolidCoordinate || kind == Kind::ConvertToSolid || kind == Kind::TriplanarTexture;
            }

            Expr Input(const MaterialGraphNode& node, size_t index)
            {
                if (index < node.InputPins.size())
                {
                    if (const auto* link = m_Graph.InputLink(node.InputPins[index]))
                    {
                        size_t outputIndex = 0;
                        if (const auto* source = m_Graph.FindPinNode(link->SourcePin, nullptr, &outputIndex))
                        {
                            Expr expr = Visit(*source);
                            if (outputIndex > 0)
                            {
                                expr = Component(expr, static_cast<int>(outputIndex) - 1);
                            }
                            return expr;
                        }
                    }
                }
                switch (MaterialGraphMath::DefaultInput(node.Kind, index))
                {
                    case MaterialGraphMath::SpecialInput::UV:
                        return Special(InUV, Type::Vector2);
                    case MaterialGraphMath::SpecialInput::Time:
                        return Special(InTime, Type::Scalar);
                    case MaterialGraphMath::SpecialInput::WorldPosition:
                        return Special(UsesSolidPosition(node.Kind) ? InSolidPosition : InWorldPosition, Type::Vector3);
                    default:
                        break;
                }
                const auto& definition = MaterialGraph::Definition(node.Kind);
                if (index >= definition.Inputs.size())
                {
                    return Scalar(0.0f);
                }
                return Const(MaterialGraph::UnconnectedInputValue(node, index), definition.Inputs[index].Type);
            }

            Expr In(const MaterialGraphNode& node, size_t index, Type type)
            {
                return Promote(Input(node, index), type);
            }

            bool Linked(const MaterialGraphNode& node, size_t index) const
            {
                return index < node.InputPins.size() && m_Graph.InputLink(node.InputPins[index]) != nullptr;
            }

            // Solid 系の既定座標：未接続なら (位置 + 1) * 0.5（スタジオと同じ 0〜1）
            Expr SolidPosition(const MaterialGraphNode& node, size_t index)
            {
                Expr position = In(node, index, Type::Vector3);
                if (!Linked(node, index))
                {
                    position = Arith(ArBiasScale, Type::Vector3, {position.Reg, Scalar(1.0f).Reg, Scalar(0.5f).Reg});
                }
                return position;
            }

            static std::vector<float> Seed(uint32_t seed)
            {
                return {static_cast<float>(seed & 0xffffu), static_cast<float>(seed >> 16u)};
            }

            Expr Noise(Expr position, int basis, int fractal, uint32_t seed, const std::array<Expr, 10>& settings)
            {
                std::vector<int> args = {position.Reg};
                for (const auto& setting : settings)
                {
                    args.push_back(setting.Reg);
                }
                auto imm = Seed(seed);
                imm.insert(imm.begin(), {static_cast<float>(basis), static_cast<float>(fractal)});
                imm.push_back(0.0f); // タイルしない
                return Emit(OpNoise, Type::Scalar, std::move(args), std::move(imm));
            }

            // ProcStudio::TextureScalar（solid = true）を命令列に展開する（MG_SolidScalar と同じ式）
            Expr SolidScalar(const MaterialGraphNode& node)
            {
                const Expr p = SolidPosition(node, 0);
                const int kind =
                    std::clamp(static_cast<int>(node.Value[0]), 0, static_cast<int>(MaterialSolidKind::Count) - 1);
                const int basis =
                    std::clamp(static_cast<int>(node.Value[1]), 0, MaterialGraphMath::kNoiseBasisCount - 1);
                const int fractal =
                    std::clamp(static_cast<int>(node.Value[2]), 0, MaterialGraphMath::kNoiseFractalCount - 1);
                const uint32_t seed = static_cast<uint32_t>((std::max)(node.Value[3], 0.0f));
                Expr in[13];
                for (size_t i = 1; i <= 12; ++i)
                {
                    in[i] = In(node, i, Type::Scalar);
                }
                const Expr& frequency = in[1];
                const Expr& turbulence = in[7];
                const Expr& rings = in[8];
                const Expr& coverage = in[9];
                const Expr& softness = in[10];
                const Expr& contrast = in[11];
                const Expr& bias = in[12];
                const std::array<Expr, 10> settings = {frequency,    in[2],       in[3],        in[4],
                                                       in[5],        in[6],       Scalar(0.0f), Scalar(4.0f),
                                                       Scalar(2.0f), Scalar(0.0f)};
                Expr v;
                const auto S = Type::Scalar;
                switch (static_cast<MaterialSolidKind>(kind))
                {
                    case MaterialSolidKind::Marble: {
                        const Expr n = Noise(p, basis, fractal, seed, settings);
                        const Expr x = Component(p, 0);
                        const Expr rounded = Unary(Kind::Round, rings);
                        Expr t = Arith(ArMul, S, {x.Reg, rounded.Reg});
                        t = Arith(ArAdd, S, {t.Reg, Arith(ArMul, S, {n.Reg, turbulence.Reg}).Reg});
                        t = Arith(ArMul, S, {t.Reg, Scalar(6.28318531f).Reg});
                        t = Unary(Kind::Sine, t);
                        t = Arith(ArBiasScale, S, {t.Reg, Scalar(1.0f).Reg, Scalar(0.5f).Reg});
                        t = BinaryOp(Kind::Max, S, t, Scalar(0.0f));
                        v = BinaryOp(Kind::Power, S, t, Scalar(0.6f));
                        break;
                    }
                    case MaterialSolidKind::Wood: {
                        const Expr n = Noise(p, basis, fractal, seed, settings);
                        const Expr cx = Arith(ArSub, S, {Component(p, 0).Reg, Scalar(0.5f).Reg});
                        const Expr cz = Arith(ArSub, S, {Component(p, 2).Reg, Scalar(0.5f).Reg});
                        Expr r = Arith(ArAdd, S,
                                       {Arith(ArMul, S, {cx.Reg, cx.Reg}).Reg, Arith(ArMul, S, {cz.Reg, cz.Reg}).Reg});
                        r = Unary(Kind::SquareRoot, r);
                        r = Arith(ArMul, S, {r.Reg, Arith(ArMul, S, {rings.Reg, Scalar(2.0f).Reg}).Reg});
                        const Expr wobble =
                            Arith(ArMul, S, {Arith(ArMul, S, {n.Reg, turbulence.Reg}).Reg, Scalar(0.5f).Reg});
                        r = Arith(ArAdd, S, {r.Reg, wobble.Reg});
                        const Expr f = Unary(Kind::Frac, r);
                        const Expr rise = Arith(ArSmooth, S, {Scalar(0.0f).Reg, Scalar(0.8f).Reg, f.Reg});
                        const Expr fall = Arith(ArSmooth, S, {Scalar(0.85f).Reg, Scalar(1.0f).Reg, f.Reg});
                        v = Arith(ArMul, S, {rise.Reg, Unary(Kind::OneMinus, fall).Reg});
                        break;
                    }
                    case MaterialSolidKind::Granite: {
                        const Expr cells = Noise(p, 2, 0, seed, settings); // Worley, fBm
                        const Expr grainPosition = Arith(ArMul, Type::Vector3, {p.Reg, Scalar(4.0f).Reg});
                        const Expr grain = Noise(grainPosition, basis, fractal, seed, settings);
                        Expr t = Arith(ArAdd, S,
                                       {Arith(ArMul, S, {cells.Reg, Scalar(0.6f).Reg}).Reg,
                                        Arith(ArMul, S, {grain.Reg, Scalar(0.4f).Reg}).Reg});
                        t = Unary(Kind::Saturate, t);
                        // v > 0.62 ? 1 : (v < 0.3 ? 0 : v)
                        t = Emit(OpIf, S, {t.Reg, Scalar(0.62f).Reg, Scalar(1.0f).Reg, t.Reg, t.Reg});
                        v = Emit(OpIf, S, {t.Reg, Scalar(0.3f).Reg, t.Reg, t.Reg, Scalar(0.0f).Reg});
                        break;
                    }
                    case MaterialSolidKind::Voronoi: {
                        const Expr scaled = Arith(ArMul, Type::Vector3, {p.Reg, frequency.Reg});
                        auto imm = Seed(seed);
                        const Expr f = Emit(OpWorley, Type::Vector2, {scaled.Reg}, std::move(imm));
                        const Expr d = Arith(ArSub, S, {Component(f, 1).Reg, Component(f, 0).Reg});
                        v = Unary(Kind::Saturate, Arith(ArMul, S, {d.Reg, Scalar(3.0f).Reg}));
                        break;
                    }
                    case MaterialSolidKind::Cloud: {
                        const Expr base = Noise(p, basis, 0, seed, settings);
                        const Expr billows = Unary(Kind::OneMinus, Noise(p, 2, 0, seed ^ 0x7f4a7c15u, settings));
                        const Expr shape = Arith(ArAdd, S,
                                                 {Arith(ArMul, S, {base.Reg, Scalar(0.65f).Reg}).Reg,
                                                  Arith(ArMul, S, {billows.Reg, Scalar(0.35f).Reg}).Reg});
                        const Expr threshold = Unary(Kind::OneMinus, coverage);
                        const Expr half = Arith(ArMul, S, {softness.Reg, Scalar(0.5f).Reg});
                        v = Arith(ArSmooth, S,
                                  {Arith(ArSub, S, {threshold.Reg, half.Reg}).Reg,
                                   Arith(ArAdd, S, {threshold.Reg, half.Reg}).Reg, shape.Reg});
                        break;
                    }
                    default:
                        v = Noise(p, basis, fractal, seed, settings);
                        break;
                }
                // saturate((v - 0.5) * contrast + 0.5 + bias)
                Expr t = Arith(ArSub, S, {v.Reg, Scalar(0.5f).Reg});
                t = Arith(ArMul, S, {t.Reg, contrast.Reg});
                t = Arith(ArAdd, S, {t.Reg, Scalar(0.5f).Reg});
                t = Arith(ArAdd, S, {t.Reg, bias.Reg});
                return Unary(Kind::Saturate, t);
            }

            Expr Visit(const MaterialGraphNode& node)
            {
                if (const auto found = m_Memo.find(node.Id); found != m_Memo.end())
                {
                    return found->second;
                }
                if (++m_Depth > 512)
                {
                    Unsupported(node, "グラフが深すぎます");
                    --m_Depth;
                    return Scalar(0.0f);
                }
                Expr e = VisitNode(node);
                --m_Depth;
                if (e.Reg < 0)
                {
                    e = Scalar(0.0f);
                }
                m_Memo[node.Id] = e;
                return e;
            }

            Expr VisitNode(const MaterialGraphNode& node)
            {
                const auto S = Type::Scalar;
                const auto V2 = Type::Vector2;
                const auto V3 = Type::Vector3;
                const auto V4 = Type::Vector4;
                if (node.Kind == Kind::Reroute || node.Kind == Kind::BreakVector)
                {
                    return Input(node, 0);
                }
                if (node.Kind == Kind::StaticSwitch)
                {
                    return Input(node, node.Value[0] > 0.5f ? 0 : 1);
                }
                if (MaterialGraphMath::IsConstantNode(node.Kind))
                {
                    const Type type = MaterialGraph::Definition(node.Kind).OutputType;
                    if (!m_Graph.HasConnectedComponent(node))
                    {
                        return Const(node.Value, type);
                    }
                    // 成分ピン：成分ごとのスカラーを Append でつなぐ（未接続の成分は Input がノード上の値を返す）
                    Expr result = Promote(Input(node, 0), S);
                    for (int i = 1; i < Dims(type) && i < static_cast<int>(node.InputPins.size()); ++i)
                    {
                        const Expr component = Promote(Input(node, static_cast<size_t>(i)), S);
                        if (result.Reg < 0 || component.Reg < 0)
                        {
                            return {};
                        }
                        result = Emit(OpAppend, static_cast<Type>(i + 1), {result.Reg, component.Reg},
                                      {static_cast<float>(i), 1.0f});
                    }
                    return result;
                }
                if (node.Kind == Kind::DDX || node.Kind == Kind::DDY)
                {
                    Unsupported(node, "画面微分は描画時評価では使えません（ベイクで評価します）");
                    return {};
                }
                if (MaterialGraphMath::IsUnary(node.Kind))
                {
                    return Unary(node.Kind, Input(node, 0));
                }
                if (MaterialGraphMath::IsBinary(node.Kind))
                {
                    Expr a = Input(node, 0);
                    Expr b = Input(node, 1);
                    const Type type = MaxType({a.T, b.T});
                    a = Promote(a, type);
                    if (node.Kind != Kind::Power)
                    {
                        b = Promote(b, type);
                    }
                    return BinaryOp(node.Kind, type, a, b);
                }
                switch (node.Kind)
                {
                    case Kind::ConstantBiasScale: {
                        const Expr a = Input(node, 0);
                        return Arith(ArBiasScale, a.T, {a.Reg, Input(node, 1).Reg, Input(node, 2).Reg});
                    }
                    case Kind::Add:
                    case Kind::Subtract:
                    case Kind::Multiply:
                    case Kind::Divide:
                    case Kind::Lerp:
                    case Kind::Clamp:
                    case Kind::Smoothstep: {
                        std::vector<Expr> inputs;
                        Type type = S;
                        for (size_t i = 0; i < node.InputPins.size(); ++i)
                        {
                            inputs.push_back(Input(node, i));
                            type = MaxType({type, inputs.back().T});
                        }
                        std::vector<int> args;
                        for (auto& input : inputs)
                        {
                            args.push_back(Promote(input, type).Reg);
                        }
                        static const int codes[] = {ArAdd, ArSub, ArMul, ArDiv, ArLerp, ArClamp, ArSmoothstep};
                        return Arith(codes[static_cast<int>(node.Kind) - static_cast<int>(Kind::Add)], type,
                                     std::move(args));
                    }
                    case Kind::TextureCoordinate:
                        return Emit(OpTexCoord, V2, {}, {node.Value[0], node.Value[1]});
                    case Kind::Panner: {
                        const Expr coordinate = In(node, 0, V2);
                        const Expr time = In(node, 1, S);
                        const Expr speed = In(node, 2, V2);
                        return Arith(ArAdd, V2, {coordinate.Reg, Arith(ArMul, V2, {speed.Reg, time.Reg}).Reg});
                    }
                    case Kind::Rotator: {
                        const Expr coordinate = In(node, 0, V2);
                        const Expr angle = Arith(ArMul, S, {In(node, 1, S).Reg, In(node, 2, S).Reg});
                        return Emit(OpRotate2, V2, {coordinate.Reg, In(node, 3, V2).Reg, angle.Reg});
                    }
                    case Kind::CrossProduct:
                        return Emit(OpCross, V3, {In(node, 0, V3).Reg, In(node, 1, V3).Reg});
                    case Kind::Distance:
                        return Emit(OpDistance3, S, {In(node, 0, V3).Reg, In(node, 1, V3).Reg});
                    case Kind::Length: {
                        const Expr a = Input(node, 0);
                        return Emit(OpLength, S, {a.Reg}, {static_cast<float>(Dims(a.T))});
                    }
                    case Kind::AppendVector: {
                        const Expr a = Input(node, 0);
                        const Expr b = Input(node, 1);
                        const int count = Dims(a.T) + Dims(b.T);
                        if (count > 4)
                        {
                            Unsupported(node, "4 成分を超えます");
                            return {};
                        }
                        return Emit(OpAppend, static_cast<Type>(count), {a.Reg, b.Reg},
                                    {static_cast<float>(Dims(a.T)), static_cast<float>(Dims(b.T))});
                    }
                    case Kind::ComponentMask: {
                        const Expr a = Input(node, 0);
                        const int available = Dims(a.T);
                        std::vector<float> swizzle;
                        for (int i = 0; i < 4; ++i)
                        {
                            if (node.Value[i] > 0.5f)
                            {
                                swizzle.push_back(static_cast<float>((std::min)(i, available - 1)));
                            }
                        }
                        if (swizzle.empty())
                        {
                            swizzle.push_back(0.0f);
                        }
                        const int count = static_cast<int>(swizzle.size());
                        if (count == 1)
                        {
                            swizzle.assign(4, swizzle[0]); // スカラーは全成分
                        }
                        while (swizzle.size() < 4)
                        {
                            swizzle.push_back(4.0f); // 4 = 0
                        }
                        return Emit(OpSwizzle, static_cast<Type>(count), {a.Reg}, swizzle);
                    }
                    case Kind::ReflectionVector:
                        return Special(InReflection, V3);
                    case Kind::Desaturation:
                        return Emit(OpDesaturate, V3, {In(node, 0, V3).Reg, In(node, 1, S).Reg});
                    case Kind::If: {
                        const Expr a = In(node, 0, S);
                        const Expr b = In(node, 1, S);
                        Expr branches[3] = {Input(node, 2), Input(node, 3), Input(node, 4)};
                        const Type type = MaxType({branches[0].T, branches[1].T, branches[2].T});
                        return Emit(OpIf, type,
                                    {a.Reg, b.Reg, Promote(branches[0], type).Reg, Promote(branches[1], type).Reg,
                                     Promote(branches[2], type).Reg});
                    }
                    case Kind::SphereMask:
                        return Emit(OpSphereMask, S,
                                    {In(node, 0, V3).Reg, In(node, 1, V3).Reg, In(node, 2, S).Reg, In(node, 3, S).Reg});
                    case Kind::CheapContrast:
                        return Emit(OpCheapContrast, S, {In(node, 0, S).Reg, In(node, 1, S).Reg});
                    case Kind::Noise:
                        return Emit(OpValueNoise, S, {In(node, 0, V3).Reg, In(node, 1, S).Reg});
                    case Kind::Pi:
                        return Scalar(3.14159265f * node.Value[0]);
                    case Kind::InverseLerp: {
                        const auto value = Input(node, 0);
                        const auto numerator = Arith(ArSub, value.T, {value.Reg, In(node, 1, S).Reg});
                        const auto denominator = Arith(ArSub, S, {In(node, 2, S).Reg, In(node, 1, S).Reg});
                        return SafeQuotient(numerator, denominator);
                    }
                    case Kind::ReflectVector: {
                        const auto incident = In(node, 0, V3);
                        const auto normal = In(node, 1, V3);
                        const auto dot = Emit(OpDot3, S, {incident.Reg, normal.Reg});
                        const auto lengthSquared = Emit(OpDot3, S, {normal.Reg, normal.Reg});
                        const auto scale = Arith(ArMul, S, {SafeQuotient(dot, lengthSquared).Reg, Scalar(2).Reg});
                        const auto projection = Arith(ArMul, V3, {normal.Reg, scale.Reg});
                        return Arith(ArSub, V3, {incident.Reg, projection.Reg});
                    }
                    case Kind::RenderSoftBand:
                        return SoftBand(In(node, 0, S), In(node, 1, S), In(node, 2, S), In(node, 3, S));
                    case Kind::RenderRadialPulse: {
                        const auto radius = Emit(OpLength, S, {In(node, 0, V3).Reg}, {3});
                        const auto frequency = Arith(ArMul, S, {radius.Reg, In(node, 2, S).Reg});
                        const auto phase = Arith(ArSub, S, {frequency.Reg, In(node, 1, S).Reg});
                        const auto wave = Unary(Kind::Frac, Arith(ArAdd, S, {phase.Reg, Scalar(0.5f).Reg}));
                        const auto width = Arith(ArClamp, S, {In(node, 3, S).Reg, Scalar(0).Reg, Scalar(0.5f).Reg});
                        return SoftBand(wave, Scalar(0.5f), width, Scalar(0.05f));
                    }
                    case Kind::RenderFlowUV: {
                        const auto uv = In(node, 0, V2);
                        const auto time = In(node, 2, S);
                        const auto scroll = Arith(ArMul, V2, {In(node, 1, V2).Reg, time.Reg});
                        const auto swapped = Emit(OpSwizzle, V2, {uv.Reg}, {1, 0, 4, 4});
                        const auto angles = Arith(ArMul, V2, {swapped.Reg, Scalar(6.283185307f).Reg});
                        const auto negativeTime = Arith(ArMul, S, {time.Reg, Scalar(-1).Reg});
                        const auto phase = Emit(OpAppend, V2, {time.Reg, negativeTime.Reg}, {1, 1});
                        const auto wave = Unary(Kind::Sine, Arith(ArAdd, V2, {angles.Reg, phase.Reg}));
                        const auto offset = Arith(ArMul, V2, {wave.Reg, In(node, 3, S).Reg});
                        return Arith(ArAdd, V2, {Arith(ArAdd, V2, {uv.Reg, scroll.Reg}).Reg, offset.Reg});
                    }
                    case Kind::RemapValueRange: {
                        const Expr a = Input(node, 0);
                        return Emit(
                            OpRemap, a.T,
                            {a.Reg, In(node, 1, S).Reg, In(node, 2, S).Reg, In(node, 3, S).Reg, In(node, 4, S).Reg});
                    }
                    case Kind::Posterize: {
                        const Expr a = Input(node, 0);
                        return Emit(OpPosterize, a.T, {a.Reg, In(node, 1, S).Reg});
                    }
                    case Kind::HueShift:
                        return Emit(OpHueShift, V3, {In(node, 0, V3).Reg, In(node, 1, S).Reg});
                    case Kind::Blend:
                        return Emit(OpBlend, V3, {In(node, 0, V3).Reg, In(node, 1, V3).Reg, In(node, 2, S).Reg},
                                    {static_cast<float>(static_cast<int>(node.Value[0]))});
                    case Kind::LinearGradient:
                        return Unary(Kind::Saturate, Component(In(node, 0, V2), node.Value[0] > 0.5f ? 1 : 0));
                    case Kind::RadialGradient:
                        return Emit(OpRadialGradient, S,
                                    {In(node, 0, V2).Reg, In(node, 1, V2).Reg, In(node, 2, S).Reg, In(node, 3, S).Reg});
                    case Kind::ProceduralNoise:
                    case Kind::FractalNoise: {
                        const bool fractal = node.Kind == Kind::FractalNoise;
                        const Expr position =
                            Emit(OpAppend, V3, {In(node, 0, V2).Reg, In(node, 1, S).Reg}, {2.0f, 1.0f});
                        const int basis =
                            std::clamp(static_cast<int>(node.Value[0]), 0, MaterialGraphMath::kNoiseBasisCount - 1);
                        const int fractalType = fractal ? std::clamp(static_cast<int>(node.Value[1]), 0,
                                                                     MaterialGraphMath::kNoiseFractalCount - 1)
                                                        : 6;
                        const uint32_t seed = static_cast<uint32_t>((std::max)(node.Value[fractal ? 2 : 1], 0.0f));
                        const bool tileable = node.Value[fractal ? 3 : 2] > 0.5f;
                        std::array<Expr, 10> settings;
                        settings[0] = In(node, 2, S);
                        if (fractal)
                        {
                            for (int i = 0; i < 5; ++i)
                            {
                                settings[1 + i] = In(node, 3 + i, S);
                            }
                            settings[6] = Scalar(0.0f);
                            settings[7] = Scalar(4.0f);
                            settings[8] = Scalar(2.0f);
                            settings[9] = Scalar(0.0f);
                        }
                        else
                        {
                            settings[1] = Scalar(1.0f);
                            settings[2] = Scalar(2.0f);
                            settings[3] = Scalar(0.5f);
                            settings[4] = Scalar(0.6f);
                            settings[5] = Scalar(1.0f);
                            for (int i = 0; i < 4; ++i)
                            {
                                settings[6 + i] = In(node, 3 + i, S);
                            }
                        }
                        Expr result = Noise(position, basis, fractalType, seed, settings);
                        m_Code.back().Imm[4] = tileable ? 1.0f : 0.0f;
                        return result;
                    }
                    case Kind::ConvertToNormalHeight:
                    case Kind::ConvertToHeight:
                    case Kind::ConvertToNormal: {
                        const int heap = Texture(node, MaterialSamplerType::Linear);
                        const float channel = static_cast<float>(std::clamp(
                            static_cast<int>(node.Value[1]), 0, MaterialGraphMath::kConvertChannelCount - 1));
                        const float invert = node.Value[2] > 0.5f ? 1.0f : 0.0f;
                        const bool normal = node.Kind == Kind::ConvertToNormal ||
                                            (node.Kind == Kind::ConvertToNormalHeight && node.Value[0] > 0.5f);
                        if (normal)
                        {
                            return Emit(
                                OpConvertNormal, V3,
                                {In(node, 0, V2).Reg, In(node, 1, S).Reg, In(node, 2, S).Reg, In(node, 3, S).Reg,
                                 In(node, 4, S).Reg},
                                {static_cast<float>(heap), channel, invert, node.Value[3] > 0.5f ? 1.0f : 0.0f});
                        }
                        return Emit(OpConvertHeight, S, {In(node, 0, V2).Reg, In(node, 1, S).Reg, In(node, 2, S).Reg},
                                    {static_cast<float>(heap), channel, invert});
                    }
                    case Kind::SolidCoordinate: {
                        const Expr position = SolidPosition(node, 0);
                        const Expr scaled = Arith(ArMul, V3, {position.Reg, In(node, 1, S).Reg});
                        return Arith(ArAdd, V3, {scaled.Reg, In(node, 2, V3).Reg});
                    }
                    case Kind::SolidPattern:
                        return SolidScalar(node);
                    case Kind::SolidTexture: {
                        const Expr v = SolidScalar(node);
                        if (static_cast<int>(node.Value[0]) == static_cast<int>(MaterialSolidKind::Cloud))
                        {
                            // lerp(空色, lerp(B, 1, 0.5), v)
                            const Expr sky = Const({0.32f, 0.52f, 0.86f, 0.0f}, V3);
                            const Expr bright =
                                Arith(ArLerp, V3, {In(node, 14, V3).Reg, Scalar(1.0f).Reg, Scalar(0.5f).Reg});
                            return Arith(ArLerp, V3, {sky.Reg, bright.Reg, v.Reg});
                        }
                        return Emit(OpThreeRamp, V3,
                                    {v.Reg, In(node, 13, V3).Reg, In(node, 14, V3).Reg, In(node, 15, V3).Reg});
                    }
                    case Kind::ConvertToSolid: {
                        if (node.Texture == 0)
                        {
                            return In(node, 0, V4); // 元画像なし：入力をそのまま通す
                        }
                        const auto sampler =
                            static_cast<int>(node.Value[1]) == static_cast<int>(MaterialSamplerType::Color)
                                ? MaterialSamplerType::Color
                                : MaterialSamplerType::Linear;
                        const int heap = Texture(node, sampler);
                        const Expr position = SolidPosition(node, 1);
                        const Expr p = Arith(ArMul, V3, {position.Reg, In(node, 2, S).Reg});
                        const float gray = node.Value[3] > 0.5f ? 1.0f : 0.0f;
                        if (static_cast<int>(node.Value[0]) == static_cast<int>(MaterialSolidConvertMethod::Stochastic))
                        {
                            auto imm = Seed(static_cast<uint32_t>((std::max)(node.Value[2], 0.0f)));
                            imm.insert(imm.begin(), static_cast<float>(heap));
                            imm.push_back(gray);
                            return Emit(OpSolidStochastic, V4, {p.Reg, In(node, 5, S).Reg, In(node, 6, S).Reg},
                                        std::move(imm));
                        }
                        if (static_cast<int>(node.Value[0]) == static_cast<int>(MaterialSolidConvertMethod::Triplanar))
                        {
                            return Emit(OpTriplanar, V4, {p.Reg, In(node, 5, S).Reg},
                                        {static_cast<float>(heap), gray, static_cast<float>(sampler)});
                        }
                        auto imm = Seed(static_cast<uint32_t>((std::max)(node.Value[2], 0.0f)));
                        imm.insert(imm.begin(), static_cast<float>(heap));
                        imm.push_back(gray);
                        return Emit(OpSolidVolume, V4, {p.Reg, In(node, 3, S).Reg, In(node, 4, S).Reg}, std::move(imm));
                    }
                    case Kind::ThreeColorRamp:
                        return Emit(
                            OpThreeRamp, V3,
                            {In(node, 0, S).Reg, In(node, 1, V3).Reg, In(node, 2, V3).Reg, In(node, 3, V3).Reg});
                    case Kind::NoiseCombine:
                        return Emit(
                            OpNoiseCombine, S, {In(node, 0, S).Reg, In(node, 1, S).Reg, In(node, 2, S).Reg},
                            {static_cast<float>(std::clamp(static_cast<int>(node.Value[0]), 0,
                                                           static_cast<int>(MaterialNoiseCombineMode::Count) - 1)),
                             node.Value[1] > 0.5f ? 1.0f : 0.0f});
                    case Kind::NoiseLayers: {
                        Expr sum = Scalar(0.0f);
                        Expr weights = Scalar(0.0f);
                        for (size_t layer = 0; layer < 4; ++layer)
                        {
                            const size_t pin = layer * 2;
                            if (pin + 1 >= node.InputPins.size() || !Linked(node, pin))
                            {
                                continue;
                            }
                            const Expr weight = BinaryOp(Kind::Max, S, In(node, pin + 1, S), Scalar(0.0f));
                            sum = Arith(ArAdd, S, {sum.Reg, Arith(ArMul, S, {In(node, pin, S).Reg, weight.Reg}).Reg});
                            weights = Arith(ArAdd, S, {weights.Reg, weight.Reg});
                        }
                        const Expr average = Arith(ArDiv, S, {sum.Reg, weights.Reg});
                        const Expr zero = Scalar(0.0f);
                        return Emit(OpIf, S, {weights.Reg, zero.Reg, average.Reg, zero.Reg, zero.Reg});
                    }
                    case Kind::Voronoi: {
                        const Expr uv = Arith(ArMul, V2, {In(node, 0, V2).Reg, In(node, 1, S).Reg});
                        return Emit(OpVoronoi, S, {uv.Reg, In(node, 2, S).Reg}, {node.Value[0] > 0.5f ? 1.0f : 0.0f});
                    }
                    case Kind::NormalFromHeight:
                        Unsupported(node,
                                    "画面微分を使うので描画時評価では使えません（Convert To Normal を使ってください）");
                        return {};
                    case Kind::BumpOffset:
                        Unsupported(node, "描画時評価では使えません（ベイクで評価します）");
                        return {};
                    case Kind::BlendAngleCorrectedNormals:
                        return Emit(OpBlendAngle, V3, {In(node, 0, V3).Reg, In(node, 1, V3).Reg});
                    case Kind::FlattenNormal:
                        return Emit(OpFlatten, V3, {In(node, 0, V3).Reg, In(node, 1, S).Reg});
                    case Kind::RotateAboutAxis:
                        return Emit(
                            OpRotateAxis, V3,
                            {In(node, 0, V3).Reg, In(node, 1, S).Reg, In(node, 2, V3).Reg, In(node, 3, V3).Reg});
                    case Kind::BlackBody:
                        return Emit(OpBlackBody, V3, {In(node, 0, S).Reg});
                    case Kind::ConvertToTexture:
                        if (node.Texture == 0)
                        {
                            return In(node, 0, V4); // 未変換：入力をそのまま通す
                        }
                        [[fallthrough]];
                    case Kind::TextureSample: {
                        const auto sampler =
                            static_cast<MaterialSamplerType>(std::clamp(static_cast<int>(node.Value[0]), 0, 2));
                        const int heap = Texture(node, sampler);
                        const Expr uv = In(node, node.Kind == Kind::ConvertToTexture ? 1 : 0, V2);
                        return Emit(OpTexSample, V4, {uv.Reg}, {static_cast<float>(heap), static_cast<float>(sampler)});
                    }
                    case Kind::Normalize:
                        return Emit(OpNormalize3, V3, {In(node, 0, V3).Reg});
                    case Kind::Dot:
                        return Emit(OpDot3, S, {In(node, 0, V3).Reg, In(node, 1, V3).Reg});
                    case Kind::Fresnel:
                        m_UsesView = true;
                        return Emit(OpFresnel, S, {In(node, 0, S).Reg});
                    case Kind::UV:
                        return Special(InUV, V2);
                    case Kind::WorldPosition:
                        return Special(InWorldPosition, V3);
                    case Kind::WorldNormal:
                        return Special(InNormal, V3);
                    case Kind::CameraVector:
                        return Special(InCameraVector, V3);
                    case Kind::Time:
                        return Special(InTime, S);
                    // ---- 便利ノード
                    case Kind::LocalPosition:
                        return Special(InObjectPosition, V3);
                    // ---- オブジェクト / カメラ
                    case Kind::ObjectPosition:
                        return Special(InObjectOrigin, V3);
                    case Kind::ObjectScale:
                        return Special(InObjectScale, V3);
                    case Kind::ObjectOrientation:
                        return Special(InObjectOrientation, V3);
                    case Kind::ObjectRadius:
                        return Special(InObjectRadius, S);
                    case Kind::ObjectBounds: {
                        static const int inputs[] = {InBoundsSize, InBoundsCenter, InBoundsMin, InBoundsMax};
                        return Special(inputs[std::clamp(static_cast<int>(node.Value[0]), 0, 3)], V3);
                    }
                    case Kind::BoundingBoxUVW:
                        return Special(InBoundsUVW, V3);
                    case Kind::PerInstanceRandom:
                        return Special(InInstanceRandom, S);
                    case Kind::TransformVector: {
                        static const float modes[] = {0, 1, 4, 5};
                        return Emit(OpTransform, V3, {In(node, 0, V3).Reg},
                                    {modes[std::clamp(static_cast<int>(node.Value[0]), 0, 3)]});
                    }
                    case Kind::TransformPosition:
                        return Emit(OpTransform, V3, {In(node, 0, V3).Reg},
                                    {static_cast<int>(node.Value[0]) == 1 ? 3.0f : 2.0f});
                    case Kind::CameraPosition:
                        return Special(InCameraPosition, V3);
                    case Kind::PixelDepth:
                        return Special(InPixelDepth, S);
                    case Kind::ScreenPosition:
                        return Special(InScreenUV, V2);
                    case Kind::DitherOpacity:
                        return Emit(OpDither, S, {In(node, 0, S).Reg});
                    case Kind::DistanceFade: {
                        const Expr distance = Special(InCameraDistance, S);
                        const Expr start = In(node, 0, S);
                        const Expr end = In(node, 1, S);
                        const Expr t = Arith(ArDiv, S,
                                             {Arith(ArSub, S, {distance.Reg, start.Reg}).Reg,
                                              Arith(ArSub, S, {end.Reg, start.Reg}).Reg});
                        return Unary(Kind::Saturate, t);
                    }
                    case Kind::HeightGradient: {
                        const int mode = std::clamp(static_cast<int>(node.Value[0]), 0, 2);
                        const Expr position = Special(mode == 0   ? InWorldPosition
                                                      : mode == 1 ? InObjectPosition
                                                                  : InBoundsUVW,
                                                      V3);
                        const Expr y = Component(position, 1);
                        const Expr start = In(node, 0, S);
                        const Expr end = In(node, 1, S);
                        const Expr t =
                            Arith(ArDiv, S,
                                  {Arith(ArSub, S, {y.Reg, start.Reg}).Reg, Arith(ArSub, S, {end.Reg, start.Reg}).Reg});
                        return Unary(Kind::Saturate, t);
                    }
                    case Kind::VertexColor:
                        return Special(InVertexColor, V4);
                    case Kind::CameraDistance:
                        return Special(InCameraDistance, S);
                    case Kind::SlopeMask:
                        return Emit(OpSlopeMask, S, {In(node, 0, V3).Reg, In(node, 1, S).Reg, In(node, 2, S).Reg});
                    case Kind::HeightLerp: {
                        const Expr a = Input(node, 0);
                        const Expr b = Input(node, 1);
                        const Type type = MaxType({a.T, b.T});
                        const Expr alpha =
                            Emit(OpHeightLerpAlpha, S, {In(node, 2, S).Reg, In(node, 3, S).Reg, In(node, 4, S).Reg});
                        return Arith(ArLerp, type, {Promote(a, type).Reg, Promote(b, type).Reg, alpha.Reg});
                    }
                    case Kind::ColorAdjust:
                        return Emit(OpColorAdjust, V3,
                                    {In(node, 0, V3).Reg, In(node, 1, S).Reg, In(node, 2, S).Reg, In(node, 3, S).Reg});
                    case Kind::TriplanarTexture: {
                        const auto sampler =
                            static_cast<MaterialSamplerType>(std::clamp(static_cast<int>(node.Value[0]), 0, 2));
                        const int heap = Texture(node, sampler);
                        const Expr p = Arith(ArMul, V3, {In(node, 0, V3).Reg, In(node, 1, S).Reg});
                        return Emit(OpTriplanar, V4, {p.Reg, In(node, 2, S).Reg},
                                    {static_cast<float>(heap), 0.0f, static_cast<float>(sampler)});
                    }
                    default:
                        Unsupported(node, "描画時評価に未対応のノードです（ベイクで評価します）");
                        return {};
                }
            }

            const MaterialGraph& m_Graph;
            const TextureResolver& m_Resolve;
            std::vector<Instruction> m_Code;
            std::unordered_map<uint64_t, Expr> m_Memo;
            std::map<Float4, int> m_Constants;
            std::unordered_map<int, int> m_Inputs;
            std::string m_Error;
            int m_NextReg = 0;
            int m_Depth = 0;
            bool m_UsesTime = false;
            bool m_UsesView = false;
        };
    } // namespace

    Program Compile(const MaterialGraph& graph, const TextureResolver& resolveTexture, const Options& options)
    {
        std::string error;
        if (!graph.Validate(error))
        {
            Program program;
            program.Error = error;
            return program;
        }
        return Generator(graph, resolveTexture).Run(options);
    }

    std::array<float, 4> HeaderTexel(const Program& program, float time)
    {
        return {time, static_cast<float>(kHeaderTexels), static_cast<float>(program.Texels.size()),
                static_cast<float>((program.UsesTime ? 1 : 0) | (program.WorldSolidSpace ? 2 : 0))};
    }

    Program Compile(const MaterialGraph& graph, const TextureResolver& resolveTexture)
    {
        std::string error;
        if (!graph.Validate(error))
        {
            Program program;
            program.Error = error;
            return program;
        }
        return Generator(graph, resolveTexture).Run(Options{});
    }
} // namespace MaterialGraphVm
