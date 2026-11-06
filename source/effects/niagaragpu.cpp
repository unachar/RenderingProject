#include "pch.h"
#include "niagaragpu.h"
#include "niagaragpugraph.h"
#include "graphicsdevice.h"
#include <cmath>
#include <bit>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace Niagara
{
    namespace
    {
        // Explicit packing avoids depending on the compiler's layout of the editable Particle type.
        struct GPUReferenceParticle
        {
            XMFLOAT4 PositionAge;
            XMFLOAT4 VelocityLifetime;
            XMFLOAT4 Color;
            XMFLOAT4 SizeInitialSeed;
            XMFLOAT4 InitialColor;
        };

        static_assert(sizeof(GPUReferenceParticle) == 80);
        static_assert(offsetof(GPUReferenceParticle, VelocityLifetime) == 16);
        static_assert(offsetof(GPUReferenceParticle, Color) == 32);
        static_assert(offsetof(GPUReferenceParticle, SizeInitialSeed) == 48);
        static_assert(offsetof(GPUReferenceParticle, InitialColor) == 64);

        struct GPUState
        {
            ComPtr<ID3D12Device> Device;
            ComPtr<ID3D12CommandQueue> Queue;
            ComPtr<ID3D12CommandAllocator> Allocator;
            ComPtr<ID3D12GraphicsCommandList> List;
            ComPtr<ID3D12Fence> Fence;
            ComPtr<ID3D12RootSignature> Root;
            ComPtr<ID3D12Resource> Upload, Buffer, Readback;
            std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> Pipelines;
            HANDLE Event = nullptr;
            uint64_t Submitted = 0;
            size_t Capacity = 0;

            ~GPUState()
            {
                if (Event)
                {
                    CloseHandle(Event);
                }
            }
        };

        std::unique_ptr<GPUState> state;

        bool Result(HRESULT result, const char* operation, std::string& diagnostic)
        {
            if (SUCCEEDED(result))
            {
                return true;
            }
            diagnostic = std::string("Niagara GPU ") + operation + " failed (" + std::to_string(result) + ").";
            return false;
        }

        bool Wait(GPUState& gpu, std::string& diagnostic)
        {
            if (gpu.Fence->GetCompletedValue() == UINT64_MAX)
            {
                diagnostic = "Niagara GPU device was removed; playback stopped.";
                return false;
            }
            if (gpu.Fence->GetCompletedValue() >= gpu.Submitted)
            {
                return true;
            }
            ResetEvent(gpu.Event);
            if (!Result(gpu.Fence->SetEventOnCompletion(gpu.Submitted, gpu.Event), "fence event", diagnostic))
            {
                return false;
            }
            const auto wait = WaitForSingleObject(gpu.Event, 1000);
            if (wait != WAIT_OBJECT_0 || gpu.Fence->GetCompletedValue() < gpu.Submitted ||
                gpu.Fence->GetCompletedValue() == UINT64_MAX)
            {
                diagnostic =
                    "Niagara GPU did not complete within 1 second; playback stopped and in-flight resources retained.";
                return false;
            }
            return true;
        }

        bool Initialize(std::string& diagnostic)
        {
            auto* device = GraphicsDevice::GetDevice();
            if (!device)
            {
                diagnostic = "Niagara GPU needs an initialized DirectX 12 device.";
                return false;
            }
            if (state && state->Device.Get() == device)
            {
                return Wait(*state, diagnostic);
            }
            if (state)
            {
                if (!Wait(*state, diagnostic))
                {
                    return false;
                }
                state.reset();
            }
            auto candidate = std::make_unique<GPUState>();
            candidate->Device = device;
            D3D12_COMMAND_QUEUE_DESC queue{};
            queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            if (!Result(device->CreateCommandQueue(&queue, IID_PPV_ARGS(&candidate->Queue)), "queue", diagnostic) ||
                !Result(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                       IID_PPV_ARGS(&candidate->Allocator)),
                        "allocator", diagnostic) ||
                !Result(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, candidate->Allocator.Get(),
                                                  nullptr, IID_PPV_ARGS(&candidate->List)),
                        "command list", diagnostic) ||
                !Result(candidate->List->Close(), "initial close", diagnostic) ||
                !Result(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&candidate->Fence)), "fence",
                        diagnostic))
            {
                return false;
            }
            candidate->Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!candidate->Event)
            {
                diagnostic = "Niagara GPU cannot create fence event.";
                return false;
            }
            D3D12_ROOT_PARAMETER parameters[3]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[0].Descriptor.ShaderRegister = 0;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[1].Constants.ShaderRegister = 0;
            parameters[1].Constants.Num32BitValues = 4;
            parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[2].Descriptor.ShaderRegister = 0;
            D3D12_ROOT_SIGNATURE_DESC description{};
            description.NumParameters = 3;
            description.pParameters = parameters;
            ComPtr<ID3DBlob> serialized, errors;
            if (!Result(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
                        "root serialization", diagnostic) ||
                !Result(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                    IID_PPV_ARGS(&candidate->Root)),
                        "root signature", diagnostic))
            {
                return false;
            }
            state = std::move(candidate);
            return true;
        }

        bool Allocate(GPUState& gpu, size_t bytes, std::string& diagnostic)
        {
            if (gpu.Capacity >= bytes)
            {
                return true;
            }
            const size_t capacity = (bytes + 65535) & ~size_t(65535);
            const auto resource = CD3DX12_RESOURCE_DESC::Buffer(capacity);
            const auto uav = CD3DX12_RESOURCE_DESC::Buffer(capacity, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            const CD3DX12_HEAP_PROPERTIES upload(D3D12_HEAP_TYPE_UPLOAD), standard(D3D12_HEAP_TYPE_DEFAULT),
                readback(D3D12_HEAP_TYPE_READBACK);
            ComPtr<ID3D12Resource> newUpload, newBuffer, newReadback;
            if (!Result(gpu.Device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &resource,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                            IID_PPV_ARGS(&newUpload)),
                        "upload buffer", diagnostic) ||
                !Result(gpu.Device->CreateCommittedResource(&standard, D3D12_HEAP_FLAG_NONE, &uav,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&newBuffer)),
                        "simulation buffer", diagnostic) ||
                !Result(gpu.Device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &resource,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&newReadback)),
                        "readback buffer", diagnostic))
            {
                return false;
            }
            gpu.Upload = std::move(newUpload);
            gpu.Buffer = std::move(newBuffer);
            gpu.Readback = std::move(newReadback);
            gpu.Capacity = capacity;
            return true;
        }

        std::string Number(float value)
        {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            stream << std::scientific << std::setprecision(9) << value;
            return stream.str();
        }

        std::string Vector(const std::array<float, 4>& value)
        {
            return "float4(" + Number(value[0]) + "," + Number(value[1]) + "," + Number(value[2]) + "," +
                   Number(value[3]) + ")";
        }

        std::array<float, 4> Values(const System& system, const Module& module)
        {
            for (const auto& parameter : system.Parameters)
            {
                if (parameter.Name == module.ParameterName)
                {
                    return parameter.Value;
                }
            }
            return module.Value;
        }

        std::string Curve(const Module& module, const std::string& fallback)
        {
            if (module.Curve.empty())
            {
                return fallback;
            }
            std::string expression = Vector(module.Curve.back().Value);
            for (size_t i = module.Curve.size() - 1; i > 0; --i)
            {
                const auto& a = module.Curve[i - 1];
                const auto& b = module.Curve[i];
                expression = "(t<=" + Number(b.Time) + "?lerp(" + Vector(a.Value) + "," + Vector(b.Value) +
                             ",saturate((t-" + Number(a.Time) + ")/" + Number(b.Time - a.Time) + ")):" + expression +
                             ")";
            }
            return "(t<=" + Number(module.Curve.front().Time) + "?" + Vector(module.Curve.front().Value) + ":" +
                   expression + ")";
        }

        std::string Shader(const System& system, const Emitter& emitter, std::string& diagnostic)
        {
            std::string shader = R"(
struct Particle { float4 PositionAge; float4 VelocityLifetime; float4 Color; float4 SizeInitialSeed; float4 InitialColor; };
RWStructuredBuffer<Particle> Particles : register(u0);
StructuredBuffer<float4> NiagaraParameters : register(t0);
cbuffer Settings : register(b0) { uint Count; float Time; float Dt; uint Padding; };
[numthreads(64,1,1)] void CS(uint3 id:SV_DispatchThreadID) {
if(id.x>=Count) return;
Particle p=Particles[id.x]; p.PositionAge.w+=Dt;
float t=saturate(p.PositionAge.w/max(p.VelocityLifetime.w,1.0/60.0));
)";
            for (size_t moduleIndex = 0; moduleIndex < emitter.Modules.size(); ++moduleIndex)
            {
                const auto& module = emitter.Modules[moduleIndex];
                if (!module.Enabled)
                {
                    continue;
                }
                const auto index = system.Parameters.size() + moduleIndex * 2;
                shader += "{float4 v=NiagaraParameters[" + std::to_string(index) + "];float4 s=NiagaraParameters[" +
                          std::to_string(index + 1) + "];";
                switch (module.Kind)
                {
                    case ModuleKind::CustomGraph:
                        if (module.ExecutionStage == Stage::ParticleUpdate)
                        {
                            std::string graph;
                            if (!GenerateGPUGraph(module, system, graph, diagnostic))
                            {
                                return {};
                            }
                            shader += graph;
                        }
                        break;
                    case ModuleKind::PointAttraction:
                        shader += "float3 q=v.xyz-p.PositionAge.xyz;float r=length(q);"
                                  "if(r>1e-5&&r<s.x)p.VelocityLifetime.xyz+=q*(v.w*pow(1-r/s.x,s.y)*Dt/r);";
                        break;
                    case ModuleKind::VortexForce:
                        shader += "float3 axis=normalize(v.xyz);float3 q=p.PositionAge.xyz-s.xyz;"
                                  "q-=axis*dot(q,axis);float r=length(q);"
                                  "if(r>1e-5&&r<s.w)p.VelocityLifetime.xyz+=cross(axis,q)*(v.w*(1-r/s.w)*Dt/r);";
                        break;
                    case ModuleKind::LimitVelocity:
                        shader += "float speed=length(p.VelocityLifetime.xyz);"
                                  "if(speed>v.x&&speed>1e-5)p.VelocityLifetime.xyz*=v.x/speed;";
                        break;
                    case ModuleKind::RenderRadialWave:
                        shader += "float3 q=p.PositionAge.xyz-v.xyz;float r=length(q);"
                                  "if(r>1e-5&&r<s.w){float phase=(r*s.y-Time*s.x)*6.283185307+s.z;"
                                  "p.VelocityLifetime.xyz+=q*(sin(phase)*v.w*(1-r/s.w)*Dt/r);}";
                        break;
                    case ModuleKind::RenderSpeedColor:
                        shader += "float blend=saturate((length(p.VelocityLifetime.xyz)-v.w)/max(s.w-v.w,1e-5));"
                                  "p.Color.rgb=p.InitialColor.rgb*lerp(v.rgb,s.rgb,blend);";
                        break;
                    case ModuleKind::Gravity:
                        shader += "p.VelocityLifetime.xyz+=v.xyz*Dt;";
                        break;
                    case ModuleKind::Drag:
                        shader += "p.VelocityLifetime.xyz*=exp(-v.x*Dt);";
                        break;
                    case ModuleKind::Noise:
                        shader +=
                            "float seed=p.SizeInitialSeed.z*0.61803399;float3 q=p.PositionAge.xyz*s.x;"
                            "p.VelocityLifetime.xyz+=sin(float3(q.y+Time+seed,q.z+Time*1.13+seed,q.x+Time*0.91+seed))*v.xyz*Dt;";
                        break;
                    case ModuleKind::ColorOverLife:
                        shader += "p.Color=p.InitialColor*" + Curve(module, "lerp(v,s,t)") + ";";
                        break;
                    case ModuleKind::ScaleOverLife:
                        shader +=
                            "p.SizeInitialSeed.x=max(0,p.SizeInitialSeed.y*(" + Curve(module, "lerp(v,s,t)") + ").x);";
                        break;
                    case ModuleKind::GroundCollision:
                        shader +=
                            "if(p.PositionAge.y<v.x){p.PositionAge.y=v.x;if(p.VelocityLifetime.y<0)p.VelocityLifetime.y=-p.VelocityLifetime.y*v.y;}";
                        break;
                    default:
                        break;
                }
                shader += "}\n";
            }
            shader += "p.PositionAge.xyz+=p.VelocityLifetime.xyz*Dt;\n";
            for (size_t moduleIndex = 0; moduleIndex < emitter.Modules.size(); ++moduleIndex)
            {
                const auto& module = emitter.Modules[moduleIndex];
                if (module.Enabled && module.Kind == ModuleKind::GroundCollision)
                {
                    const auto index = system.Parameters.size() + moduleIndex * 2;
                    shader += "{float4 collision=NiagaraParameters[" + std::to_string(index) +
                              "];"
                              "if(p.PositionAge.y<collision.x){p.PositionAge.y=collision.x;"
                              "if(p.VelocityLifetime.y<0)p.VelocityLifetime.y=-p.VelocityLifetime.y*collision.y;}}\n";
                }
            }
            shader += "Particles[id.x]=p;}";
            return shader;
        }

        bool Pipeline(GPUState& gpu, const std::string& shader, ID3D12PipelineState*& pipeline, std::string& diagnostic)
        {
            if (auto found = gpu.Pipelines.find(shader); found != gpu.Pipelines.end())
            {
                pipeline = found->second.Get();
                return true;
            }
            ComPtr<ID3DBlob> bytecode, errors;
            const auto compile = D3DCompile(
                shader.data(), shader.size(), "Niagara reference simulation", nullptr, nullptr, "CS", "cs_5_1",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
            if (!Result(compile, "compute compilation", diagnostic))
            {
                if (errors)
                {
                    diagnostic += " " + std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                                    errors->GetBufferSize());
                }
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
            description.pRootSignature = gpu.Root.Get();
            description.CS = {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
            ComPtr<ID3D12PipelineState> created;
            if (!Result(gpu.Device->CreateComputePipelineState(&description, IID_PPV_ARGS(&created)),
                        "compute pipeline", diagnostic))
            {
                return false;
            }
            // All earlier dispatches have finished before this cache may evict a pipeline.
            if (gpu.Pipelines.size() >= 64)
            {
                gpu.Pipelines.clear();
            }
            pipeline = created.Get();
            gpu.Pipelines.emplace(shader, std::move(created));
            return true;
        }
    }

    bool SupportsGPU(const Emitter& emitter, std::string& diagnostic)
    {
        diagnostic.clear();
        for (const auto& module : emitter.Modules)
        {
            if (module.Enabled && module.Kind == ModuleKind::CustomGraph &&
                module.ExecutionStage == Stage::ParticleUpdate)
            {
                for (const auto& node : module.Graph.Nodes)
                {
                    if (!SupportsGraphNode(node.Kind))
                    {
                        diagnostic = "GPU custom graph has an unsupported node: " +
                                     std::string(MaterialGraph::Definition(node.Kind).Name);
                        return false;
                    }
                }
            }
        }
        return true;
    }

    bool UpdateGPU(const System& system, const Emitter& emitter, std::vector<Particle>& particles, float time,
                   float seconds, std::string& diagnostic)
    {
        diagnostic.clear();
        std::vector<size_t> indices;
        std::vector<GPUReferenceParticle> packed;
        for (size_t i = 0; i < particles.size(); ++i)
        {
            const auto& particle = particles[i];
            if (particle.EmitterId != emitter.Id)
            {
                continue;
            }
            indices.push_back(i);
            packed.push_back({{particle.Position.x, particle.Position.y, particle.Position.z, particle.Age},
                              {particle.Velocity.x, particle.Velocity.y, particle.Velocity.z, particle.Lifetime},
                              particle.Color,
                              {particle.Size, particle.InitialSize, float(particle.Id % 10000),
                               std::bit_cast<float>(static_cast<uint32_t>(particle.Id))},
                              particle.InitialColor});
        }
        if (packed.empty())
        {
            return true;
        }
        if (!SupportsGPU(emitter, diagnostic) || !Initialize(diagnostic))
        {
            return false;
        }
        auto& gpu = *state;
        const size_t bytes = packed.size() * sizeof(GPUReferenceParticle);
        const size_t parameterOffset = (bytes + 255) & ~size_t(255);
        std::vector<std::array<float, 4>> uniforms;
        uniforms.reserve(system.Parameters.size() + emitter.Modules.size() * 2);
        for (const auto& parameter : system.Parameters)
        {
            uniforms.push_back(parameter.Value);
        }
        for (const auto& module : emitter.Modules)
        {
            uniforms.push_back(Values(system, module));
            uniforms.push_back(module.Secondary);
        }
        const size_t uniformBytes = uniforms.size() * sizeof(std::array<float, 4>);
        ID3D12PipelineState* pipeline = nullptr;
        const auto shader = Shader(system, emitter, diagnostic);
        if (shader.empty() || !Allocate(gpu, parameterOffset + (std::max)(uniformBytes, size_t(16)), diagnostic) ||
            !Pipeline(gpu, shader, pipeline, diagnostic))
        {
            return false;
        }
        void* mapped = nullptr;
        const D3D12_RANGE empty{0, 0};
        if (!Result(gpu.Upload->Map(0, &empty, &mapped), "upload map", diagnostic))
        {
            return false;
        }
        std::memcpy(mapped, packed.data(), bytes);
        if (uniformBytes)
        {
            std::memcpy(static_cast<uint8_t*>(mapped) + parameterOffset, uniforms.data(), uniformBytes);
        }
        gpu.Upload->Unmap(0, nullptr);
        if (!Result(gpu.Allocator->Reset(), "allocator reset", diagnostic) ||
            !Result(gpu.List->Reset(gpu.Allocator.Get(), pipeline), "list reset", diagnostic))
        {
            return false;
        }
        gpu.List->CopyBufferRegion(gpu.Buffer.Get(), 0, gpu.Upload.Get(), 0, bytes);
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(gpu.Buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.List->ResourceBarrier(1, &barrier);
        gpu.List->SetComputeRootSignature(gpu.Root.Get());
        gpu.List->SetComputeRootUnorderedAccessView(0, gpu.Buffer->GetGPUVirtualAddress());
        gpu.List->SetComputeRootShaderResourceView(2, gpu.Upload->GetGPUVirtualAddress() + parameterOffset);

        struct Constants
        {
            uint32_t Count;
            float Time, Seconds;
            uint32_t Padding;
        } constants{static_cast<uint32_t>(packed.size()), time, seconds, 0};

        gpu.List->SetComputeRoot32BitConstants(1, 4, &constants, 0);
        gpu.List->Dispatch(static_cast<UINT>((packed.size() + 63) / 64), 1, 1);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(gpu.Buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.List->ResourceBarrier(1, &barrier);
        gpu.List->CopyBufferRegion(gpu.Readback.Get(), 0, gpu.Buffer.Get(), 0, bytes);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(gpu.Buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.List->ResourceBarrier(1, &barrier);
        if (!Result(gpu.List->Close(), "list close", diagnostic))
        {
            return false;
        }
        ID3D12CommandList* lists[]{gpu.List.Get()};
        gpu.Queue->ExecuteCommandLists(1, lists);
        ++gpu.Submitted;
        if (!Result(gpu.Queue->Signal(gpu.Fence.Get(), gpu.Submitted), "queue signal", diagnostic) ||
            !Wait(gpu, diagnostic))
        {
            return false;
        }
        const D3D12_RANGE readRange{0, bytes};
        if (!Result(gpu.Readback->Map(0, &readRange, &mapped), "readback map", diagnostic))
        {
            return false;
        }
        std::memcpy(packed.data(), mapped, bytes);
        gpu.Readback->Unmap(0, &empty);
        for (size_t i = 0; i < packed.size(); ++i)
        {
            auto& particle = particles[indices[i]];
            const auto& result = packed[i];
            particle.Position = {result.PositionAge.x, result.PositionAge.y, result.PositionAge.z};
            particle.Age = result.PositionAge.w;
            particle.Velocity = {result.VelocityLifetime.x, result.VelocityLifetime.y, result.VelocityLifetime.z};
            particle.Lifetime = result.VelocityLifetime.w;
            particle.Color = result.Color;
            particle.Size = result.SizeInitialSeed.x;
            if (!std::isfinite(particle.Position.x) || !std::isfinite(particle.Position.y) ||
                !std::isfinite(particle.Position.z) || !std::isfinite(particle.Size) ||
                !std::isfinite(particle.Color.x) || !std::isfinite(particle.Color.y) ||
                !std::isfinite(particle.Color.z) || !std::isfinite(particle.Color.w))
            {
                diagnostic = "Niagara GPU produced non-finite particle data; playback stopped.";
                return false;
            }
        }
        return true;
    }

    void ShutdownGPU()
    {
        if (!state)
        {
            return;
        }
        std::string diagnostic;
        if (Wait(*state, diagnostic))
        {
            state.reset();
        }
        else
        {
            Debug::Log("%s\n", diagnostic.c_str());
        }
    }
}
