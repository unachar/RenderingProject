#include "pch.h"
#include "niagara.h"
#include "niagaragpu.h"
#include "../assets/materialgraphmath.h"
#include <json/json.h>
#include <cmath>
#include <chrono>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace Niagara
{
    namespace
    {
        constexpr float StepSeconds = 1.0f / 60.0f;
        constexpr size_t MaxEmitters = 128, MaxModules = 128, MaxParameters = 1024;
        constexpr uint32_t MaxParticleCount = 200000;
        constexpr uint64_t MaxIdentity = uint64_t(1) << 52;
        constexpr uint64_t MaxSteps = 60 * 60 * 24;
        using Vector = std::array<float, 4>;
        using Kind = MaterialNodeKind;

        int Components(ParameterType type)
        {
            if (type == ParameterType::Vector2)
            {
                return 2;
            }
            if (type == ParameterType::Vector3)
            {
                return 3;
            }
            if (type == ParameterType::Vector4 || type == ParameterType::Color)
            {
                return 4;
            }
            return 1;
        }

        bool Finite(const Vector& value)
        {
            return std::all_of(value.begin(), value.end(), [](float v) {
                return std::isfinite(v) && std::abs(v) <= 1e7f;
            });
        }

        bool Int32Value(float value)
        {
            const double widened = value;
            return std::isfinite(value) && std::trunc(value) == value &&
                   widened >= static_cast<double>((std::numeric_limits<int32_t>::min)()) &&
                   widened <= static_cast<double>((std::numeric_limits<int32_t>::max)());
        }

        const Parameter* FindParameter(const System& system, const std::string& name)
        {
            for (const auto& parameter : system.Parameters)
            {
                if (parameter.Name == name)
                {
                    return &parameter;
                }
            }
            return nullptr;
        }

        int ModuleComponents(const Module& module)
        {
            switch (module.Kind)
            {
                case ModuleKind::Initialize:
                case ModuleKind::ColorOverLife:
                case ModuleKind::ConeVelocity:
                case ModuleKind::PointAttraction:
                case ModuleKind::VortexForce:
                case ModuleKind::RenderRadialWave:
                case ModuleKind::RenderSpeedColor:
                    return 4;
                case ModuleKind::Velocity:
                case ModuleKind::Gravity:
                case ModuleKind::Noise:
                    return 3;
                default:
                    return 1;
            }
        }

        int AttributeComponents(Attribute attribute)
        {
            if (attribute == Attribute::Position || attribute == Attribute::Velocity)
            {
                return 3;
            }
            return attribute == Attribute::Color ? 4 : 1;
        }

        int ParticleInputComponents(const std::string& name)
        {
            if (name == "Particles.Position" || name == "Particles.Velocity")
            {
                return 3;
            }
            if (name == "Particles.Color")
            {
                return 4;
            }
            if (name == "Particles.Age" || name == "Particles.NormalizedAge" || name == "Particles.Size" ||
                name == "Particles.Lifetime" || name == "System.Time")
            {
                return 1;
            }
            return 0;
        }

        bool ParameterNode(Kind kind)
        {
            return kind == Kind::ScalarParameter || kind == Kind::VectorParameter || kind == Kind::ColorParameter ||
                   kind == Kind::Vector2Parameter;
        }

        bool SupportedNode(Kind kind)
        {
            if (MaterialGraphMath::IsUnary(kind))
            {
                return kind != Kind::DDX && kind != Kind::DDY;
            }
            if (MaterialGraphMath::IsBinary(kind) || MaterialGraphMath::IsConstantNode(kind))
            {
                return true;
            }
            switch (kind)
            {
                case Kind::Add:
                case Kind::Subtract:
                case Kind::Multiply:
                case Kind::Divide:
                case Kind::Lerp:
                case Kind::Clamp:
                case Kind::Smoothstep:
                case Kind::Normalize:
                case Kind::Dot:
                case Kind::Length:
                case Kind::Distance:
                case Kind::CrossProduct:
                case Kind::ReflectVector:
                case Kind::InverseLerp:
                case Kind::Reroute:
                case Kind::Time:
                case Kind::Pi:
                case Kind::Output:
                case Kind::Group:
                case Kind::WorldPosition:
                case Kind::PerInstanceRandom:
                    return true;
                default:
                    return false;
            }
        }

        Vector CurveValue(const Module& module, float time, Vector fallback)
        {
            if (module.Curve.empty())
            {
                return fallback;
            }
            if (time <= module.Curve.front().Time)
            {
                return module.Curve.front().Value;
            }
            for (size_t i = 1; i < module.Curve.size(); ++i)
            {
                const auto& a = module.Curve[i - 1];
                const auto& b = module.Curve[i];
                if (time <= b.Time)
                {
                    const float t = (time - a.Time) / (b.Time - a.Time);
                    for (size_t c = 0; c < 4; ++c)
                    {
                        fallback[c] = a.Value[c] + (b.Value[c] - a.Value[c]) * t;
                    }
                    return fallback;
                }
            }
            return module.Curve.back().Value;
        }

        Vector ParticleInput(const std::string& name, const Particle& particle, float time)
        {
            if (name == "Particles.Position")
            {
                return {particle.Position.x, particle.Position.y, particle.Position.z, 0};
            }
            if (name == "Particles.Velocity")
            {
                return {particle.Velocity.x, particle.Velocity.y, particle.Velocity.z, 0};
            }
            if (name == "Particles.Color")
            {
                return {particle.Color.x, particle.Color.y, particle.Color.z, particle.Color.w};
            }
            if (name == "Particles.Age")
            {
                return {particle.Age, 0, 0, 0};
            }
            if (name == "Particles.NormalizedAge")
            {
                return {particle.Age / particle.Lifetime, 0, 0, 0};
            }
            if (name == "Particles.Size")
            {
                return {particle.Size, 0, 0, 0};
            }
            if (name == "Particles.Lifetime")
            {
                return {particle.Lifetime, 0, 0, 0};
            }
            return {time, 0, 0, 0};
        }

        struct GraphValue
        {
            Vector Value{};
            int Count = 1;
        };

        // Graph validation limits depth and rejects cycles before this dependency evaluation.
        class GraphEvaluation
        {
          public:
            GraphEvaluation(const Module& module, const System& system, const Particle& particle, float time,
                            const std::vector<Parameter>* emitterParameters = nullptr)
                : m_Graph(module.Graph), m_System(system), m_Particle(particle), m_Time(time),
                  m_EmitterParameters(emitterParameters)
            {
            }

            GraphValue Pin(uint64_t pin, size_t depth = 0)
            {
                const auto* node = m_Graph.FindPinNode(pin);
                if (!node || depth > 128)
                {
                    return {};
                }
                GraphValue value = Node(*node, depth + 1);
                const int component = m_Graph.OutputComponent(pin);
                if (component >= 0)
                {
                    return {{value.Value[component], 0, 0, 0}, 1};
                }
                return value;
            }

          private:
            GraphValue Input(const MaterialGraphNode& node, size_t input, size_t depth)
            {
                if (input >= node.InputPins.size())
                {
                    return {};
                }
                if (const auto* link = m_Graph.InputLink(node.InputPins[input]))
                {
                    return Pin(link->SourcePin, depth);
                }
                const auto& definition = MaterialGraph::Definition(node.Kind);
                return {MaterialGraph::UnconnectedInputValue(node, input),
                        (std::max)(1, static_cast<int>(definition.Inputs[input].Type))};
            }

            static float Component(const GraphValue& value, int index)
            {
                return value.Value[value.Count == 1 ? 0 : (std::min)(index, value.Count - 1)];
            }

            GraphValue Node(const MaterialGraphNode& node, size_t depth)
            {
                if (auto found = m_Cache.find(node.Id); found != m_Cache.end())
                {
                    return found->second;
                }
                GraphValue result{node.Value, (std::max)(1, static_cast<int>(m_Graph.ResolveOutputType(node.Id)))};
                if (ParameterNode(node.Kind))
                {
                    if (const auto* parameter = FindParameter(m_System, node.Name))
                    {
                        result.Value = parameter->Value;
                        if (parameter->Scope == ParameterScope::Emitter && m_EmitterParameters)
                        {
                            for (const auto& scoped : *m_EmitterParameters)
                            {
                                if (scoped.Name == node.Name)
                                {
                                    result.Value = scoped.Value;
                                    break;
                                }
                            }
                        }
                    }
                    else
                    {
                        result.Value = ParticleInput(node.Name, m_Particle, m_Time);
                    }
                }
                else if (MaterialGraphMath::IsConstantNode(node.Kind))
                {
                    for (size_t i = 0; i < node.InputPins.size() && i < 4; ++i)
                    {
                        if (const auto* link = m_Graph.InputLink(node.InputPins[i]))
                        {
                            result.Value[i] = Pin(link->SourcePin, depth).Value[0];
                        }
                    }
                }
                else if (node.Kind == Kind::Time)
                {
                    result.Value = {m_Time, 0, 0, 0};
                }
                else if (node.Kind == Kind::WorldPosition)
                {
                    result.Value = {m_Particle.Position.x, m_Particle.Position.y, m_Particle.Position.z, 0};
                }
                else if (node.Kind == Kind::PerInstanceRandom)
                {
                    uint32_t hash = static_cast<uint32_t>(m_Particle.Id) ^ m_System.Seed;
                    hash ^= hash >> 16;
                    hash *= 0x7feb352du;
                    hash ^= hash >> 15;
                    hash *= 0x846ca68bu;
                    hash ^= hash >> 16;
                    result.Value = {float(hash >> 8) / 16777216.0f, 0, 0, 0};
                }
                else if (node.Kind == Kind::Pi)
                {
                    result.Value = {3.14159265359f, 0, 0, 0};
                }
                else if (node.Kind == Kind::ReflectVector)
                {
                    const auto incident = Input(node, 0, depth);
                    const auto normal = Input(node, 1, depth);
                    float dot = 0, normalSquared = 0;
                    for (int component = 0; component < 3; ++component)
                    {
                        dot += Component(incident, component) * Component(normal, component);
                        normalSquared += Component(normal, component) * Component(normal, component);
                    }
                    const float scale = normalSquared > 1e-8f ? 2.0f * dot / normalSquared : 0.0f;
                    result.Count = 3;
                    for (int component = 0; component < 3; ++component)
                    {
                        result.Value[component] = Component(incident, component) - scale * Component(normal, component);
                    }
                }
                else
                {
                    const auto a = Input(node, 0, depth), b = Input(node, 1, depth), c = Input(node, 2, depth);
                    result.Count = (std::max)({a.Count, b.Count, c.Count});
                    float sum = 0;
                    for (int i = 0; i < result.Count; ++i)
                    {
                        const float x = Component(a, i), y = Component(b, i), z = Component(c, i);
                        float value = x;
                        if (MaterialGraphMath::IsUnary(node.Kind))
                        {
                            value = MaterialGraphMath::Unary(node.Kind, x);
                        }
                        else if (MaterialGraphMath::IsBinary(node.Kind))
                        {
                            value = MaterialGraphMath::Binary(node.Kind, x, y);
                        }
                        else
                        {
                            switch (node.Kind)
                            {
                                case Kind::Add:
                                    value = x + y;
                                    break;
                                case Kind::Subtract:
                                    value = x - y;
                                    break;
                                case Kind::Multiply:
                                    value = x * y;
                                    break;
                                case Kind::Divide:
                                    value = std::abs(y) > 1e-8f ? x / y : 0;
                                    break;
                                case Kind::InverseLerp:
                                    value = std::abs(z - y) > 1e-8f ? (x - y) / (z - y) : 0;
                                    break;
                                case Kind::Lerp:
                                    value = x + (y - x) * z;
                                    break;
                                case Kind::Clamp:
                                    value = (std::min)((std::max)(x, y), z);
                                    break;
                                case Kind::Smoothstep: {
                                    const float t = std::clamp((z - x) / (std::max)(y - x, 1e-8f), 0.0f, 1.0f);
                                    value = t * t * (3 - 2 * t);
                                    break;
                                }
                                default:
                                    break;
                            }
                        }
                        result.Value[i] = value;
                        if (node.Kind == Kind::Dot)
                        {
                            sum += x * y;
                        }
                        else if (node.Kind == Kind::Distance)
                        {
                            sum += (x - y) * (x - y);
                        }
                        else
                        {
                            sum += x * x;
                        }
                    }
                    if (node.Kind == Kind::Dot || node.Kind == Kind::Length || node.Kind == Kind::Distance)
                    {
                        result = {{node.Kind == Kind::Dot ? sum : std::sqrt(sum), 0, 0, 0}, 1};
                    }
                    else if (node.Kind == Kind::Normalize)
                    {
                        const float length = std::sqrt(sum);
                        for (int i = 0; i < result.Count; ++i)
                        {
                            result.Value[i] = length > 1e-8f ? a.Value[i] / length : 0;
                        }
                    }
                    else if (node.Kind == Kind::CrossProduct)
                    {
                        result = {{a.Value[1] * b.Value[2] - a.Value[2] * b.Value[1],
                                   a.Value[2] * b.Value[0] - a.Value[0] * b.Value[2],
                                   a.Value[0] * b.Value[1] - a.Value[1] * b.Value[0], 0},
                                  3};
                    }
                }
                m_Cache.emplace(node.Id, result);
                return result;
            }

            const MaterialGraph& m_Graph;
            const System& m_System;
            const Particle& m_Particle;
            float m_Time;
            std::unordered_map<uint64_t, GraphValue> m_Cache;
            const std::vector<Parameter>* m_EmitterParameters;
        };
    }

    bool SupportsGraphNode(MaterialNodeKind kind)
    {
        return SupportedNode(kind);
    }

    const char* ModuleName(ModuleKind kind)
    {
        static const char* names[] = {"Spawn Rate",
                                      "Burst",
                                      "Initialize",
                                      "Velocity",
                                      "Gravity",
                                      "Drag",
                                      "Color Over Life",
                                      "Scale Over Life",
                                      "Noise",
                                      "Ground Collision",
                                      "Death Event Spawn",
                                      "Custom Graph",
                                      "Add Velocity in Cone",
                                      "Point Attraction Force",
                                      "Vortex Force",
                                      "Limit Velocity",
                                      "Render Radial Wave",
                                      "Render Speed Color"};
        const auto index = static_cast<size_t>(kind);
        return index < std::size(names) ? names[index] : "Unknown";
    }

    Module MakeModule(ModuleKind kind)
    {
        Module module;
        module.Kind = kind;
        switch (kind)
        {
            case ModuleKind::SpawnRate:
                module.Value[0] = 30;
                break;
            case ModuleKind::Burst:
                module.Value = {20, 0, 0, 0};
                break; // count, emitter-relative time
            case ModuleKind::Initialize:
                module.Value = {1, 0.5f, 0.1f, 1};
                module.Secondary = {2, 0.1f, 0.2f, 0};
                break; // lifetime, size, spawn sphere radius
            case ModuleKind::Velocity:
                module.Value = {0, 2, 0, 0};
                module.Secondary = {1, 1, 1, 0};
                break;
            case ModuleKind::Gravity:
                module.Value = {0, -9.81f, 0, 0};
                break;
            case ModuleKind::Drag:
                module.Value[0] = 0.2f;
                break;
            case ModuleKind::ColorOverLife:
                module.Value = {1, 1, 1, 1};
                module.Secondary = {1, 0.1f, 0, 0};
                break;
            case ModuleKind::ScaleOverLife:
                module.Value[0] = 1;
                module.Secondary[0] = 0;
                break;
            case ModuleKind::Noise:
                module.Value = {1, 1, 1, 0};
                module.Secondary[0] = 2;
                break;
            case ModuleKind::GroundCollision:
                module.Value = {0, 0.5f, 0, 0};
                break; // ground height, restitution
            case ModuleKind::DeathEventSpawn:
                module.Value[0] = 1;
                break; // count; Secondary[0] target emitter index
            case ModuleKind::ConeVelocity:
                module.Value = {0, 1, 0, 3};
                module.Secondary = {25, 0.5f, 0, 0};
                break;
            case ModuleKind::PointAttraction:
                module.Value = {0, 1, 0, 5};
                module.Secondary = {5, 1, 0, 0};
                break;
            case ModuleKind::VortexForce:
                module.Value = {0, 1, 0, 5};
                module.Secondary = {0, 0, 0, 5};
                break;
            case ModuleKind::LimitVelocity:
                module.Value[0] = 5;
                break;
            case ModuleKind::RenderRadialWave:
                module.Value = {0, 0, 0, 4};
                module.Secondary = {1, 0.5f, 0, 5};
                break;
            case ModuleKind::RenderSpeedColor:
                module.Value = {0.1f, 0.4f, 1, 0};
                module.Secondary = {1, 0.2f, 0.05f, 5};
                break;
            case ModuleKind::CustomGraph: {
                module.Graph.AddNode(Kind::Output, 300, 0);
                const auto id = module.Graph.AddNode(Kind::Constant3Vector, 0, 0);
                module.OutputPin = module.Graph.FindNode(id)->OutputPin;
                break;
            }
            default:
                break;
        }
        return module;
    }

    uint64_t NextId(const System& system)
    {
        uint64_t next = 1;
        const auto include = [&](uint64_t id) {
            if (id >= MaxIdentity)
            {
                return false;
            }
            next = (std::max)(next, id + 1);
            return true;
        };
        for (const auto& module : system.Modules)
        {
            if (!include(module.Id))
            {
                return 0;
            }
        }
        for (const auto& emitter : system.Emitters)
        {
            if (!include(emitter.Id))
            {
                return 0;
            }
            for (const auto& module : emitter.Modules)
            {
                if (!include(module.Id))
                {
                    return 0;
                }
            }
        }
        return next;
    }

    System MakeDefaultSystem()
    {
        System system;
        Emitter emitter;
        emitter.Id = 1;
        for (auto kind : {ModuleKind::SpawnRate, ModuleKind::Initialize, ModuleKind::Velocity, ModuleKind::Gravity,
                          ModuleKind::ColorOverLife, ModuleKind::ScaleOverLife})
        {
            auto module = MakeModule(kind);
            module.Id = emitter.Modules.size() + 2;
            emitter.Modules.push_back(std::move(module));
        }
        system.Emitters.push_back(std::move(emitter));
        return system;
    }

    bool Validate(const System& system, std::string& diagnostic)
    {
        diagnostic.clear();
        const auto fail = [&](const std::string& message) {
            diagnostic = message;
            return false;
        };
        if (system.Name.size() > 1024 || !std::isfinite(system.Duration) || system.Duration < StepSeconds ||
            system.Duration > 86400 || system.Emitters.size() > MaxEmitters ||
            system.Parameters.size() > MaxParameters || system.Modules.size() > MaxModules)
        {
            return fail("Invalid system duration, name or collection limit.");
        }
        std::unordered_set<std::string> names;
        for (const auto& parameter : system.Parameters)
        {
            if (parameter.Name.empty() || parameter.Name.size() > 256 || !names.insert(parameter.Name).second ||
                static_cast<int>(parameter.Type) < 0 ||
                static_cast<int>(parameter.Type) > static_cast<int>(ParameterType::Color) ||
                static_cast<int>(parameter.Scope) < 0 ||
                static_cast<int>(parameter.Scope) > static_cast<int>(ParameterScope::Particles) ||
                !Finite(parameter.Value))
            {
                return fail("Invalid or duplicate parameter: " + parameter.Name);
            }
            if (ParticleInputComponents(parameter.Name))
            {
                return fail("Reserved runtime parameter name: " + parameter.Name);
            }
            if (parameter.Type == ParameterType::Bool && parameter.Value[0] != 0 && parameter.Value[0] != 1)
            {
                return fail("Bool parameter must be 0 or 1: " + parameter.Name);
            }
            if (parameter.Type == ParameterType::Int && !Int32Value(parameter.Value[0]))
            {
                return fail("Int parameter must be an integer within int32 range: " + parameter.Name);
            }
        }
        std::unordered_set<uint64_t> ids;
        const auto validateModule = [&](const Module& module, const Emitter* owner) {
            const std::string ownerName = owner ? owner->Name : system.Name;
            const bool lifecycle = module.ExecutionStage >= Stage::SystemSpawn;
            const bool systemStage =
                module.ExecutionStage == Stage::SystemSpawn || module.ExecutionStage == Stage::SystemUpdate;
            if (!owner && (module.Kind != ModuleKind::CustomGraph || !systemStage))
            {
                return fail("System stack accepts Custom Graph modules in System Spawn/Update stages.");
            }
            if (owner && module.Kind == ModuleKind::CustomGraph && systemStage)
            {
                return fail("System lifecycle graphs must be placed in the system stack.");
            }

            if (!module.Id || module.Id > MaxIdentity || !ids.insert(module.Id).second ||
                static_cast<int>(module.Kind) < 0 || module.Kind >= ModuleKind::Count || !Finite(module.Value) ||
                !Finite(module.Secondary) || module.ParameterName.size() > 256 || module.Curve.size() > 1024 ||
                static_cast<int>(module.Target) < 0 || static_cast<int>(module.Target) > 4 ||
                module.WriteParameter.size() > 256 || static_cast<int>(module.ExecutionStage) < 0 ||
                static_cast<int>(module.ExecutionStage) > static_cast<int>(Stage::EmitterUpdate))
            {
                return fail("Invalid module in " + ownerName);
            }
            Vector value = module.Value;
            if (!module.ParameterName.empty())
            {
                const auto* parameter = FindParameter(system, module.ParameterName);
                if (!parameter || Components(parameter->Type) != ModuleComponents(module) ||
                    parameter->Type == ParameterType::Bool)
                {
                    return fail("Missing or incompatible module parameter: " + module.ParameterName);
                }
                value = parameter->Value;
            }
            if ((module.Kind == ModuleKind::SpawnRate || module.Kind == ModuleKind::Burst ||
                 module.Kind == ModuleKind::Drag || module.Kind == ModuleKind::ScaleOverLife ||
                 module.Kind == ModuleKind::DeathEventSpawn) &&
                value[0] < 0)
            {
                return fail("Negative count, rate, drag or scale in " + ownerName);
            }
            if ((module.Kind == ModuleKind::Burst || module.Kind == ModuleKind::DeathEventSpawn) &&
                (value[0] > MaxParticleCount || std::trunc(value[0]) != value[0]))
            {
                return fail("Spawn count must be an integer within capacity.");
            }
            if (module.Kind == ModuleKind::SpawnRate && value[0] > 1000000)
            {
                return fail("Spawn rate exceeds 1000000 per second.");
            }
            if (module.Kind == ModuleKind::Initialize &&
                (module.Secondary[0] <= 0 || module.Secondary[1] < 0 || module.Secondary[2] < 0))
            {
                return fail("Initialize needs positive lifetime and nonnegative size and radius.");
            }
            if (module.Kind == ModuleKind::GroundCollision && (value[1] < 0 || value[1] > 1))
            {
                return fail("Ground restitution must be between 0 and 1.");
            }
            if (module.Kind == ModuleKind::Burst && (value[1] < 0 || value[1] > owner->End - owner->Start))
            {
                return fail("Burst time is outside emitter interval.");
            }
            if (module.Kind == ModuleKind::DeathEventSpawn &&
                (module.Secondary[0] < 0 || module.Secondary[0] >= system.Emitters.size() ||
                 std::trunc(module.Secondary[0]) != module.Secondary[0]))
            {
                return fail("Death event target emitter index is invalid.");
            }
            const float axisSquared = value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
            if ((module.Kind == ModuleKind::ConeVelocity || module.Kind == ModuleKind::VortexForce) &&
                axisSquared < 1e-8f)
            {
                return fail("Cone and vortex axes must be nonzero in " + ownerName);
            }
            if (module.Kind == ModuleKind::ConeVelocity &&
                (value[3] < 0 || module.Secondary[0] < 0 || module.Secondary[0] > 180 || module.Secondary[1] < 0))
            {
                return fail("Cone needs nonnegative speed/spread and an angle within 0..180 degrees.");
            }
            if ((module.Kind == ModuleKind::PointAttraction && (module.Secondary[0] <= 0 || module.Secondary[1] < 0)) ||
                (module.Kind == ModuleKind::VortexForce && module.Secondary[3] <= 0) ||
                (module.Kind == ModuleKind::RenderRadialWave && (module.Secondary[3] <= 0 || module.Secondary[1] < 0)))
            {
                return fail("Force modules need a positive radius and nonnegative falloff/frequency.");
            }
            if (module.Kind == ModuleKind::LimitVelocity && value[0] < 0)
            {
                return fail("Velocity limit must be nonnegative.");
            }
            if (module.Kind == ModuleKind::RenderSpeedColor && (value[3] < 0 || module.Secondary[3] <= value[3]))
            {
                return fail("Speed Color needs 0 <= minimum speed < maximum speed.");
            }
            float lastTime = -1;
            for (const auto& key : module.Curve)
            {
                if (!std::isfinite(key.Time) || key.Time < 0 || key.Time > 1 || key.Time <= lastTime ||
                    !Finite(key.Value))
                {
                    return fail("Curve keys must be finite, ordered and within normalized lifetime 0..1.");
                }
                lastTime = key.Time;
            }
            if (!module.Curve.empty() && module.Kind != ModuleKind::ColorOverLife &&
                module.Kind != ModuleKind::ScaleOverLife)
            {
                return fail("Curves currently apply to color and scale over life only.");
            }
            if (module.Kind != ModuleKind::CustomGraph)
            {
                return true;
            }
            std::string graphError;
            if (module.Graph.Nodes.size() > 128 || module.Graph.Links.size() > 512 ||
                !module.Graph.Validate(graphError))
            {
                return fail("Custom graph: " + graphError);
            }
            const auto* output = module.Graph.FindPinNode(module.OutputPin);
            const Parameter* target = lifecycle ? FindParameter(system, module.WriteParameter) : nullptr;
            if (lifecycle && (!target || target->Type == ParameterType::Bool ||
                              target->Scope != (systemStage ? ParameterScope::System : ParameterScope::Emitter)))
            {
                return fail("Lifecycle graph requires an existing writable parameter in its System or Emitter scope.");
            }
            if (!output || !output->OwnsOutput(module.OutputPin) ||
                static_cast<int>(module.Graph.PinOutputType(module.OutputPin)) !=
                    (lifecycle ? Components(target->Type) : AttributeComponents(module.Target)))
            {
                return fail("Custom graph output type does not match target attribute.");
            }
            if (!lifecycle && module.Target == Attribute::Lifetime && module.ExecutionStage != Stage::ParticleSpawn)
            {
                return fail("Lifetime may only be written during Particle Spawn.");
            }
            for (const auto& node : module.Graph.Nodes)
            {
                if (lifecycle && (node.Kind == Kind::WorldPosition || node.Kind == Kind::PerInstanceRandom ||
                                  (ParameterNode(node.Kind) && node.Name.starts_with("Particles."))))
                {
                    return fail("Particle attributes are unavailable in System/Emitter lifecycle graphs.");
                }
                if (!SupportedNode(node.Kind))
                {
                    return fail("Unsupported particle graph node: " +
                                std::string(MaterialGraph::Definition(node.Kind).Name));
                }
                if (node.Kind == Kind::Output)
                {
                    for (const auto pin : node.InputPins)
                    {
                        if (module.Graph.RawInputLink(pin))
                        {
                            return fail(
                                "Material surface output inputs are unavailable in particle graphs; select a numeric output pin.");
                        }
                    }
                }
                if (ParameterNode(node.Kind))
                {
                    const auto* parameter = FindParameter(system, node.Name);
                    if (systemStage && parameter && parameter->Scope == ParameterScope::Emitter)
                    {
                        return fail("System graphs cannot read per-emitter scoped parameters.");
                    }
                    const int components = parameter ? Components(parameter->Type) : ParticleInputComponents(node.Name);
                    if (!components || components != static_cast<int>(module.Graph.ResolveOutputType(node.Id)))
                    {
                        return fail("Missing or incompatible graph parameter at node " + std::to_string(node.Id) +
                                    ": " + node.Name);
                    }
                }
            }

            return true;
        };
        for (const auto& module : system.Modules)
        {
            if (!validateModule(module, nullptr))
            {
                return false;
            }
        }
        uint64_t capacity = 0;
        for (const auto& emitter : system.Emitters)
        {
            if (!emitter.Id || emitter.Id > MaxIdentity || !ids.insert(emitter.Id).second ||
                emitter.Name.size() > 1024 || !std::isfinite(emitter.Position[0]) ||
                !std::isfinite(emitter.Position[1]) || !std::isfinite(emitter.Start) || !std::isfinite(emitter.End) ||
                emitter.Start < 0 || emitter.End < emitter.Start || emitter.End > system.Duration ||
                !emitter.MaxParticles || emitter.MaxParticles > MaxParticleCount || emitter.Modules.size() > MaxModules)
            {
                return fail("Invalid emitter: " + emitter.Name);
            }
            capacity += emitter.MaxParticles;
            if (capacity > MaxParticleCount)
            {
                return fail("Total emitter capacity exceeds 200000 particles.");
            }
            if (emitter.Simulation != Backend::CPU && emitter.Simulation != Backend::GPU)
            {
                return fail("Unknown simulation backend.");
            }
            if (emitter.Simulation == Backend::GPU && !SupportsGPU(emitter, diagnostic))
            {
                return false;
            }
            if (static_cast<int>(emitter.Renderer) < 0 || static_cast<int>(emitter.Renderer) > 2)
            {
                return fail("Unknown particle renderer.");
            }
            for (const auto& module : emitter.Modules)
            {
                if (!validateModule(module, &emitter))
                {
                    return false;
                }
            }
        }
        return true;
    }

    const Parameter* Runtime::RuntimeParameter(const std::string& name, size_t emitter) const
    {
        const auto* parameter = FindParameter(m_System, name);
        if (parameter && parameter->Scope == ParameterScope::Emitter && emitter < m_EmitterParameters.size())
        {
            for (const auto& scoped : m_EmitterParameters[emitter])
            {
                if (scoped.Name == name)
                {
                    return &scoped;
                }
            }
        }
        return parameter;
    }

    Vector Runtime::Values(const Module& module, size_t emitter) const
    {
        if (!module.ParameterName.empty())
        {
            if (const auto* parameter = RuntimeParameter(module.ParameterName, emitter))
            {
                return parameter->Value;
            }
        }
        return module.Value;
    }

    void Runtime::ApplyGraph(const Module& module, Particle& particle, size_t emitter)
    {
        GraphEvaluation evaluation(module, m_System, particle, Time(), &m_EmitterParameters[emitter]);
        const auto value = evaluation.Pin(module.OutputPin).Value;
        if (!Finite(value))
        {
            m_Diagnostic = "Non-finite custom graph output; particle removed.";
            particle.Lifetime = 0;
            return;
        }
        switch (module.Target)
        {
            case Attribute::Position:
                particle.Position = {value[0], value[1], value[2]};
                break;
            case Attribute::Velocity:
                particle.Velocity = {value[0], value[1], value[2]};
                break;
            case Attribute::Color:
                particle.Color = {value[0], value[1], value[2], value[3]};
                break;
            case Attribute::Size:
                particle.Size = (std::max)(0.0f, value[0]);
                break;
            case Attribute::Lifetime:
                particle.Lifetime = (std::max)(StepSeconds, value[0]);
                break;
        }
    }

    void Runtime::ExecuteParameterModule(const Module& module, size_t emitter)
    {
        if (m_Failed || !module.Enabled || module.Kind != ModuleKind::CustomGraph)
        {
            return;
        }
        const Particle context;
        GraphEvaluation evaluation(module, m_System, context, Time(),
                                   emitter < m_EmitterParameters.size() ? &m_EmitterParameters[emitter] : nullptr);
        const auto value = evaluation.Pin(module.OutputPin).Value;
        const auto* definition = RuntimeParameter(module.WriteParameter, emitter);
        if (!definition || !Finite(value) || (definition->Type == ParameterType::Int && !Int32Value(value[0])))
        {
            m_Diagnostic = "Lifecycle graph produced an invalid typed value for " + module.WriteParameter;
            m_Failed = true;
            return;
        }
        System candidate = m_System;
        for (auto& parameter : candidate.Parameters)
        {
            if (const auto* current = RuntimeParameter(parameter.Name, emitter))
            {
                parameter.Value = current->Value;
            }
            if (parameter.Name == module.WriteParameter)
            {
                parameter.Value = value;
            }
        }
        if (!Validate(candidate, m_Diagnostic))
        {
            m_Failed = true;
            return;
        }
        auto& parameters = emitter < m_EmitterParameters.size() ? m_EmitterParameters[emitter] : m_System.Parameters;
        for (auto& parameter : parameters)
        {
            if (parameter.Name == module.WriteParameter)
            {
                parameter.Value = value;
                break;
            }
        }
    }

    void Runtime::Reset(const System& system)
    {
        m_Step = m_TargetStep = m_Dropped = 0;
        m_NextParticle = 1;
        m_Accumulator = 0;
        m_Failed = false;
        m_Particles.clear();
        if (!Validate(system, m_Diagnostic))
        {
            m_Failed = true;
            m_System = {};
            m_System.Emitters.clear();
            m_SpawnRemainders.clear();
            m_LiveCounts.clear();
            return;
        }
        m_System = system;
        m_Definition = system;
        m_Random = system.Seed ? system.Seed : 0x9e3779b9u;
        m_SpawnRemainders.assign(system.Emitters.size() * MaxModules, 0);
        m_LiveCounts.assign(system.Emitters.size(), 0);
        m_EmitterParameters.assign(system.Emitters.size(), system.Parameters);
        m_EmitterSpawned.assign(system.Emitters.size(), false);
        for (const auto& module : m_System.Modules)
        {
            if (module.ExecutionStage == Stage::SystemSpawn)
            {
                ExecuteParameterModule(module, m_System.Emitters.size());
            }
        }
    }

    void Runtime::Seek(float time)
    {
        if (!std::isfinite(time))
        {
            return;
        }
        const auto target = static_cast<uint64_t>(std::clamp(double(time) * 60.0 + 0.00001, 0.0, double(MaxSteps)));
        if (target < m_Step)
        {
            const System copy = m_Definition;
            Reset(copy);
        }
        m_TargetStep = target;
        m_Accumulator = 0;
    }

    bool Runtime::UpdateSeek(size_t maxSteps)
    {
        maxSteps = (std::min)(maxSteps, size_t(2400));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
        while (Seeking() && maxSteps--)
        {
            Step();
            if (std::chrono::steady_clock::now() >= deadline)
            {
                break;
            }
        }
        return !m_Failed && m_Step >= m_TargetStep;
    }

    void Runtime::Advance(float seconds)
    {
        if (!std::isfinite(seconds) || seconds <= 0 || Seeking() || m_Failed)
        {
            return;
        }
        m_Accumulator += double(seconds);
        size_t budget = 240;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
        while (m_Accumulator + 1e-8 >= 1.0 / 60 && budget-- && m_Step < MaxSteps && !m_Failed)
        {
            Step();
            m_Accumulator -= 1.0 / 60;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                break;
            }
        }
        m_TargetStep = m_Step;
    }

    void Runtime::Spawn(size_t emitterIndex, uint32_t count, const DirectX::XMFLOAT3* eventPosition)
    {
        const auto& emitter = m_System.Emitters[emitterIndex];
        const uint32_t available = emitter.MaxParticles - m_LiveCounts[emitterIndex];
        const auto accepted = (std::min)(count, available);
        m_LiveCounts[emitterIndex] += accepted;
        m_Dropped += count - accepted;
        const auto random = [&]() {
            m_Random ^= m_Random << 13;
            m_Random ^= m_Random >> 17;
            m_Random ^= m_Random << 5;
            return float(m_Random >> 8) / 16777216.0f;
        };
        for (uint32_t i = 0; i < accepted; ++i)
        {
            Particle particle;
            particle.Id = m_NextParticle++;
            particle.EmitterId = emitter.Id;
            for (const auto& module : emitter.Modules)
            {
                if (!module.Enabled)
                {
                    continue;
                }
                const auto value = Values(module, emitterIndex);
                if (module.Kind == ModuleKind::Initialize)
                {
                    particle.Lifetime = module.Secondary[0];
                    particle.Size = module.Secondary[1];
                    particle.Color = {value[0], value[1], value[2], value[3]};
                    // Uniform sphere direction with a cubic radial distribution.
                    const float z = random() * 2 - 1, angle = random() * DirectX::XM_2PI;
                    const float radius = module.Secondary[2] * std::cbrt(random());
                    const float xy = std::sqrt((std::max)(0.0f, 1 - z * z));
                    particle.Position = {radius * xy * std::cos(angle), radius * z, radius * xy * std::sin(angle)};
                }
                else if (module.Kind == ModuleKind::Velocity)
                {
                    particle.Velocity = {value[0] + (random() * 2 - 1) * module.Secondary[0],
                                         value[1] + (random() * 2 - 1) * module.Secondary[1],
                                         value[2] + (random() * 2 - 1) * module.Secondary[2]};
                }
                else if (module.Kind == ModuleKind::ConeVelocity)
                {
                    const auto axis = XMVector3Normalize(XMVectorSet(value[0], value[1], value[2], 0));
                    const auto reference =
                        std::abs(XMVectorGetY(axis)) < 0.99f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
                    const auto tangent = XMVector3Normalize(XMVector3Cross(reference, axis));
                    const auto bitangent = XMVector3Cross(axis, tangent);
                    const float cosine = std::lerp(std::cos(module.Secondary[0] * XM_PI / 180.0f), 1.0f, random());
                    const float sine = std::sqrt((std::max)(0.0f, 1.0f - cosine * cosine));
                    const float angle = random() * XM_2PI;
                    const float speed = (std::max)(0.0f, value[3] + (random() * 2.0f - 1.0f) * module.Secondary[1]);
                    auto direction = XMVectorAdd(XMVectorScale(axis, cosine),
                                                 XMVectorAdd(XMVectorScale(tangent, sine * std::cos(angle)),
                                                             XMVectorScale(bitangent, sine * std::sin(angle))));
                    // NiagaraのAdd Velocityと同じく、既存の初速度へ加算する。
                    XMStoreFloat3(&particle.Velocity,
                                  XMVectorAdd(XMLoadFloat3(&particle.Velocity), XMVectorScale(direction, speed)));
                }
                else if (module.Kind == ModuleKind::CustomGraph && module.ExecutionStage == Stage::ParticleSpawn)
                {
                    ApplyGraph(module, particle, emitterIndex);
                }
            }
            if (!emitter.LocalSpace)
            {
                const auto transform = DirectX::XMLoadFloat4x4(&m_Transform);
                DirectX::XMStoreFloat3(&particle.Position, DirectX::XMVector3TransformCoord(
                                                               DirectX::XMLoadFloat3(&particle.Position), transform));
                DirectX::XMStoreFloat3(&particle.Velocity, DirectX::XMVector3TransformNormal(
                                                               DirectX::XMLoadFloat3(&particle.Velocity), transform));
            }
            if (eventPosition)
            {
                particle.Position = *eventPosition;
            }
            particle.InitialColor = particle.Color;
            particle.InitialSize = particle.Size;
            m_Particles.push_back(particle);
        }
    }

    void Runtime::Step()
    {
        for (const auto& module : m_System.Modules)
        {
            if (module.ExecutionStage == Stage::SystemUpdate)
            {
                ExecuteParameterModule(module, m_System.Emitters.size());
            }
        }
        if (m_Failed)
        {
            return;
        }
        const bool solo = std::any_of(m_System.Emitters.begin(), m_System.Emitters.end(), [](const Emitter& e) {
            return e.Enabled && e.Solo;
        });
        const uint64_t cycleSteps =
            (std::max)(uint64_t(1), static_cast<uint64_t>(std::llround(m_System.Duration * 60.0)));
        const uint64_t phaseStep = m_System.Loop ? m_Step % cycleSteps : m_Step;
        const double phase = double(phaseStep) / 60;
        const auto active = [&](const Emitter& e) {
            return e.Enabled && (!solo || e.Solo);
        };
        for (size_t ei = 0; ei < m_System.Emitters.size(); ++ei)
        {
            const auto& emitter = m_System.Emitters[ei];
            if (!active(emitter) || (!m_System.Loop && m_Step >= cycleSteps))
            {
                continue;
            }
            const double overlap = (std::max)(0.0, (std::min)(phase + 1.0 / 60, double(emitter.End)) -
                                                       (std::max)(phase, double(emitter.Start)));
            const auto activationStep = static_cast<uint64_t>(std::ceil(emitter.Start * 60.0 - 1e-5));
            if (overlap > 0 || phaseStep == activationStep)
            {
                for (const auto& module : emitter.Modules)
                {
                    if ((!m_EmitterSpawned[ei] && module.ExecutionStage == Stage::EmitterSpawn) ||
                        (overlap > 0 && module.ExecutionStage == Stage::EmitterUpdate))
                    {
                        ExecuteParameterModule(module, ei);
                    }
                }
                m_EmitterSpawned[ei] = true;
                if (m_Failed)
                {
                    return;
                }
            }
            for (size_t mi = 0; mi < emitter.Modules.size(); ++mi)
            {
                const auto& module = emitter.Modules[mi];
                if (!module.Enabled)
                {
                    continue;
                }
                const auto value = Values(module, ei);
                uint32_t count = 0;
                if (module.Kind == ModuleKind::SpawnRate && overlap > 0)
                {
                    auto& remainder = m_SpawnRemainders[ei * MaxModules + mi];
                    remainder += value[0] * overlap;
                    count = static_cast<uint32_t>(std::floor(remainder + 1e-8));
                    remainder -= count;
                }
                else if (module.Kind == ModuleKind::Burst)
                {
                    const auto burstStep = static_cast<uint64_t>(std::ceil((emitter.Start + value[1]) * 60.0 - 1e-5));
                    if (phaseStep == burstStep && phase <= emitter.End)
                    {
                        count = static_cast<uint32_t>(value[0]);
                    }
                }
                if (count)
                {
                    Spawn(ei, count);
                }
            }
        }
        for (size_t ei = 0; ei < m_System.Emitters.size(); ++ei)
        {
            const auto& emitter = m_System.Emitters[ei];
            if (!active(emitter) || emitter.Simulation != Backend::GPU)
            {
                continue;
            }
            System resolved = m_System;
            for (auto& parameter : resolved.Parameters)
            {
                if (const auto* current = RuntimeParameter(parameter.Name, ei))
                {
                    parameter.Value = current->Value;
                }
            }
            if (!UpdateGPU(resolved, emitter, m_Particles, Time(), StepSeconds, m_Diagnostic))
            {
                m_Failed = true;
                return;
            }
        }

        struct Event
        {
            size_t Emitter;
            uint32_t Count;
            DirectX::XMFLOAT3 WorldPosition;
        };

        std::vector<Event> events;
        // Spawning from events is deferred so vector reallocation cannot invalidate an updating particle.
        for (auto& particle : m_Particles)
        {
            auto found = std::find_if(m_System.Emitters.begin(), m_System.Emitters.end(), [&](const Emitter& e) {
                return e.Id == particle.EmitterId;
            });
            if (found == m_System.Emitters.end())
            {
                particle.Lifetime = 0;
                continue;
            }
            const auto& emitter = *found;
            const size_t emitterIndex = static_cast<size_t>(found - m_System.Emitters.begin());
            if (!active(emitter))
            {
                particle.Lifetime = 0;
                continue;
            }
            if (emitter.Simulation == Backend::CPU)
            {
                particle.Age += StepSeconds;
                for (const auto& module : emitter.Modules)
                {
                    if (!module.Enabled)
                    {
                        continue;
                    }
                    const auto value = Values(module, emitterIndex);
                    const float t = std::clamp(particle.Age / (std::max)(particle.Lifetime, StepSeconds), 0.0f, 1.0f);
                    switch (module.Kind)
                    {
                        case ModuleKind::PointAttraction: {
                            const auto offset = XMVectorSubtract(XMVectorSet(value[0], value[1], value[2], 0),
                                                                 XMLoadFloat3(&particle.Position));
                            const float distance = XMVectorGetX(XMVector3Length(offset));
                            if (distance > 1e-5f && distance < module.Secondary[0])
                            {
                                const float weight =
                                    std::pow(1.0f - distance / module.Secondary[0], module.Secondary[1]);
                                XMStoreFloat3(
                                    &particle.Velocity,
                                    XMVectorAdd(XMLoadFloat3(&particle.Velocity),
                                                XMVectorScale(offset, value[3] * weight * StepSeconds / distance)));
                            }
                            break;
                        }
                        case ModuleKind::VortexForce: {
                            const auto axis = XMVector3Normalize(XMVectorSet(value[0], value[1], value[2], 0));
                            const auto offset = XMVectorSubtract(
                                XMLoadFloat3(&particle.Position),
                                XMVectorSet(module.Secondary[0], module.Secondary[1], module.Secondary[2], 0));
                            const auto radial =
                                XMVectorSubtract(offset, XMVectorScale(axis, XMVectorGetX(XMVector3Dot(offset, axis))));
                            const float distance = XMVectorGetX(XMVector3Length(radial));
                            if (distance > 1e-5f && distance < module.Secondary[3])
                            {
                                const float force = value[3] * (1.0f - distance / module.Secondary[3]);
                                XMStoreFloat3(&particle.Velocity,
                                              XMVectorAdd(XMLoadFloat3(&particle.Velocity),
                                                          XMVectorScale(XMVector3Cross(axis, radial),
                                                                        force * StepSeconds / distance)));
                            }
                            break;
                        }
                        case ModuleKind::LimitVelocity: {
                            const auto velocity = XMLoadFloat3(&particle.Velocity);
                            const float speed = XMVectorGetX(XMVector3Length(velocity));
                            if (speed > value[0] && speed > 1e-5f)
                            {
                                XMStoreFloat3(&particle.Velocity, XMVectorScale(velocity, value[0] / speed));
                            }
                            break;
                        }
                        case ModuleKind::RenderRadialWave: {
                            const auto radial = XMVectorSubtract(XMLoadFloat3(&particle.Position),
                                                                 XMVectorSet(value[0], value[1], value[2], 0));
                            const float distance = XMVectorGetX(XMVector3Length(radial));
                            if (distance > 1e-5f && distance < module.Secondary[3])
                            {
                                const float phase =
                                    (distance * module.Secondary[1] - Time() * module.Secondary[0]) * XM_2PI +
                                    module.Secondary[2];
                                const float force =
                                    std::sin(phase) * value[3] * (1.0f - distance / module.Secondary[3]);
                                XMStoreFloat3(&particle.Velocity,
                                              XMVectorAdd(XMLoadFloat3(&particle.Velocity),
                                                          XMVectorScale(radial, force * StepSeconds / distance)));
                            }
                            break;
                        }
                        case ModuleKind::RenderSpeedColor: {
                            const float speed = XMVectorGetX(XMVector3Length(XMLoadFloat3(&particle.Velocity)));
                            const float blend = std::clamp(
                                (speed - value[3]) / (std::max)(module.Secondary[3] - value[3], 1e-5f), 0.0f, 1.0f);
                            particle.Color.x =
                                particle.InitialColor.x * std::lerp(value[0], module.Secondary[0], blend);
                            particle.Color.y =
                                particle.InitialColor.y * std::lerp(value[1], module.Secondary[1], blend);
                            particle.Color.z =
                                particle.InitialColor.z * std::lerp(value[2], module.Secondary[2], blend);
                            break;
                        }
                        case ModuleKind::Gravity:
                            particle.Velocity.x += value[0] * StepSeconds;
                            particle.Velocity.y += value[1] * StepSeconds;
                            particle.Velocity.z += value[2] * StepSeconds;
                            break;
                        case ModuleKind::Drag: {
                            const float damping = std::exp(-value[0] * StepSeconds);
                            particle.Velocity.x *= damping;
                            particle.Velocity.y *= damping;
                            particle.Velocity.z *= damping;
                            break;
                        }
                        case ModuleKind::Noise: {
                            const float f = module.Secondary[0], time = Time();
                            const float seed = float(particle.Id % 10000) * 0.61803399f;
                            particle.Velocity.x +=
                                std::sin(particle.Position.y * f + time + seed) * value[0] * StepSeconds;
                            particle.Velocity.y +=
                                std::sin(particle.Position.z * f + time * 1.13f + seed) * value[1] * StepSeconds;
                            particle.Velocity.z +=
                                std::sin(particle.Position.x * f + time * 0.91f + seed) * value[2] * StepSeconds;
                            break;
                        }
                        case ModuleKind::ColorOverLife: {
                            Vector color{};
                            for (size_t c = 0; c < 4; ++c)
                            {
                                color[c] = value[c] + (module.Secondary[c] - value[c]) * t;
                            }
                            color = CurveValue(module, t, color);
                            particle.Color = {particle.InitialColor.x * color[0], particle.InitialColor.y * color[1],
                                              particle.InitialColor.z * color[2], particle.InitialColor.w * color[3]};
                            break;
                        }
                        case ModuleKind::ScaleOverLife:
                            particle.Size =
                                (std::max)(0.0f, particle.InitialSize *
                                                     CurveValue(module, t,
                                                                {value[0] + (module.Secondary[0] - value[0]) * t, 0, 0,
                                                                 0})[0]);
                            break;
                        case ModuleKind::GroundCollision:
                            if (particle.Position.y < value[0])
                            {
                                particle.Position.y = value[0];
                                if (particle.Velocity.y < 0)
                                {
                                    particle.Velocity.y = -particle.Velocity.y * value[1];
                                }
                            }
                            break;
                        case ModuleKind::CustomGraph:
                            if (module.ExecutionStage == Stage::ParticleUpdate)
                            {
                                ApplyGraph(module, particle, emitterIndex);
                            }
                            break;
                        default:
                            break;
                    }
                }
                particle.Position.x += particle.Velocity.x * StepSeconds;
                particle.Position.y += particle.Velocity.y * StepSeconds;
                particle.Position.z += particle.Velocity.z * StepSeconds;
                for (const auto& module : emitter.Modules)
                {
                    if (module.Enabled && module.Kind == ModuleKind::GroundCollision)
                    {
                        const auto value = Values(module, emitterIndex);
                        if (particle.Position.y < value[0])
                        {
                            particle.Position.y = value[0];
                            if (particle.Velocity.y < 0)
                            {
                                particle.Velocity.y = -particle.Velocity.y * value[1];
                            }
                        }
                    }
                }
            }
            if (particle.Age >= particle.Lifetime)
            {
                for (const auto& module : emitter.Modules)
                {
                    if (module.Enabled && module.Kind == ModuleKind::DeathEventSpawn &&
                        events.size() < MaxParticleCount)
                    {
                        const auto target = static_cast<size_t>(module.Secondary[0]);
                        const auto& targetEmitter = m_System.Emitters[target];
                        if (!active(targetEmitter) || phase < targetEmitter.Start || phase >= targetEmitter.End ||
                            (!m_System.Loop && m_Step >= cycleSteps))
                        {
                            continue;
                        }
                        DirectX::XMFLOAT3 worldPosition = particle.Position;
                        const auto transform = DirectX::XMLoadFloat4x4(&m_Transform);
                        if (emitter.LocalSpace && !targetEmitter.LocalSpace)
                        {
                            DirectX::XMStoreFloat3(
                                &worldPosition,
                                DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&worldPosition), transform));
                        }
                        if (!emitter.LocalSpace && targetEmitter.LocalSpace)
                        {
                            const float determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(transform));
                            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f)
                            {
                                m_Dropped += static_cast<uint32_t>(Values(module, emitterIndex)[0]);
                                continue;
                            }
                            DirectX::XMStoreFloat3(&worldPosition, DirectX::XMVector3TransformCoord(
                                                                       DirectX::XMLoadFloat3(&worldPosition),
                                                                       DirectX::XMMatrixInverse(nullptr, transform)));
                        }
                        events.push_back(
                            {target, static_cast<uint32_t>(Values(module, emitterIndex)[0]), worldPosition});
                    }
                }
            }
        }
        std::erase_if(m_Particles, [](const Particle& p) {
            return p.Age >= p.Lifetime;
        });
        std::fill(m_LiveCounts.begin(), m_LiveCounts.end(), 0);
        for (const auto& particle : m_Particles)
        {
            for (size_t i = 0; i < m_System.Emitters.size(); ++i)
            {
                if (particle.EmitterId == m_System.Emitters[i].Id)
                {
                    ++m_LiveCounts[i];
                    break;
                }
            }
        }
        for (const auto& event : events)
        {
            Spawn(event.Emitter, event.Count, &event.WorldPosition);
        }
        ++m_Step;
    }

    namespace
    {
        Json::Value JsonVector(const Vector& vector)
        {
            Json::Value result(Json::arrayValue);
            for (const auto value : vector)
            {
                result.append(value);
            }
            return result;
        }

        bool ReadVector(const Json::Value& value, Vector& vector)
        {
            if (!value.isArray() || value.size() != 4)
            {
                return false;
            }
            for (Json::ArrayIndex i = 0; i < 4; ++i)
            {
                if (!value[i].isNumeric())
                {
                    return false;
                }
                vector[i] = value[i].asFloat();
            }
            return Finite(vector);
        }

        bool ReadJson(const std::string& text, Json::Value& value, std::string& error)
        {
            Json::CharReaderBuilder builder;
            builder["rejectDupKeys"] = true;
            builder["failIfExtra"] = true;
            builder["stackLimit"] = 128;
            const auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
            return reader->parse(text.data(), text.data() + text.size(), &value, &error);
        }

        std::string JsonText(const Json::Value& value)
        {
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "  ";
            return Json::writeString(writer, value);
        }
    }

    bool Save(const std::filesystem::path& path, const System& system, std::string& diagnostic)
    {
        if (!Validate(system, diagnostic))
        {
            return false;
        }
        try
        {
            Json::Value document(Json::objectValue);
            document["format"] = "Niagara";
            document["version"] = 1;
            document["name"] = system.Name;
            document["duration"] = system.Duration;
            document["loop"] = system.Loop;
            document["seed"] = system.Seed;
            document["parameters"] = Json::Value(Json::arrayValue);
            document["emitters"] = Json::Value(Json::arrayValue);
            for (const auto& parameter : system.Parameters)
            {
                Json::Value value(Json::objectValue);
                value["name"] = parameter.Name;
                value["type"] = static_cast<int>(parameter.Type);
                value["scope"] = static_cast<int>(parameter.Scope);
                value["value"] = JsonVector(parameter.Value);
                document["parameters"].append(value);
            }
            const auto writeModule = [&](const Module& module, Json::Value& item) {
                item["id"] = Json::UInt64(module.Id);
                item["kind"] = static_cast<int>(module.Kind);
                item["enabled"] = module.Enabled;
                item["value"] = JsonVector(module.Value);
                item["secondary"] = JsonVector(module.Secondary);
                item["parameter"] = module.ParameterName;
                item["target"] = static_cast<int>(module.Target);
                item["stage"] = static_cast<int>(module.ExecutionStage);
                item["output"] = Json::UInt64(module.OutputPin);
                item["curve"] = Json::Value(Json::arrayValue);
                for (const auto& key : module.Curve)
                {
                    Json::Value entry(Json::objectValue);
                    entry["time"] = key.Time;
                    entry["value"] = JsonVector(key.Value);
                    item["curve"].append(entry);
                }
                if (!module.Graph.Nodes.empty())
                {
                    Json::Value graph;
                    if (!ReadJson(module.Graph.Serialize(), graph, diagnostic))
                    {
                        return false;
                    }
                    item["graph"] = std::move(graph);
                }

                item["writeParameter"] = module.WriteParameter;
                return true;
            };
            document["modules"] = Json::Value(Json::arrayValue);
            for (const auto& module : system.Modules)
            {
                Json::Value item(Json::objectValue);
                if (!writeModule(module, item))
                {
                    return false;
                }
                document["modules"].append(std::move(item));
            }
            for (const auto& emitter : system.Emitters)
            {
                Json::Value value(Json::objectValue);
                value["id"] = Json::UInt64(emitter.Id);
                value["name"] = emitter.Name;
                value["position"] = Json::Value(Json::arrayValue);
                value["position"].append(emitter.Position[0]);
                value["position"].append(emitter.Position[1]);
                value["enabled"] = emitter.Enabled;
                value["solo"] = emitter.Solo;
                value["localSpace"] = emitter.LocalSpace;
                value["start"] = emitter.Start;
                value["end"] = emitter.End;
                value["maxParticles"] = emitter.MaxParticles;
                value["renderer"] = static_cast<int>(emitter.Renderer);
                value["backend"] = static_cast<int>(emitter.Simulation);
                value["modules"] = Json::Value(Json::arrayValue);
                for (const auto& module : emitter.Modules)
                {
                    Json::Value item(Json::objectValue);
                    if (!writeModule(module, item))
                    {
                        return false;
                    }
                    value["modules"].append(std::move(item));
                }
                document["emitters"].append(std::move(value));
            }
            const auto serialized = JsonText(document);
            if (serialized.size() > 16 * 1024 * 1024)
            {
                diagnostic = "Niagara asset exceeds 16 MiB.";
                return false;
            }
            return WriteAssetFile(path, serialized, diagnostic);
        }
        catch (const std::exception& exception)
        {
            diagnostic = "Cannot save Niagara asset: " + std::string(exception.what());
            return false;
        }
    }

    bool Load(const std::filesystem::path& path, System& system, std::string& diagnostic)
    {
        diagnostic.clear();
        const auto fail = [&](const char* message) {
            diagnostic = message;
            return false;
        };
        try
        {
            std::error_code fileError;
            const auto size = std::filesystem::file_size(path, fileError);
            if (fileError || size > 16 * 1024 * 1024)
            {
                return fail("Cannot read Niagara asset or file exceeds 16 MiB.");
            }
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return fail("Cannot open Niagara asset.");
            }
            std::string text(static_cast<size_t>(size), '\0');
            stream.read(text.data(), static_cast<std::streamsize>(text.size()));
            if (!stream)
            {
                return fail("Cannot read complete Niagara asset.");
            }
            Json::Value document;
            if (!ReadJson(text, document, diagnostic))
            {
                return false;
            }
            if (!document.isObject() || document["format"] != "Niagara" || !document["version"].isInt() ||
                document["version"].asInt() != 1)
            {
                return fail("Unknown Niagara format or version.");
            }
            if (!document["name"].isString() || !document["duration"].isNumeric() || !document["loop"].isBool() ||
                !document["seed"].isUInt() || !document["parameters"].isArray() || !document["emitters"].isArray() ||
                document["parameters"].size() > MaxParameters || document["emitters"].size() > MaxEmitters)
            {
                return fail("Malformed Niagara system fields.");
            }
            System candidate;
            candidate.Name = document["name"].asString();
            candidate.Duration = document["duration"].asFloat();
            candidate.Loop = document["loop"].asBool();
            candidate.Seed = document["seed"].asUInt();
            for (const auto& value : document["parameters"])
            {
                Parameter parameter;
                if (!value.isObject() || !value["name"].isString() || !value["type"].isInt() ||
                    !value["scope"].isInt() || !ReadVector(value["value"], parameter.Value))
                {
                    return fail("Malformed Niagara parameter.");
                }
                parameter.Name = value["name"].asString();
                parameter.Type = static_cast<ParameterType>(value["type"].asInt());
                parameter.Scope = static_cast<ParameterScope>(value["scope"].asInt());
                candidate.Parameters.push_back(std::move(parameter));
            }
            const auto readModule = [&](const Json::Value& item, Module& module) {
                if (!item.isObject() || !item["id"].isUInt64() || !item["kind"].isInt() || !item["enabled"].isBool() ||
                    !item["parameter"].isString() || !item["target"].isInt() || !item["stage"].isInt() ||
                    !item["output"].isUInt64() || !ReadVector(item["value"], module.Value) ||
                    !ReadVector(item["secondary"], module.Secondary) || !item["curve"].isArray() ||
                    item["curve"].size() > 1024)
                {
                    return fail("Malformed Niagara module.");
                }
                module.Id = item["id"].asUInt64();
                module.Kind = static_cast<ModuleKind>(item["kind"].asInt());
                module.Enabled = item["enabled"].asBool();
                module.ParameterName = item["parameter"].asString();
                module.Target = static_cast<Attribute>(item["target"].asInt());
                module.ExecutionStage = static_cast<Stage>(item["stage"].asInt());
                module.OutputPin = item["output"].asUInt64();
                for (const auto& key : item["curve"])
                {
                    CurveKey entry;
                    if (!key.isObject() || !key["time"].isNumeric() || !ReadVector(key["value"], entry.Value))
                    {
                        return fail("Malformed Niagara curve.");
                    }
                    entry.Time = key["time"].asFloat();
                    module.Curve.push_back(entry);
                }
                if (item.isMember("graph"))
                {
                    if (!item["graph"].isObject() ||
                        !MaterialGraph::Deserialize(JsonText(item["graph"]), module.Graph, diagnostic))
                    {
                        return false;
                    }
                }

                if (item.isMember("writeParameter"))
                {
                    if (!item["writeParameter"].isString())
                    {
                        return fail("Malformed lifecycle write parameter.");
                    }
                    module.WriteParameter = item["writeParameter"].asString();
                }
                return true;
            };
            if (document.isMember("modules"))
            {
                if (!document["modules"].isArray() || document["modules"].size() > MaxModules)
                {
                    return fail("Malformed system module stack.");
                }
                for (const auto& item : document["modules"])
                {
                    Module module;
                    if (!readModule(item, module))
                    {
                        return false;
                    }
                    candidate.Modules.push_back(std::move(module));
                }
            }
            for (const auto& value : document["emitters"])
            {
                Emitter emitter;
                if (!value.isObject() || !value["id"].isUInt64() || !value["name"].isString() ||
                    !value["enabled"].isBool() || !value["solo"].isBool() || !value["localSpace"].isBool() ||
                    !value["start"].isNumeric() || !value["end"].isNumeric() || !value["maxParticles"].isUInt() ||
                    !value["renderer"].isInt() || !value["backend"].isInt() || !value["modules"].isArray() ||
                    value["modules"].size() > MaxModules)
                {
                    return fail("Malformed Niagara emitter.");
                }
                emitter.Id = value["id"].asUInt64();
                emitter.Name = value["name"].asString();
                if (value.isMember("position"))
                {
                    if (!value["position"].isArray() || value["position"].size() != 2 ||
                        !value["position"][0].isNumeric() || !value["position"][1].isNumeric())
                    {
                        return fail("Malformed emitter overview position.");
                    }
                    emitter.Position = {value["position"][0].asFloat(), value["position"][1].asFloat()};
                }
                emitter.Enabled = value["enabled"].asBool();
                emitter.Solo = value["solo"].asBool();
                emitter.LocalSpace = value["localSpace"].asBool();
                emitter.Start = value["start"].asFloat();
                emitter.End = value["end"].asFloat();
                emitter.MaxParticles = value["maxParticles"].asUInt();
                emitter.Renderer = static_cast<RendererKind>(value["renderer"].asInt());
                emitter.Simulation = static_cast<Backend>(value["backend"].asInt());
                for (const auto& item : value["modules"])
                {
                    Module module;
                    if (!readModule(item, module))
                    {
                        return false;
                    }
                    emitter.Modules.push_back(std::move(module));
                }
                candidate.Emitters.push_back(std::move(emitter));
            }
            if (!Validate(candidate, diagnostic))
            {
                return false;
            }
            system = std::move(candidate);
            return true;
        }
        catch (const std::exception& exception)
        {
            diagnostic = "Cannot load Niagara asset: " + std::string(exception.what());
            return false;
        }
    }
}
