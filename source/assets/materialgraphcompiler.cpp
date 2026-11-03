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
                for (int component = static_cast<int>(value.ValueType); component < static_cast<int>(target); ++component)
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
                const char* fields[] = {"BaseColor",   "Metallic",        "Roughness",    "Normal",
                                        "Emissive",    "Opacity",         "OpacityMask",  "AO",
                                        "Height",      "NormalIntensity", "ParallaxDepth", "ParallaxSteps",
                                        "IOR",         "Transmission",    "TransmissionRoughness",
                                        "Thickness",   "Absorption",      "ShadingStrength"};
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
            if (m_Runtime)
            {
                header += "float4 MG_Parameter(MaterialGraphContext context, uint index) { return LoadMaterialTexture(context.ParameterTexture).Load(int3(index,0,0)); }\n";
                header += "Texture2D<float4> MG_GraphTexture(MaterialGraphContext context, uint index) { return LoadMaterialTexture(asuint(MG_Parameter(context," +
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
                if (const auto* source = m_Graph.FindPinNode(link->SourcePin))
                {
                    return Visit(*source);
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
            return Literal(input.Default, input.Type);
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
            if (node.Kind == Kind::Reroute)
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
                if (expression.ValueType == Type::Scalar)
                {
                    expression.Code += ".x";
                }
                if (expression.ValueType == Type::Vector3)
                {
                    expression.Code += ".xyz";
                }
                if (expression.ValueType == Type::Vector2)
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
                expression = {"reflect(-normalize(context.CameraVector),normalize(context.WorldNormal))", Type::Vector3,
                              {}};
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
                expression = {"saturate((" + uv.Code + ")." + (node.Value[0] > 0.5f ? "y" : "x") + ")", Type::Scalar, {}};
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
                expression = {"MG_NormalFromHeight(" + height.Code + ",context.UV," + strength.Code + ")", Type::Vector3,
                              {}};
            }
            else if (node.Kind == Kind::BlendAngleCorrectedNormals)
            {
                const auto base = Promote(Input(node, 0), Type::Vector3);
                const auto detail = Promote(Input(node, 1), Type::Vector3);
                expression = {"MG_BlendAngleCorrectedNormals(" + base.Code + "," + detail.Code + ")", Type::Vector3, {}};
            }
            else if (node.Kind == Kind::FlattenNormal)
            {
                const auto normal = Promote(Input(node, 0), Type::Vector3);
                const auto flatness = Promote(Input(node, 1), Type::Scalar);
                expression = {"normalize(" + normal.Code + "+(float3(0,0,1)-" + normal.Code + ")*" + flatness.Code + ")",
                              Type::Vector3,
                              {}};
            }
            else if (node.Kind == Kind::BumpOffset)
            {
                const auto height = Promote(Input(node, 0), Type::Scalar);
                const auto ratio = Promote(Input(node, 1), Type::Scalar);
                const auto reference = Promote(Input(node, 2), Type::Scalar);
                const auto coordinate = Promote(Input(node, 3), Type::Vector2);
                expression = {"(" + coordinate.Code + "+float2(dot(context.CameraVector,context.Tangent),dot(context.CameraVector,context.Bitangent))*((" +
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
            else if (node.Kind == Kind::TextureSample)
            {
                if (node.Texture == 0)
                {
                    m_Program.Error = "Texture Sample node " + std::to_string(node.Id) + " has no texture.";
                }
                const auto uv = Promote(Input(node, 0), Type::Vector2);
                const auto sampler = static_cast<MaterialSamplerType>(std::clamp(static_cast<int>(node.Value[0]), 0, 2));
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

