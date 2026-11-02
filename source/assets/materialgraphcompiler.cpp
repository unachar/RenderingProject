#include "pch.h"
#include "materialgraphcompiler.h"
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
        stream << std::setprecision(9) << std::scientific << value;
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
            value.Code = HlslType(target) + "(" + value.Code + ")";
            value.ValueType = target;
        }
        return value;
    }

    class Generator
    {
      public:
        explicit Generator(const MaterialGraph& graph) : m_Graph(graph) {}

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
                const char* fields[] = {"BaseColor", "Metallic", "Roughness",   "Normal",
                                        "Emissive",  "Opacity",  "OpacityMask", "AO"};
                const auto source = "material_node_" + std::to_string(node->Id);
                result = "#line 1 \"" + source + "\"\nGeneratedMaterial result;\n";
                m_Program.Sources.push_back({node->Id, source, 1});
                for (size_t i = 0; i < node->InputPins.size(); ++i)
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
                "struct MaterialGraphContext { float2 UV; float3 WorldPosition; float3 WorldNormal; float3 CameraVector; float Time; };\n"
                "struct GeneratedMaterial { float4 BaseColor; float Metallic; float Roughness; float3 Normal; float3 Emissive; float Opacity; float OpacityMask; float AO; };\n";
            if (!m_Program.Parameters.empty())
            {
                header += "cbuffer MaterialGraphParameters : register(b0) { float4 GraphParameters[" +
                          std::to_string(m_Program.Parameters.size()) + "]; };\n";
            }
            if (!m_Program.Textures.empty())
            {
                header += "Texture2D<float4> GraphTextures[" + std::to_string(m_Program.Textures.size()) +
                          "] : register(t0);\nSamplerState GraphSampler : register(s0);\n";
            }
            if (m_Program.MaterialOutput)
            {
                header += "GeneratedMaterial EvaluateMaterialGraph(MaterialGraphContext context) {\n";
            }
            else
            {
                header += "float4 EvaluateMaterialNode(MaterialGraphContext context) {\n";
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
            if (node.Kind == Kind::TextureSample && index == 0)
            {
                return {"context.UV", Type::Vector2, {}};
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

        Expression Visit(const MaterialGraphNode& node)
        {
            if (const auto found = m_Expressions.find(node.Id); found != m_Expressions.end())
            {
                return found->second;
            }
            Expression expression;
            if (node.Kind == Kind::Constant || node.Kind == Kind::ScalarParameter ||
                node.Kind == Kind::VectorParameter || node.Kind == Kind::ColorParameter)
            {
                const auto index = m_Program.Parameters.size();
                m_Program.Parameters.push_back(node.Value);
                m_Program.ParameterNodes.push_back(node.Id);
                expression.Code = "GraphParameters[" + std::to_string(index) + "]";
                expression.ValueType = MaterialGraph::Definition(node.Kind).OutputType;
                if (expression.ValueType == Type::Scalar)
                {
                    expression.Code += ".x";
                }
                if (expression.ValueType == Type::Vector3)
                {
                    expression.Code += ".xyz";
                }
            }
            else if (node.Kind >= Kind::Add && node.Kind <= Kind::Smoothstep)
            {
                expression = Arithmetic(node);
            }
            else if (node.Kind == Kind::TextureSample)
            {
                if (node.Texture == 0)
                {
                    m_Program.Error = "Texture Sample node " + std::to_string(node.Id) + " has no texture.";
                }
                const auto uv = Promote(Input(node, 0), Type::Vector2);
                const auto existing = std::find(m_Program.Textures.begin(), m_Program.Textures.end(), node.Texture);
                size_t index = existing - m_Program.Textures.begin();
                if (existing == m_Program.Textures.end())
                {
                    m_Program.Textures.push_back(node.Texture);
                }
                expression = {"GraphTextures[" + std::to_string(index) + "].Sample(GraphSampler," + uv.Code + ")",
                              Type::Vector4,
                              {}};
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
