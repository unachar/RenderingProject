#pragma once
#include "../assets/materialgraph.h"
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Niagara
{
    enum class ParameterType
    {
        Bool,
        Int,
        Float,
        Vector2,
        Vector3,
        Vector4,
        Color
    };
    enum class ParameterScope
    {
        User,
        System,
        Emitter,
        Particles
    };

    struct Parameter
    {
        std::string Name;
        ParameterType Type = ParameterType::Float;
        ParameterScope Scope = ParameterScope::User;
        std::array<float, 4> Value{};
    };
    enum class ModuleKind
    {
        SpawnRate,
        Burst,
        Initialize,
        Velocity,
        Gravity,
        Drag,
        ColorOverLife,
        ScaleOverLife,
        Noise,
        GroundCollision,
        DeathEventSpawn,
        CustomGraph,
        ConeVelocity,
        PointAttraction,
        VortexForce,
        LimitVelocity,
        RenderRadialWave,
        RenderSpeedColor,
        Count
    };
    enum class Attribute
    {
        Position,
        Velocity,
        Color,
        Size,
        Lifetime
    };
    enum class Stage
    {
        ParticleSpawn,
        ParticleUpdate,
        SystemSpawn,
        SystemUpdate,
        EmitterSpawn,
        EmitterUpdate
    };

    struct CurveKey
    {
        float Time = 0;
        std::array<float, 4> Value{};
    };

    struct Module
    {
        uint64_t Id = 0;
        ModuleKind Kind = ModuleKind::SpawnRate;
        bool Enabled = true;
        std::array<float, 4> Value{};
        std::array<float, 4> Secondary{};
        MaterialGraph Graph;
        uint64_t OutputPin = 0;
        std::string ParameterName;
        std::string WriteParameter;
        Attribute Target = Attribute::Velocity;
        Stage ExecutionStage = Stage::ParticleUpdate;
        std::vector<CurveKey> Curve;
    };
    enum class RendererKind
    {
        Sprite,
        Mesh,
        Ribbon
    };
    enum class Backend
    {
        CPU,
        GPU
    };

    struct Emitter
    {
        uint64_t Id = 0;
        std::string Name = "Emitter";
        std::array<float, 2> Position{};
        bool Enabled = true, Solo = false, LocalSpace = true;
        float Start = 0, End = 5;
        uint32_t MaxParticles = 10000;
        RendererKind Renderer = RendererKind::Sprite;
        Backend Simulation = Backend::CPU;
        std::vector<Module> Modules;
    };

    struct System
    {
        std::string Name = "Niagara System";
        float Duration = 5;
        bool Loop = true;
        uint32_t Seed = 1;
        std::vector<Emitter> Emitters;
        std::vector<Parameter> Parameters;
        std::vector<Module> Modules;
    };

    struct Particle
    {
        DirectX::XMFLOAT3 Position{}, Velocity{};
        DirectX::XMFLOAT4 Color{1, 1, 1, 1};
        float Size = 0.1f, Age = 0, Lifetime = 2;
        uint64_t EmitterId = 0, Id = 0;
        float InitialSize = 0.1f;
        DirectX::XMFLOAT4 InitialColor{1, 1, 1, 1};
    };

    class Runtime
    {
      public:
        void Reset(const System& system);
        void Seek(float time);
        void Advance(float seconds);

        void SetTransform(const DirectX::XMFLOAT4X4& transform)
        {
            m_Transform = transform;
        }

        bool UpdateSeek(size_t maxSteps = 240);

        float Time() const
        {
            return static_cast<float>(m_Step) / 60.0f;
        }

        bool Seeking() const
        {
            return !m_Failed && m_Step < m_TargetStep;
        }

        bool Failed() const
        {
            return m_Failed;
        }

        const std::vector<Particle>& Particles() const
        {
            return m_Particles;
        }

        uint64_t Dropped() const
        {
            return m_Dropped;
        }

        const std::string& Diagnostic() const
        {
            return m_Diagnostic;
        }

      private:
        void Step();
        void Spawn(size_t emitter, uint32_t count, const DirectX::XMFLOAT3* position = nullptr);
        std::array<float, 4> Values(const Module& module, size_t emitter) const;
        void ApplyGraph(const Module& module, Particle& particle, size_t emitter);
        void ExecuteParameterModule(const Module& module, size_t emitter);
        const Parameter* RuntimeParameter(const std::string& name, size_t emitter) const;
        System m_System;
        System m_Definition;
        std::vector<Particle> m_Particles;
        std::vector<double> m_SpawnRemainders;
        std::vector<uint32_t> m_LiveCounts;
        std::vector<std::vector<Parameter>> m_EmitterParameters;
        std::vector<bool> m_EmitterSpawned;
        uint64_t m_Step = 0, m_TargetStep = 0, m_Dropped = 0, m_NextParticle = 1;
        uint32_t m_Random = 1;
        double m_Accumulator = 0;
        std::string m_Diagnostic;
        bool m_Failed = false;
        DirectX::XMFLOAT4X4 m_Transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };

    System MakeDefaultSystem();
    bool Validate(const System& system, std::string& diagnostic);
    bool Load(const std::filesystem::path& path, System& system, std::string& diagnostic);
    bool Save(const std::filesystem::path& path, const System& system, std::string& diagnostic);
    const char* ModuleName(ModuleKind kind);
    bool SupportsGraphNode(MaterialNodeKind kind);
    Module MakeModule(ModuleKind kind);
    uint64_t NextId(const System& system);
}
