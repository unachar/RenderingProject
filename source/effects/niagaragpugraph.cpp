#include "pch.h"
#include "niagaragpugraph.h"
#include "materialgraphmath.h"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace Niagara
{
    namespace
    {
        using Kind = MaterialNodeKind;

        std::string Number(float value)
        {
            std::ostringstream text;
            text.imbue(std::locale::classic());
            text << std::scientific << std::setprecision(9) << value;
            return text.str();
        }

        struct Value
        {
            std::string Expression = "float4(0,0,0,0)";
            int Count = 1;
        };

        std::string RawComponent(const Value& value, int index)
        {
            return "(" + value.Expression + ")." + "xyzw"[index];
        }

        std::string Component(const Value& value, int index)
        {
            int component = 0;
            if (value.Count > 1)
            {
                component = (std::min)(index, value.Count - 1);
            }
            return RawComponent(value, component);
        }

        std::string Pack(const std::array<std::string, 4>& values)
        {
            return "float4(" + values[0] + "," + values[1] + "," + values[2] + "," + values[3] + ")";
        }

        class Generator
        {
          public:
            Generator(const Module& module, const System& system) : m_Module(module), m_System(system) {}

            bool Generate(std::string& hlsl, std::string& diagnostic)
            {
                std::string graphError;
                if (m_Module.Graph.Nodes.size() > 128 || m_Module.Graph.Links.size() > 512 ||
                    !m_Module.Graph.Validate(graphError))
                {
                    diagnostic = "GPU custom graph is invalid: " + graphError;
                    return false;
                }
                const auto* output = m_Module.Graph.FindPinNode(m_Module.OutputPin);
                if (!output || !output->OwnsOutput(m_Module.OutputPin))
                {
                    diagnostic = "GPU custom graph needs an output pin.";
                    return false;
                }
                int expected = 1;
                if (m_Module.Target == Attribute::Position || m_Module.Target == Attribute::Velocity)
                {
                    expected = 3;
                }
                else if (m_Module.Target == Attribute::Color)
                {
                    expected = 4;
                }
                if (static_cast<int>(m_Module.Graph.PinOutputType(m_Module.OutputPin)) != expected)
                {
                    diagnostic = "GPU custom graph output type does not match its particle attribute.";
                    return false;
                }
                if (m_Module.Target == Attribute::Lifetime && m_Module.ExecutionStage != Stage::ParticleSpawn)
                {
                    diagnostic = "GPU custom graph may write lifetime only during Particle Spawn.";
                    return false;
                }
                const Value result = Pin(m_Module.OutputPin, 0);
                if (!m_Error.empty())
                {
                    diagnostic = m_Error;
                    return false;
                }
                const auto value = result.Expression;
                // CPU ApplyGraph removes particles if a graph produces non-finite values.
                m_Code += "if(!all(isfinite(" + value + "))){p.VelocityLifetime.w=0;}else{";
                switch (m_Module.Target)
                {
                    case Attribute::Position:
                        m_Code += "p.PositionAge.xyz=(" + value + ").xyz;";
                        break;
                    case Attribute::Velocity:
                        m_Code += "p.VelocityLifetime.xyz=(" + value + ").xyz;";
                        break;
                    case Attribute::Color:
                        m_Code += "p.Color=" + value + ";";
                        break;
                    case Attribute::Size:
                        m_Code += "p.SizeInitialSeed.x=max(0,(" + value + ").x);";
                        break;
                    case Attribute::Lifetime:
                        m_Code += "p.VelocityLifetime.w=max(1.0/60.0,(" + value + ").x);";
                        break;
                    default:
                        diagnostic = "Unknown GPU custom graph output attribute.";
                        return false;
                }
                m_Code += "}\n";
                hlsl = "{\n" + m_Code + "}\n";
                diagnostic.clear();
                return true;
            }

          private:
            Value Literal(const std::array<float, 4>& input, int count)
            {
                std::array<std::string, 4> values;
                for (size_t index = 0; index < 4; ++index)
                {
                    if (!std::isfinite(input[index]))
                    {
                        m_Error = "GPU custom graph contains a non-finite constant.";
                        return {};
                    }
                    values[index] = Number(input[index]);
                }
                return {Pack(values), count};
            }

            Value Pin(uint64_t pin, size_t depth)
            {
                const auto* node = m_Module.Graph.FindPinNode(pin);
                if (!node || !node->OwnsOutput(pin) || depth > 128)
                {
                    m_Error = "GPU custom graph contains an invalid output or exceeds the depth limit.";
                    return {};
                }
                Value result = Node(*node, depth + 1);
                const int component = m_Module.Graph.OutputComponent(pin);
                if (component >= 0)
                {
                    result = {"float4(" + RawComponent(result, component) + ",0,0,0)", 1};
                }
                return result;
            }

            Value Input(const MaterialGraphNode& node, size_t index, size_t depth)
            {
                if (index >= node.InputPins.size())
                {
                    return {};
                }
                if (const auto* link = m_Module.Graph.InputLink(node.InputPins[index]))
                {
                    return Pin(link->SourcePin, depth);
                }
                const auto& definition = MaterialGraph::Definition(node.Kind);
                if (index >= definition.Inputs.size())
                {
                    m_Error = "GPU custom graph has an invalid input layout.";
                    return {};
                }
                return Literal(MaterialGraph::UnconnectedInputValue(node, index),
                               (std::max)(1, static_cast<int>(definition.Inputs[index].Type)));
            }

            Value Parameter(const MaterialGraphNode& node, int count)
            {
                const auto scalar = [](const std::string& expression) {
                    return Value{"float4(" + expression + ",0,0,0)", 1};
                };
                if (node.Name == "Particles.Position")
                {
                    return {"float4(p.PositionAge.xyz,0)", 3};
                }
                if (node.Name == "Particles.Velocity")
                {
                    return {"float4(p.VelocityLifetime.xyz,0)", 3};
                }
                if (node.Name == "Particles.Color")
                {
                    return {"p.Color", 4};
                }
                if (node.Name == "Particles.Age")
                {
                    return scalar("p.PositionAge.w");
                }
                if (node.Name == "Particles.NormalizedAge")
                {
                    // CPU exposes the unclamped ratio to graph parameters.
                    return scalar("p.PositionAge.w/p.VelocityLifetime.w");
                }
                if (node.Name == "Particles.Size")
                {
                    return scalar("p.SizeInitialSeed.x");
                }
                if (node.Name == "Particles.Lifetime")
                {
                    return scalar("p.VelocityLifetime.w");
                }
                if (node.Name == "System.Time")
                {
                    return scalar("Time");
                }
                for (size_t index = 0; index < m_System.Parameters.size(); ++index)
                {
                    if (m_System.Parameters[index].Name != node.Name)
                    {
                        continue;
                    }
                    // Values are uploaded every simulation step. Keeping only the stable
                    // parameter index in the source avoids recompiling animated uniforms.
                    const std::string parameter = "NiagaraParameters[" + std::to_string(index) + "]";
                    switch (count)
                    {
                        case 1:
                            return scalar(parameter + ".x");
                        case 2:
                            return {"float4(" + parameter + ".xy,0,0)", 2};
                        case 3:
                            return {"float4(" + parameter + ".xyz,0)", 3};
                        case 4:
                            return {parameter, 4};
                        default:
                            m_Error = "GPU custom graph parameter has an invalid type: " + node.Name;
                            return {};
                    }
                }
                m_Error = "GPU custom graph parameter is undefined: " + node.Name;
                return {};
            }

            std::string Operation(Kind kind, const std::string& x, const std::string& y, const std::string& z)
            {
                if (MaterialGraphMath::IsUnary(kind))
                {
                    if (kind == Kind::Round)
                    {
                        // std::round uses ties away from zero; HLSL round uses ties to even.
                        return "(sign(" + x + ")*floor(abs(" + x + ")+0.5))";
                    }
                    return MaterialGraphMath::UnaryHlsl(kind, x);
                }
                if (MaterialGraphMath::IsBinary(kind))
                {
                    if (kind == Kind::Fmod)
                    {
                        return "(" + y + "==0?0:fmod(" + x + "," + y + "))";
                    }
                    if (kind == Kind::Power)
                    {
                        return "(" + y + "==0?1:(" + x + "<0?(trunc(" + y + ")==" + y + "?pow(abs(" + x + ")," + y +
                               ")*(fmod(abs(" + y + "),2)==0?1:-1):asfloat(0x7fc00000u)):pow(" + x + "," + y + ")))";
                    }
                    if (kind == Kind::Arctangent2)
                    {
                        return "(" + x + "==0&&" + y + "==0?0:atan2(" + x + "," + y + "))";
                    }
                    return MaterialGraphMath::BinaryHlsl(kind, x, y);
                }
                switch (kind)
                {
                    case Kind::Add:
                        return "(" + x + "+" + y + ")";
                    case Kind::Subtract:
                        return "(" + x + "-" + y + ")";
                    case Kind::Multiply:
                        return "(" + x + "*" + y + ")";
                    case Kind::Divide:
                        return "(abs(" + y + ")>1e-8?" + x + "/" + y + ":0)";
                    case Kind::InverseLerp:
                        return "(abs(" + z + "-" + y + ")>1e-8?(" + x + "-" + y + ")/(" + z + "-" + y + "):0)";
                    case Kind::Lerp:
                        return "(" + x + "+(" + y + "-" + x + ")*" + z + ")";
                    case Kind::Clamp:
                        return "min(max(" + x + "," + y + ")," + z + ")";
                    case Kind::Smoothstep: {
                        const auto fraction = "saturate((" + z + "-" + x + ")/max(" + y + "-" + x + ",1e-8))";
                        return "(" + fraction + "*" + fraction + "*(3-2*" + fraction + "))";
                    }
                    default:
                        return x;
                }
            }

            Value Node(const MaterialGraphNode& node, size_t depth)
            {
                if (const auto found = m_Cache.find(node.Id); found != m_Cache.end())
                {
                    return found->second;
                }
                if (!m_Visiting.insert(node.Id).second)
                {
                    m_Error = "GPU custom graph contains a cycle.";
                    return {};
                }
                if (!SupportsGraphNode(node.Kind) || node.Kind == Kind::Output || node.Kind == Kind::Group)
                {
                    m_Error = "GPU custom graph has an unsupported numeric node: " +
                              std::string(MaterialGraph::Definition(node.Kind).Name);
                    return {};
                }
                int count = std::clamp(static_cast<int>(m_Module.Graph.ResolveOutputType(node.Id)), 1, 4);
                Value result = Literal(node.Value, count);
                if (node.Kind == Kind::ScalarParameter || node.Kind == Kind::Vector2Parameter ||
                    node.Kind == Kind::VectorParameter || node.Kind == Kind::ColorParameter)
                {
                    result = Parameter(node, count);
                }
                else if (MaterialGraphMath::IsConstantNode(node.Kind))
                {
                    std::array<std::string, 4> components;
                    for (size_t index = 0; index < 4; ++index)
                    {
                        components[index] = Number(node.Value[index]);
                        if (index < node.InputPins.size())
                        {
                            if (const auto* link = m_Module.Graph.InputLink(node.InputPins[index]))
                            {
                                components[index] = RawComponent(Pin(link->SourcePin, depth), 0);
                            }
                        }
                    }
                    result.Expression = Pack(components);
                }
                else if (node.Kind == Kind::Time)
                {
                    result.Expression = "float4(Time,0,0,0)";
                }
                else if (node.Kind == Kind::WorldPosition)
                {
                    result.Expression = "float4(p.PositionAge.xyz,0)";
                }
                else if (node.Kind == Kind::Pi)
                {
                    result.Expression = "float4(3.14159265359,0,0,0)";
                }
                else if (node.Kind == Kind::PerInstanceRandom)
                {
                    const auto name = "ngHash" + std::to_string(m_Next++);
                    m_Code += "uint " + name + "=asuint(p.SizeInitialSeed.w)^" + std::to_string(m_System.Seed) + "u;" +
                              name + "^=" + name + ">>16;" + name + "*=0x7feb352du;" + name + "^=" + name + ">>15;" +
                              name + "*=0x846ca68bu;" + name + "^=" + name + ">>16;\n";
                    result.Expression = "float4(float(" + name + ">>8)/16777216.0,0,0,0)";
                }
                else if (node.Kind == Kind::ReflectVector)
                {
                    const auto incident = Input(node, 0, depth);
                    const auto normal = Input(node, 1, depth);
                    const auto i = "float3(" + Component(incident, 0) + "," + Component(incident, 1) + "," +
                                   Component(incident, 2) + ")";
                    const auto n = "float3(" + Component(normal, 0) + "," + Component(normal, 1) + "," +
                                   Component(normal, 2) + ")";
                    const auto squared = "dot(" + n + "," + n + ")";
                    result = {"float4((" + squared + ">1e-8?" + i + "-2*dot(" + i + "," + n + ")*" + n + "/" + squared +
                                  ":" + i + "),0)",
                              3};
                }
                else
                {
                    const auto a = Input(node, 0, depth);
                    const auto b = Input(node, 1, depth);
                    const auto c = Input(node, 2, depth);
                    count = (std::max)({a.Count, b.Count, c.Count});
                    std::array<std::string, 4> components;
                    std::string sum = "0.0";
                    for (int index = 0; index < 4; ++index)
                    {
                        components[index] = Number(node.Value[index]);
                        if (index >= count)
                        {
                            continue;
                        }
                        const auto x = Component(a, index);
                        const auto y = Component(b, index);
                        components[index] = Operation(node.Kind, x, y, Component(c, index));
                        std::string term = "(" + x + "*" + x + ")";
                        if (node.Kind == Kind::Dot)
                        {
                            term = "(" + x + "*" + y + ")";
                        }
                        else if (node.Kind == Kind::Distance)
                        {
                            term = "((" + x + "-" + y + ")*(" + x + "-" + y + "))";
                        }
                        sum += "+" + term;
                    }
                    if (node.Kind == Kind::Dot || node.Kind == Kind::Length || node.Kind == Kind::Distance)
                    {
                        if (node.Kind != Kind::Dot)
                        {
                            sum = "sqrt(" + sum + ")";
                        }
                        result = {"float4(" + sum + ",0,0,0)", 1};
                    }
                    else if (node.Kind == Kind::CrossProduct)
                    {
                        result = {"float4(cross((" + a.Expression + ").xyz,(" + b.Expression + ").xyz),0)", 3};
                    }
                    else
                    {
                        if (node.Kind == Kind::Normalize)
                        {
                            const auto length = "sqrt(" + sum + ")";
                            for (int index = 0; index < count; ++index)
                            {
                                components[index] =
                                    "(" + length + ">1e-8?" + RawComponent(a, index) + "/" + length + ":0)";
                            }
                        }
                        result = {Pack(components), count};
                    }
                }
                const std::string name = "ng" + std::to_string(m_Next++);
                m_Code += "float4 " + name + "=" + result.Expression + ";\n";
                result.Expression = name;
                m_Visiting.erase(node.Id);
                m_Cache.emplace(node.Id, result);
                return result;
            }

            const Module& m_Module;
            const System& m_System;
            std::string m_Code;
            std::string m_Error;
            std::unordered_map<uint64_t, Value> m_Cache;
            std::unordered_set<uint64_t> m_Visiting;
            unsigned int m_Next = 0;
        };
    }

    bool GenerateGPUGraph(const Module& module, const System& system, std::string& hlsl, std::string& diagnostic)
    {
        Generator generator(module, system);
        return generator.Generate(hlsl, diagnostic);
    }
}
