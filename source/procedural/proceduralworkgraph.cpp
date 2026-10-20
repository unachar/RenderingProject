#include "pch.h"
#include "proceduralworkgraph.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "workgraphruntime.h"
#include <cstring>
#include <cmath>
#include <chrono>
#include <mutex>
#include <stdexcept>

namespace ProceduralWorkGraph
{
    namespace
    {
        struct Parameters
        {
            uint32_t Mode;
            uint32_t Width;
            uint32_t Height;
            uint32_t Depth;
            uint32_t Kind;
            uint32_t Output;
            ProcStudio::NoiseParams Noise;
            std::array<float, 3> ColorA;
            std::array<float, 3> ColorB;
            std::array<float, 3> ColorC;
            uint32_t Tileable;
            float Contrast;
            float Bias;
            float Turbulence;
            float RingCount;
            int BricksX;
            int BricksY;
            float Mortar;
            float Time;
            float Choppiness;
            float Coverage;
            float Softness;
            float CenterX;
            float CenterY;
            float Zoom;
            float JuliaX;
            float JuliaY;
            int Iterations;
            float NormalStrength;
            int SourceWidth = 0;
            int SourceHeight = 0;
            int ImageOctaves = 1;
            float VerticalVariation = 0;
            uint32_t ImageSeed = 0;
            ProcStudio::Float3 MappingMinimum;
            float MappingScale = 1;
        };

        static_assert(offsetof(Parameters, NormalStrength) == 168);
        static_assert(offsetof(Parameters, ImageSeed) == 188);
        static_assert(offsetof(Parameters, MappingMinimum) == 192);
        static_assert(offsetof(Parameters, MappingScale) == 204);

        Parameters Pack(const ProcStudio::TextureParams& source, uint32_t mode, int width, int height, int depth)
        {
            return {mode,
                    static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height),
                    static_cast<uint32_t>(depth),
                    static_cast<uint32_t>(source.Kind),
                    static_cast<uint32_t>(source.Output),
                    source.Noise,
                    source.ColorA,
                    source.ColorB,
                    source.ColorC,
                    static_cast<uint32_t>(source.Tileable),
                    source.Contrast,
                    source.Bias,
                    source.Turbulence,
                    source.RingCount,
                    source.BricksX,
                    source.BricksY,
                    source.Mortar,
                    source.Time,
                    source.Choppiness,
                    source.Coverage,
                    source.Softness,
                    source.CenterX,
                    source.CenterY,
                    source.Zoom,
                    source.JuliaX,
                    source.JuliaY,
                    source.Iterations,
                    source.NormalStrength};
        }

        void Require(HRESULT result, const char* operation)
        {
            if (FAILED(result))
            {
                throw std::runtime_error(std::string("Procedural Work Graph: ") + operation);
            }
        }

        class Generator
        {
          public:
            void Execute(const Parameters& parameters, const void* source, size_t sourceBytes, void* destination,
                         size_t destinationBytes)
            {
                std::lock_guard<std::mutex> guard(m_Mutex);
                ID3D12Device* device = GraphicsDevice::GetDevice();
                if (!device || !WorkGraph::IsSupported(device))
                {
                    throw std::runtime_error("Procedural generation requires a Work Graph capable D3D12 device.");
                }
                Initialize(device, parameters);
                auto parameterBuffer = Upload(device, &parameters, sizeof(parameters));
                uint32_t emptyInput = 0;
                if (!source || sourceBytes == 0)
                {
                    source = &emptyInput;
                    sourceBytes = sizeof(emptyInput);
                }
                auto sourceBuffer = Upload(device, source, sourceBytes);
                auto output = Buffer(device, destinationBytes, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                auto readback =
                    Buffer(device, destinationBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                Require(m_Allocator->Reset(), "reset allocator");
                Require(m_Commands->Reset(m_Allocator.Get(), nullptr), "reset commands");
                m_Commands->SetComputeRootSignature(m_Root.Get());
                m_Commands->SetComputeRootShaderResourceView(0, parameterBuffer->GetGPUVirtualAddress());
                m_Commands->SetComputeRootShaderResourceView(1, sourceBuffer->GetGPUVirtualAddress());
                m_Commands->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
                const uint64_t count = static_cast<uint64_t>(parameters.Width) * parameters.Height * parameters.Depth;
                const uint32_t groups = static_cast<uint32_t>((count + 63) / 64);
                const uint32_t record[3] = {(std::min)(groups, 4096u), (groups + 4095) / 4096, 1};
                if (!m_Graphs[GraphIndex(parameters)].DispatchCpu(m_Commands.Get(), 0, record, sizeof(record)))
                {
                    throw std::runtime_error("Procedural Work Graph dispatch failed.");
                }
                const auto transition = CD3DX12_RESOURCE_BARRIER::Transition(
                    output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                m_Commands->ResourceBarrier(1, &transition);
                m_Commands->CopyResource(readback.Get(), output.Get());
                Require(m_Commands->Close(), "close commands");
                ID3D12CommandList* lists[] = {m_Commands.Get()};
                m_Queue->ExecuteCommandLists(1, lists);
                const UINT64 fenceValue = ++m_FenceValue;
                Require(m_Queue->Signal(m_Fence.Get(), fenceValue), "signal authoring fence");
                if (m_Fence->GetCompletedValue() < fenceValue)
                {
                    Require(m_Fence->SetEventOnCompletion(fenceValue, m_Event), "wait for authoring output");
                    if (WaitForSingleObject(m_Event, INFINITE) != WAIT_OBJECT_0)
                    {
                        throw std::runtime_error("Procedural authoring fence wait failed.");
                    }
                }
                void* mapped = nullptr;
                const D3D12_RANGE range{0, destinationBytes};
                Require(readback->Map(0, &range, &mapped), "map authoring output");
                std::memcpy(destination, mapped, destinationBytes);
                const D3D12_RANGE noWrites{0, 0};
                readback->Unmap(0, &noWrites);
            }

            ~Generator()
            {
                if (m_Event)
                {
                    CloseHandle(m_Event);
                }
            }

          private:
            static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, size_t bytes, D3D12_HEAP_TYPE heapType,
                                                 D3D12_RESOURCE_STATES state,
                                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
            {
                const auto heap = CD3DX12_HEAP_PROPERTIES(heapType);
                const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
                ComPtr<ID3D12Resource> resource;
                Require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                                        IID_PPV_ARGS(&resource)),
                        "create authoring buffer");
                return resource;
            }

            static ComPtr<ID3D12Resource> Upload(ID3D12Device* device, const void* source, size_t bytes)
            {
                auto resource = Buffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                void* mapped = nullptr;
                const D3D12_RANGE noReads{0, 0};
                Require(resource->Map(0, &noReads, &mapped), "map authoring parameters");
                std::memcpy(mapped, source, bytes);
                resource->Unmap(0, nullptr);
                return resource;
            }

            static size_t GraphIndex(const Parameters& parameters)
            {
                if (parameters.Mode == 0 && parameters.Kind < static_cast<uint32_t>(ProcStudio::TextureKind::Count) &&
                    parameters.Output < static_cast<uint32_t>(ProcStudio::TextureOutput::Count))
                {
                    return 18 + parameters.Kind * static_cast<size_t>(ProcStudio::TextureOutput::Count) +
                           parameters.Output;
                }
                return parameters.Mode;
            }

            void Initialize(ID3D12Device* device, const Parameters& parameters)
            {
                const uint32_t mode = parameters.Mode;
                if (mode >= 18)
                {
                    throw std::runtime_error("Unknown procedural Work Graph mode.");
                }
                if (m_Device.Get() != device || !m_Root)
                {
                    for (auto& graph : m_Graphs)
                    {
                        graph.Reset();
                    }
                    m_Device = device;
                    CD3DX12_ROOT_PARAMETER roots[3];
                    roots[0].InitAsShaderResourceView(0);
                    roots[1].InitAsShaderResourceView(1);
                    roots[2].InitAsUnorderedAccessView(0);
                    CD3DX12_ROOT_SIGNATURE_DESC description(3, roots);
                    ComPtr<ID3DBlob> serialized;
                    ComPtr<ID3DBlob> errors;
                    Require(
                        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
                        "serialize authoring root signature");
                    Require(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(&m_Root)),
                            "create authoring root signature");
                    D3D12_COMMAND_QUEUE_DESC queueDescription{};
                    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                    Require(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_Queue)),
                            "create authoring queue");
                    Require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_Allocator)),
                            "create authoring allocator");
                    Require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Allocator.Get(), nullptr,
                                                      IID_PPV_ARGS(&m_Commands)),
                            "create authoring commands");
                    Require(m_Commands->Close(), "close initial authoring commands");
                    Require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence)),
                            "create authoring fence");
                    m_FenceValue = 0;
                    if (!m_Event)
                    {
                        m_Event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    }
                    if (!m_Event)
                    {
                        throw std::runtime_error("Cannot create procedural authoring fence event.");
                    }
                }
                auto& graph = m_Graphs[GraphIndex(parameters)];
                if (graph.IsInitialized())
                {
                    return;
                }
                std::string source = "#define PROCEDURAL_SPECIALIZED_MODE " + std::to_string(mode) + "\n";
                if (GraphIndex(parameters) >= 18)
                {
                    source += "#define PROCEDURAL_TEXTURE_KIND " + std::to_string(parameters.Kind) + "\n";
                    source += "#define PROCEDURAL_TEXTURE_OUTPUT " + std::to_string(parameters.Output) + "\n";
                }
                source += "#include \"ProceduralStudioWorkGraph.hlsl\"\n";
                ComPtr<ID3DBlob> library;
                WriteGraphicsLog("[WorkGraph procedural] compiling specialized mode=%u kind=%u output=%u\n", mode,
                                 parameters.Kind, parameters.Output);
                Require(WorkGraph::CompileSource(source.data(), source.size(),
                                                 L"shader/hlsl/ProceduralStudioWorkGraph.hlsl", nullptr, L"lib_6_9",
                                                 library),
                        "compile specialized authoring program");
                WriteGraphicsLog("[WorkGraph procedural] compiled mode=%u bytecodeBytes=%zu; creating GPU program\n",
                                 mode, library->GetBufferSize());
                if (!graph.InitializeLibrary(device, m_Root.Get(), library.Get(), L"Generate"))
                {
                    throw std::runtime_error("Cannot initialize specialized procedural authoring Work Graph.");
                }
                WriteGraphicsLog("[WorkGraph procedural] ready mode=%u bytecodeBytes=%zu\n", mode,
                                 library->GetBufferSize());
            }

            std::mutex m_Mutex;
            ComPtr<ID3D12Device> m_Device;
            ComPtr<ID3D12RootSignature> m_Root;
            ComPtr<ID3D12CommandQueue> m_Queue;
            ComPtr<ID3D12CommandAllocator> m_Allocator;
            ComPtr<ID3D12GraphicsCommandList> m_Commands;
            ComPtr<ID3D12Fence> m_Fence;
            HANDLE m_Event = nullptr;
            UINT64 m_FenceValue = 0;
            std::array<WorkGraph::ComputeGraph, 18 + static_cast<size_t>(ProcStudio::TextureKind::Count) *
                                                         static_cast<size_t>(ProcStudio::TextureOutput::Count)>
                m_Graphs;
        };

        Generator g_Generator;
    }

    void BakeSolidVertexColors(ProcStudio::Mesh& mesh, const ProcStudio::TextureParams& parameters, float scale)
    {
        if (mesh.Positions.empty())
        {
            mesh.Colors.clear();
            return;
        }
        if (mesh.Positions.size() > UINT32_MAX / sizeof(ProcStudio::Float3))
        {
            throw std::runtime_error("Procedural solid color bake exceeds the Work Graph address limit.");
        }
        ProcStudio::Float3 minimum;
        ProcStudio::Float3 maximum;
        mesh.Bounds(minimum, maximum);
        const float extent = (std::max)({maximum.X - minimum.X, maximum.Y - minimum.Y, maximum.Z - minimum.Z, 1e-4f});
        auto packed = Pack(parameters, 17, static_cast<int>(mesh.Positions.size()), 1, 1);
        packed.MappingMinimum = minimum;
        packed.MappingScale = scale / extent;
        std::vector<ProcStudio::Float3> colors(mesh.Positions.size());
        g_Generator.Execute(packed, mesh.Positions.data(), mesh.Positions.size() * sizeof(ProcStudio::Float3),
                            colors.data(), colors.size() * sizeof(ProcStudio::Float3));
        mesh.Colors = std::move(colors);
    }

    std::vector<Procedural::Vector3> SampleMotionBatch(const std::vector<MotionInput>& samples)
    {
        static_assert(sizeof(MotionInput) == 32);
        static_assert(offsetof(MotionInput, Time) == 0);
        static_assert(offsetof(MotionInput, Speed) == 8);
        static_assert(offsetof(MotionInput, Method) == 12);
        static_assert(offsetof(MotionInput, Amplitude) == 16);
        static_assert(offsetof(MotionInput, Seed) == 28);
        static_assert(sizeof(Procedural::Vector3) == 12);
        if (samples.empty())
        {
            return {};
        }
        if (samples.size() > UINT32_MAX / sizeof(MotionInput))
        {
            throw std::runtime_error("Procedural motion batch exceeds the Work Graph address limit.");
        }
        std::vector<Procedural::Vector3> offsets(samples.size());
        const auto packed = Pack({}, 16, static_cast<int>(samples.size()), 1, 1);
        g_Generator.Execute(packed, samples.data(), samples.size() * sizeof(MotionInput), offsets.data(),
                            offsets.size() * sizeof(Procedural::Vector3));
        return offsets;
    }

    std::vector<float> BuildHeightMap(const Procedural::TerrainSettings& settings)
    {
        static_assert(sizeof(Procedural::BasicNoiseSettings) == 28);
        static_assert(sizeof(Procedural::NoiseSettings) == 192);
        static_assert(sizeof(Procedural::TerrainSettings) == 208);
        const int side = std::clamp(settings.Resolution, 4, 256) + 1;
        std::vector<float> heights(static_cast<size_t>(side) * side);
        const auto packed = Pack({}, 4, side, side, 1);
        g_Generator.Execute(packed, &settings, sizeof(settings), heights.data(), heights.size() * sizeof(float));
        return heights;
    }

    std::vector<Vertex> BuildTerrainVertices(const Procedural::TerrainSettings& settings)
    {
        static_assert(sizeof(Vertex) == 48);
        const int resolution = std::clamp(settings.Resolution, 4, 256);
        std::vector<Vertex> vertices(static_cast<size_t>(resolution) * resolution * 6);
        const auto packed = Pack({}, 15, static_cast<int>(vertices.size()), 1, 1);
        g_Generator.Execute(packed, &settings, sizeof(settings), vertices.data(), vertices.size() * sizeof(Vertex));
        return vertices;
    }

    ProcStudio::Mesh GenerateGridModel(const ProcStudio::ModelParams& parameters)
    {
        int resolution = std::clamp(parameters.Resolution, 2, 512);
        uint32_t mode = 5;
        ProcStudio::TextureParams texture;
        texture.Noise = parameters.Noise;
        texture.Noise.Seed ^= parameters.Seed;
        texture.Tileable = false;
        texture.Time = parameters.Time;
        texture.Turbulence = parameters.Height;
        texture.RingCount = parameters.Size;
        texture.Coverage = parameters.WaveAmplitude;
        texture.Output = static_cast<ProcStudio::TextureOutput>(parameters.TerrainColors);
        if (parameters.Kind == ProcStudio::ModelKind::WaterSurface)
        {
            mode = 6;
            texture.Kind = ProcStudio::TextureKind::FractalWater;
        }
        else if (parameters.Kind == ProcStudio::ModelKind::Plane)
        {
            mode = 7;
            resolution = 2;
            texture.RingCount = 2.5f;
        }
        const size_t vertexCount = static_cast<size_t>(resolution + 1) * (resolution + 1);
        const size_t indexCount = static_cast<size_t>(resolution) * resolution * 6;

        struct GridVertex
        {
            ProcStudio::Float3 Position;
            ProcStudio::Float3 Normal;
            ProcStudio::Float3 Color;
            ProcStudio::Float2 UV;
        };

        static_assert(sizeof(GridVertex) == 44);
        std::vector<uint32_t> output(vertexCount * 11 + indexCount);
        auto packed = Pack(texture, mode, static_cast<int>((std::max)(vertexCount, indexCount)), 1, 1);
        packed.SourceWidth = resolution;
        g_Generator.Execute(packed, nullptr, 0, output.data(), output.size() * sizeof(uint32_t));
        ProcStudio::Mesh mesh;
        mesh.Positions.resize(vertexCount);
        mesh.Normals.resize(vertexCount);
        mesh.Colors.resize(vertexCount);
        mesh.UVs.resize(vertexCount);
        for (size_t index = 0; index < vertexCount; ++index)
        {
            GridVertex vertex;
            std::memcpy(&vertex, output.data() + index * 11, sizeof(vertex));
            mesh.Positions[index] = vertex.Position;
            mesh.Normals[index] = vertex.Normal;
            mesh.Colors[index] = vertex.Color;
            mesh.UVs[index] = vertex.UV;
        }
        mesh.Indices.assign(output.begin() + vertexCount * 11, output.end());
        return mesh;
    }

    ProcStudio::Mesh GenerateAnalyticModel(const ProcStudio::ModelParams& parameters)
    {
        uint32_t mode = 8;
        size_t vertexCount = 65 * 33;
        size_t indexCount = 64 * 32 * 6;
        int level = 0;
        if (parameters.Kind == ProcStudio::ModelKind::Cube)
        {
            mode = 9;
            vertexCount = 24;
            indexCount = 36;
        }
        else if (parameters.Kind == ProcStudio::ModelKind::MengerSponge)
        {
            mode = 10;
            level = std::clamp(parameters.Level, 0, 3);
            size_t cubes = 1;
            for (int depth = 0; depth < level; ++depth)
            {
                cubes *= 20;
            }
            vertexCount = cubes * 24;
            indexCount = cubes * 36;
        }
        else if (parameters.Kind == ProcStudio::ModelKind::SierpinskiPyramid)
        {
            mode = 11;
            level = std::clamp(parameters.Level, 0, 7);
            vertexCount = (size_t(1) << (2 * level)) * 12;
            indexCount = vertexCount;
        }
        else if (parameters.Kind == ProcStudio::ModelKind::Rock)
        {
            mode = 12;
            level = std::clamp(parameters.Subdivisions, 0, 6);
            const size_t power = size_t(1) << (2 * level);
            vertexCount = 10 * power + 2;
            indexCount = 60 * power;
        }

        struct GridVertex
        {
            ProcStudio::Float3 Position;
            ProcStudio::Float3 Normal;
            ProcStudio::Float3 Color;
            ProcStudio::Float2 UV;
        };

        static_assert(sizeof(GridVertex) == 44);
        size_t outputWords = vertexCount * 11 + indexCount;
        if (mode == 12)
        {
            outputWords += indexCount + vertexCount * 4;
        }
        std::vector<uint32_t> output(outputWords);
        ProcStudio::TextureParams texture;
        texture.Noise = parameters.Noise;
        texture.Noise.Seed ^= parameters.Seed;
        texture.Turbulence = parameters.Displacement;
        int threadCount = static_cast<int>((std::max)(vertexCount, indexCount));
        if (mode == 12)
        {
            threadCount = 1;
        }
        auto packed = Pack(texture, mode, threadCount, 1, 1);
        packed.SourceWidth = static_cast<int>(vertexCount);
        packed.SourceHeight = static_cast<int>(indexCount);
        packed.ImageOctaves = level;
        g_Generator.Execute(packed, nullptr, 0, output.data(), output.size() * sizeof(uint32_t));
        ProcStudio::Mesh mesh;
        mesh.Positions.resize(vertexCount);
        mesh.Normals.resize(vertexCount);
        mesh.Colors.resize(vertexCount);
        mesh.UVs.resize(vertexCount);
        for (size_t index = 0; index < vertexCount; ++index)
        {
            GridVertex vertex;
            std::memcpy(&vertex, output.data() + index * 11, sizeof(vertex));
            mesh.Positions[index] = vertex.Position;
            mesh.Normals[index] = vertex.Normal;
            mesh.Colors[index] = vertex.Color;
            mesh.UVs[index] = vertex.UV;
        }
        mesh.Indices.assign(output.begin() + vertexCount * 11, output.begin() + vertexCount * 11 + indexCount);
        return mesh;
    }

    namespace
    {
        struct MeshCapacity
        {
            uint64_t Vertices = 0;
            uint64_t Indices = 0;
        };

        MeshCapacity LeafCapacity(const ProcStudio::LeafParams& parameters)
        {
            const int segments = std::clamp(parameters.Segments, 2, 16);
            int depth = std::clamp(parameters.Depth, 0, 4);
            if (parameters.Shape == ProcStudio::LeafShape::Card || parameters.Shape == ProcStudio::LeafShape::Simple)
            {
                depth = 0;
            }
            const int leaflets = std::clamp(parameters.Leaflets, 1, 12);
            int factor = 2 * leaflets + 1;
            if (parameters.Shape == ProcStudio::LeafShape::Fern)
            {
                factor = 4 * leaflets + 1;
            }
            else if (parameters.Shape == ProcStudio::LeafShape::Palmate)
            {
                factor = leaflets | 1;
            }
            uint64_t blades = 1;
            uint64_t axes = 0;
            for (int level = 0; level < depth; ++level)
            {
                axes += blades;
                blades *= factor;
            }
            if (parameters.Shape == ProcStudio::LeafShape::Palmate)
            {
                axes = 0;
            }
            return {blades * 6 * (segments + 1) + axes * 8, blades * 24 * segments + axes * 12};
        }

        ProcStudio::Mesh GenerateTreeOutput(uint32_t mode, const MeshCapacity& capacity, uint64_t placementCapacity,
                                            const void* source, size_t sourceBytes, bool orientForModel = false)
        {
            const uint64_t meshBytes = capacity.Vertices * 44 + capacity.Indices * 4;
            const uint64_t outputBytes = meshBytes * 2 + 64 + placementCapacity * 40;
            // ByteAddressBuffer offsets and the node ABI use 32-bit byte addresses.
            if (capacity.Vertices > UINT32_MAX || capacity.Indices > UINT32_MAX || placementCapacity > UINT32_MAX ||
                outputBytes > UINT32_MAX)
            {
                throw std::runtime_error("Procedural tree/leaf exceeds the Work Graph buffer address limit.");
            }
            std::vector<uint32_t> output(static_cast<size_t>(outputBytes / 4));
            auto packed = Pack({}, mode, 1, 1, 1);
            packed.SourceWidth = static_cast<int>(capacity.Vertices);
            packed.SourceHeight = static_cast<int>(capacity.Indices);
            packed.ImageOctaves = static_cast<int>(placementCapacity);
            packed.VerticalVariation = orientForModel ? 1.0f : 0.0f;
            g_Generator.Execute(packed, source, sourceBytes, output.data(), output.size() * sizeof(uint32_t));
            const size_t header = static_cast<size_t>(meshBytes / 4);
            const uint32_t vertexCount = output[header];
            const uint32_t indexCount = output[header + 1];
            if (output[header + 2] != 0 || vertexCount > capacity.Vertices || indexCount > capacity.Indices)
            {
                throw std::runtime_error("Procedural tree/leaf Work Graph output capacity exceeded.");
            }
            ProcStudio::Mesh mesh;
            mesh.Positions.resize(vertexCount);
            mesh.Normals.resize(vertexCount);
            mesh.Colors.resize(vertexCount);
            mesh.UVs.resize(vertexCount);
            for (size_t index = 0; index < vertexCount; ++index)
            {
                const auto* vertex = output.data() + index * 11;
                std::memcpy(&mesh.Positions[index], vertex, sizeof(ProcStudio::Float3));
                std::memcpy(&mesh.Normals[index], vertex + 3, sizeof(ProcStudio::Float3));
                std::memcpy(&mesh.Colors[index], vertex + 6, sizeof(ProcStudio::Float3));
                std::memcpy(&mesh.UVs[index], vertex + 9, sizeof(ProcStudio::Float2));
            }
            const size_t firstIndex = static_cast<size_t>(capacity.Vertices * 11);
            mesh.Indices.assign(output.begin() + firstIndex, output.begin() + firstIndex + indexCount);
            return mesh;
        }
    }

    ProcStudio::Mesh GenerateTree(const ProcStudio::ModelParams& parameters)
    {
        static_assert(sizeof(ProcStudio::ModelParams) == 204);
        static_assert(offsetof(ProcStudio::ModelParams, Seed) == 40);
        static_assert(offsetof(ProcStudio::ModelParams, Depth) == 60);
        static_assert(offsetof(ProcStudio::ModelParams, Leaf) == 112);
        static_assert(offsetof(ProcStudio::ModelParams, RadialSegments) == 180);
        const int depth = std::clamp(parameters.Depth, 1, 9);
        const uint64_t branches = static_cast<uint64_t>((std::max)(parameters.Branches, 0));
        std::array<uint64_t, 10> nodesAtDepth{};
        nodesAtDepth[depth] = 1;
        MeshCapacity capacity;
        for (int level = depth; level > 0; --level)
        {
            const uint64_t nodes = nodesAtDepth[level];
            const int64_t segments = (std::max)(int64_t(3), static_cast<int64_t>(parameters.RadialSegments) -
                                                                (static_cast<int64_t>(parameters.Depth) - level));
            // Reject impossible capacities before multiplication can overflow the allocation ABI.
            if (nodes > UINT32_MAX || segments > UINT32_MAX || nodes * uint64_t(segments) > UINT32_MAX)
            {
                throw std::runtime_error("Procedural tree branching exceeds the Work Graph capacity.");
            }
            capacity.Vertices += nodes * 2 * (segments + 1);
            capacity.Indices += nodes * 6 * segments;
            if (level > 1)
            {
                const uint64_t children = nodes * branches;
                if (children > UINT32_MAX)
                {
                    throw std::runtime_error("Procedural tree branching exceeds the Work Graph capacity.");
                }
                nodesAtDepth[level - 1] += children;
                if (level > 2)
                {
                    nodesAtDepth[level - 2] += nodes;
                }
            }
        }
        uint64_t placements = 0;
        if (parameters.Leaves)
        {
            placements = nodesAtDepth[1] * std::clamp(parameters.LeavesPerTip, 1, 12);
        }
        if (placements > 0)
        {
            const uint64_t triangleBudget = static_cast<uint64_t>((std::max)(parameters.LeafTriangleBudget, 1000));
            const uint64_t leafTriangles = (std::max)(triangleBudget, 24 * placements);
            capacity.Vertices += leafTriangles * 2;
            capacity.Indices += leafTriangles * 3;
        }
        return GenerateTreeOutput(13, capacity, placements, &parameters, sizeof(parameters));
    }

    ProcStudio::Mesh GenerateLeaf(const ProcStudio::LeafParams& parameters, uint32_t seed, bool orientForModel)
    {
        static_assert(sizeof(ProcStudio::LeafParams) == 68);

        struct LeafInput
        {
            ProcStudio::LeafParams Parameters;
            uint32_t Seed;
        };

        static_assert(sizeof(LeafInput) == 72);
        const LeafInput input{parameters, seed};
        return GenerateTreeOutput(14, LeafCapacity(parameters), 0, &input, sizeof(input), orientForModel);
    }

    ProcStudio::Image GenerateTexture(const ProcStudio::TextureParams& parameters, int width, int height)
    {
        ProcStudio::Image image;
        image.Width = (std::max)(width, 1);
        image.Height = (std::max)(height, 1);
        image.Pixels.resize(static_cast<size_t>(image.Width) * image.Height);
        const auto packed = Pack(parameters, 0, image.Width, image.Height, 1);
        g_Generator.Execute(packed, nullptr, 0, image.Pixels.data(), image.Pixels.size() * sizeof(ProcStudio::Float4));
        return image;
    }

    ProcStudio::Volume GenerateVolume(const ProcStudio::TextureParams& parameters, int extent)
    {
        ProcStudio::Volume volume;
        volume.Extent = std::clamp(extent, 8, 256);
        volume.Density.resize(static_cast<size_t>(volume.Extent) * volume.Extent * volume.Extent);
        const auto packed = Pack(parameters, 1, volume.Extent, volume.Extent, volume.Extent);
        g_Generator.Execute(packed, nullptr, 0, volume.Density.data(), volume.Density.size() * sizeof(float));
        return volume;
    }

    std::vector<ProcStudio::Float4> GenerateSolidVolume(const ProcStudio::TextureParams& parameters, int extent)
    {
        extent = std::clamp(extent, 8, 256);
        std::vector<ProcStudio::Float4> voxels(static_cast<size_t>(extent) * extent * extent);
        const auto packed = Pack(parameters, 2, extent, extent, extent);
        g_Generator.Execute(packed, nullptr, 0, voxels.data(), voxels.size() * sizeof(ProcStudio::Float4));
        return voxels;
    }

    ProcStudio::Volume VolumeFromImage(const ProcStudio::Image& image, int extent, int octaves, float verticalVariation,
                                       uint32_t seed)
    {
        ProcStudio::Volume volume;
        volume.Extent = std::clamp(extent, 8, 256);
        volume.Density.resize(static_cast<size_t>(volume.Extent) * volume.Extent * volume.Extent);
        auto packed = Pack({}, 3, volume.Extent, volume.Extent, volume.Extent);
        packed.SourceWidth = image.Width;
        packed.SourceHeight = image.Height;
        packed.ImageOctaves = octaves;
        packed.VerticalVariation = verticalVariation;
        packed.ImageSeed = seed;
        g_Generator.Execute(packed, image.Pixels.data(), image.Pixels.size() * sizeof(ProcStudio::Float4),
                            volume.Density.data(), volume.Density.size() * sizeof(float));
        return volume;
    }

    bool ValidateAuthoringOutputs()
    {
        using namespace ProcStudio;
        size_t comparedValues = 0;
        float maximumError = 0;
        int textureKind = -1;
        int textureBasis = -1;
        int textureFractal = -1;
        auto compare = [&](float actual, float expected, const char* label, float tolerance = 0.003f) {
            const float error = std::abs(actual - expected);
            if (!std::isfinite(actual) || !std::isfinite(expected) || error > tolerance)
            {
                WriteGraphicsLog(
                    "[WorkGraph procedural] %s actual=%g expected=%g error=%g kind=%d basis=%d fractal=%d\n", label,
                    actual, expected, error, textureKind, textureBasis, textureFractal);
                throw std::runtime_error(std::string("Procedural validation mismatch: ") + label);
            }
            maximumError = (std::max)(maximumError, error);
            ++comparedValues;
        };
        auto require = [](bool condition, const char* label) {
            if (!condition)
            {
                throw std::runtime_error(std::string("Procedural validation invariant: ") + label);
            }
        };
        try
        {
            constexpr int side = 8;
            for (int kind = 0; kind < static_cast<int>(TextureKind::Count); ++kind)
            {
                for (int basis = 0; basis < static_cast<int>(Basis::Count); ++basis)
                {
                    for (int fractal = 0; fractal < static_cast<int>(FractalType::Count); ++fractal)
                    {
                        textureKind = kind;
                        textureBasis = basis;
                        textureFractal = fractal;
                        TextureParams parameters;
                        parameters.Kind = static_cast<TextureKind>(kind);
                        parameters.Noise.BasisType = static_cast<Basis>(basis);
                        parameters.Noise.Fractal = static_cast<FractalType>(fractal);
                        parameters.Noise.Frequency = 4.5f;
                        parameters.Noise.Lacunarity = 2.5f;
                        parameters.RingCount = 8.5f;
                        // Both tile and solid coordinate paths are exercised below.
                        const Image image = ProceduralWorkGraph::GenerateTexture(parameters, side, side);
                        require(image.Pixels.size() == side * side, "texture extent");
                        for (int y = 0; y < side; ++y)
                        {
                            for (int x = 0; x < side; ++x)
                            {
                                const auto expected =
                                    TextureColor(parameters, (x + 0.5f) / side, (y + 0.5f) / side, 0, false);
                                const auto& actual = image.Pixels[y * side + x];
                                compare(actual.X, expected.X, "texture red");
                                compare(actual.Y, expected.Y, "texture green");
                                compare(actual.Z, expected.Z, "texture blue");
                                compare(actual.W, 1, "texture alpha");
                            }
                        }
                    }
                }
                TextureParams parameters;
                parameters.Kind = static_cast<TextureKind>(kind);
                for (TextureOutput output : {TextureOutput::Height, TextureOutput::Normal})
                {
                    parameters.Output = output;
                    const Image image = ProceduralWorkGraph::GenerateTexture(parameters, side, side);
                    auto scalar = [&](int x, int y) {
                        x = (x + side) % side;
                        y = (y + side) % side;
                        return TextureScalar(parameters, (x + 0.5f) / side, (y + 0.5f) / side, 0, false);
                    };
                    for (int y = 0; y < side; ++y)
                    {
                        for (int x = 0; x < side; ++x)
                        {
                            const auto& actual = image.Pixels[y * side + x];
                            if (output == TextureOutput::Height)
                            {
                                compare(actual.X, scalar(x, y), "height output");
                                compare(actual.Y, actual.X, "height grayscale green");
                                compare(actual.Z, actual.X, "height grayscale blue");
                            }
                            else
                            {
                                const float dx = (scalar(x + 1, y) - scalar(x - 1, y)) * parameters.NormalStrength;
                                const float dy = (scalar(x, y + 1) - scalar(x, y - 1)) * parameters.NormalStrength;
                                const float length = std::sqrt(dx * dx + dy * dy + 1);
                                compare(actual.X, -dx / length * 0.5f + 0.5f, "normal red");
                                compare(actual.Y, dy / length * 0.5f + 0.5f, "normal green");
                                compare(actual.Z, 1 / length * 0.5f + 0.5f, "normal blue");
                            }
                        }
                    }
                }
                const Volume volume = ProceduralWorkGraph::GenerateVolume(parameters, side);
                const auto solid = ProceduralWorkGraph::GenerateSolidVolume(parameters, side);
                require(volume.Density.size() == side * side * side, "density volume extent");
                require(solid.size() == volume.Density.size(), "solid volume extent");
                for (int index = 0; index < side * side * side; ++index)
                {
                    const float x = (index % side + 0.5f) / side;
                    const float y = ((index / side) % side + 0.5f) / side;
                    const float z = (index / (side * side) + 0.5f) / side;
                    compare(volume.Density[index], TextureScalar(parameters, x, y, z, false), "density voxel");
                    const auto expected = TextureColor(parameters, x, y, z, true);
                    compare(solid[index].X, expected.X, "solid red");
                    compare(solid[index].Y, expected.Y, "solid green");
                    compare(solid[index].Z, expected.Z, "solid blue");
                    compare(solid[index].W, 1, "solid alpha");
                }
            }
            TextureParams imageParameters;
            const Image source = ProceduralWorkGraph::GenerateTexture(imageParameters, side, side);
            const Volume extruded = ProceduralWorkGraph::VolumeFromImage(source, side, 1, 0, 0);
            // Hash(0)==0; a single octave with zero variation has no random offset.
            for (int index = 0; index < side * side * side; ++index)
            {
                const float x = (index % side + 0.5f) / side;
                const float y = ((index / side) % side + 0.5f) / side;
                const float z = (index / (side * side) + 0.5f) / side;
                const auto first = source.Sample(x, z);
                const auto second = source.Sample(z + 0.37f, y);
                const float expected = std::clamp(
                    (first.X + first.Y + first.Z) / 3 * 0.7f + (second.X + second.Y + second.Z) / 3 * 0.3f, 0.0f, 1.0f);
                compare(extruded.Density[index], expected, "image volume voxel");
            }
            for (int method = 0; method <= static_cast<int>(Procedural::NoiseMethod::Composite); ++method)
            {
                Procedural::TerrainSettings settings;
                settings.Resolution = side;
                settings.Noise.Method = static_cast<Procedural::NoiseMethod>(method);
                Procedural::ApplyCommonNoiseParameters(settings.Noise);
                const auto heights = ProceduralWorkGraph::BuildHeightMap(settings);
                const auto vertices = ProceduralWorkGraph::BuildTerrainVertices(settings);
                require(heights.size() == (side + 1) * (side + 1), "terrain height extent");
                require(vertices.size() == side * side * 6, "terrain triangle vertices");
                for (int row = 0; row <= side; ++row)
                {
                    for (int column = 0; column <= side; ++column)
                    {
                        const float x = settings.Size * (static_cast<float>(column) / side - 0.5f);
                        const float z = settings.Size * (static_cast<float>(row) / side - 0.5f);
                        compare(heights[row * (side + 1) + column],
                                settings.BaseHeight + Procedural::SampleNoise(x, z, settings.Noise) * settings.Height,
                                "scene terrain height");
                    }
                }
                for (size_t index = 0; index < vertices.size(); ++index)
                {
                    const auto& vertex = vertices[index];
                    const int column = static_cast<int>(std::round(vertex.Tex.x * side));
                    const int row = static_cast<int>(std::round(vertex.Tex.y * side));
                    require(column >= 0 && column <= side && row >= 0 && row <= side, "scene terrain UV bounds");
                    compare(vertex.Pos.y, heights[row * (side + 1) + column], "scene terrain vertex height");
                    compare(vertex.Pos.x, settings.Size * (vertex.Tex.x - 0.5f), "scene terrain vertex x");
                    compare(vertex.Pos.z, settings.Size * (vertex.Tex.y - 0.5f), "scene terrain vertex z");
                    compare(std::sqrt(vertex.Normal.x * vertex.Normal.x + vertex.Normal.y * vertex.Normal.y +
                                      vertex.Normal.z * vertex.Normal.z),
                            1, "scene terrain normal");
                    compare(vertex.Color.w, 1, "scene terrain alpha");
                }
            }
            for (int kind = 0; kind < static_cast<int>(ModelKind::Count); ++kind)
            {
                const auto modelKind = static_cast<ModelKind>(kind);
                ModelParams parameters;
                parameters.Kind = modelKind;
                parameters.Resolution = side;
                parameters.Subdivisions = 2;
                parameters.Level = 2;
                parameters.Depth = 3;
                parameters.Branches = 2;
                parameters.LeavesPerTip = 1;
                parameters.LeafTriangleBudget = 1000;
                parameters.Leaf.Depth = 1;
                parameters.Leaf.Leaflets = 2;
                parameters.Leaf.Segments = 3;
                const Mesh mesh = GenerateModel(parameters);
                require(!mesh.Positions.empty() && !mesh.Indices.empty(), "model nonempty");
                require(mesh.Indices.size() % 3 == 0, "triangle index count");
                require(mesh.Normals.size() == mesh.Positions.size() && mesh.Colors.size() == mesh.Positions.size() &&
                            mesh.UVs.size() == mesh.Positions.size(),
                        "model attribute counts");
                for (uint32_t index : mesh.Indices)
                {
                    require(index < mesh.Positions.size(), "model index bounds");
                }
                for (size_t index = 0; index < mesh.Positions.size(); ++index)
                {
                    const auto& position = mesh.Positions[index];
                    const auto& normal = mesh.Normals[index];
                    const auto& color = mesh.Colors[index];
                    const auto& uv = mesh.UVs[index];
                    require(std::isfinite(position.X) && std::isfinite(position.Y) && std::isfinite(position.Z),
                            "finite model position");
                    require(std::isfinite(color.X) && std::isfinite(color.Y) && std::isfinite(color.Z) &&
                                std::isfinite(uv.X) && std::isfinite(uv.Y),
                            "finite model attributes");
                    compare(std::sqrt(normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z), 1,
                            "unit model normal");
                    if (modelKind == ModelKind::Terrain)
                    {
                        auto noise = parameters.Noise;
                        noise.Seed ^= parameters.Seed;
                        compare(position.Y, FractalNoise(noise, uv.X, uv.Y, 0.37f, false) * parameters.Height,
                                "terrain model height");
                    }
                    else if (modelKind == ModelKind::Sphere)
                    {
                        compare(std::sqrt(position.X * position.X + position.Y * position.Y + position.Z * position.Z),
                                1, "sphere radius");
                    }
                    else if (modelKind == ModelKind::Cube)
                    {
                        compare((std::max)({std::abs(position.X), std::abs(position.Y), std::abs(position.Z)}), 0.8f,
                                "cube bounds");
                    }
                    else if (modelKind == ModelKind::Rock)
                    {
                        const float radius =
                            std::sqrt(position.X * position.X + position.Y * position.Y + position.Z * position.Z);
                        auto noise = parameters.Noise;
                        noise.Seed ^= parameters.Seed;
                        const float displacement =
                            FractalNoise(noise, position.X / radius * 0.5f + 7, position.Y / radius * 0.5f + 3,
                                         position.Z / radius * 0.5f + 5, false);
                        compare(radius, 1 + (displacement - 0.5f) * 2 * parameters.Displacement, "rock displacement");
                        const float gray =
                            0.18f + 0.22f * FractalNoise(noise, position.X * 2, position.Y * 2, position.Z * 2, false);
                        compare(color.X, gray * 1.05f, "rock red");
                        compare(color.Y, gray, "rock green");
                        compare(color.Z, gray * 0.92f, "rock blue");
                    }
                    else if (modelKind == ModelKind::WaterSurface)
                    {
                        TextureParams water;
                        water.Kind = TextureKind::FractalWater;
                        water.Noise = parameters.Noise;
                        water.Noise.Seed ^= parameters.Seed;
                        water.Time = parameters.Time;
                        water.Tileable = false;
                        compare(position.Y,
                                (TextureScalar(water, uv.X, uv.Y, 0, false) - 0.5f) * 2 * parameters.WaveAmplitude,
                                "water model height");
                    }
                }
                WriteGraphicsLog("[WorkGraph procedural] model=%d vertices=%zu indices=%zu verified\n", kind,
                                 mesh.Positions.size(), mesh.Indices.size());
            }
            for (int shape = 0; shape < static_cast<int>(LeafShape::Count); ++shape)
            {
                LeafParams leaf;
                leaf.Shape = static_cast<LeafShape>(shape);
                leaf.Depth = 1;
                leaf.Leaflets = 2;
                leaf.Segments = 3;
                const auto local = ProceduralWorkGraph::GenerateLeaf(leaf, 7);
                const auto oriented = ProceduralWorkGraph::GenerateLeaf(leaf, 7, true);
                const auto capacity = LeafCapacity(leaf);
                require(!local.Positions.empty() && local.Positions.size() <= capacity.Vertices &&
                            local.Indices.size() <= capacity.Indices,
                        "leaf capacity");
                require(local.Positions.size() == oriented.Positions.size() && local.Indices == oriented.Indices,
                        "leaf model topology");
                for (size_t index = 0; index < local.Positions.size(); ++index)
                {
                    const auto& sourcePosition = local.Positions[index];
                    const auto& modelPosition = oriented.Positions[index];
                    compare(modelPosition.X, sourcePosition.X, "leaf orientation x");
                    compare(modelPosition.Y, sourcePosition.Z, "leaf orientation y");
                    compare(modelPosition.Z, -sourcePosition.Y, "leaf orientation z");
                    const auto& sourceNormal = local.Normals[index];
                    const auto& modelNormal = oriented.Normals[index];
                    compare(modelNormal.X, sourceNormal.X, "leaf normal orientation x");
                    compare(modelNormal.Y, sourceNormal.Z, "leaf normal orientation y");
                    compare(modelNormal.Z, -sourceNormal.Y, "leaf normal orientation z");
                    compare(std::sqrt(sourceNormal.X * sourceNormal.X + sourceNormal.Y * sourceNormal.Y +
                                      sourceNormal.Z * sourceNormal.Z),
                            1, "leaf unit normal");
                }
                ModelParams tree;
                tree.Kind = ModelKind::FractalTree;
                tree.Depth = 2;
                tree.Branches = 2;
                tree.LeavesPerTip = 1;
                tree.LeafTriangleBudget = 1000;
                tree.Leaf = leaf;
                const auto treeMesh = GenerateTree(tree);
                require(!treeMesh.Positions.empty() && treeMesh.Indices.size() % 3 == 0, "tree leaf shape topology");
                for (uint32_t index : treeMesh.Indices)
                {
                    require(index < treeMesh.Positions.size(), "tree leaf index bounds");
                }
                WriteGraphicsLog("[WorkGraph procedural] leafShape=%d leafVertices=%zu treeVertices=%zu verified\n",
                                 shape, local.Positions.size(), treeMesh.Positions.size());
            }
            ModelParams bakeModel;
            bakeModel.Kind = ModelKind::Cube;
            auto baked = GenerateModel(bakeModel);
            TextureParams bakeParameters;
            bakeParameters.Kind = TextureKind::Marble;
            ProcStudio::Float3 bakeMinimum;
            ProcStudio::Float3 bakeMaximum;
            baked.Bounds(bakeMinimum, bakeMaximum);
            const float bakeExtent = (std::max)({bakeMaximum.X - bakeMinimum.X, bakeMaximum.Y - bakeMinimum.Y,
                                                 bakeMaximum.Z - bakeMinimum.Z, 1e-4f});
            BakeSolidVertexColors(baked, bakeParameters, 1.25f);
            for (size_t index = 0; index < baked.Positions.size(); ++index)
            {
                const auto& position = baked.Positions[index];
                const float x = (position.X - bakeMinimum.X) * (1.25f / bakeExtent);
                const float y = (position.Y - bakeMinimum.Y) * (1.25f / bakeExtent);
                const float z = (position.Z - bakeMinimum.Z) * (1.25f / bakeExtent);
                const auto expected =
                    TextureColor(bakeParameters, x - std::floor(x), y - std::floor(y), z - std::floor(z), true);
                compare(baked.Colors[index].X, expected.X, "solid vertex bake red");
                compare(baked.Colors[index].Y, expected.Y, "solid vertex bake green");
                compare(baked.Colors[index].Z, expected.Z, "solid vertex bake blue");
            }
            std::vector<MotionInput> motionSamples;
            for (uint32_t method = 0; method < 3; ++method)
            {
                for (double time : {0.0, 0.125, 1.0, 17.5, -3.75, 8192.0, 123456.789})
                {
                    MotionInput input;
                    input.Time = time;
                    input.Speed = 0.75f;
                    input.Method = method;
                    input.Amplitude = {1.2f, -2.5f, 0.75f};
                    input.Seed = 42;
                    motionSamples.push_back(input);
                }
            }
            const auto motionStart = std::chrono::steady_clock::now();
            const auto offsets = SampleMotionBatch(motionSamples);
            const double motionMilliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - motionStart).count();
            WriteGraphicsLog("[WorkGraph procedural] motionBatch samples=%zu synchronizedCpuMs=%.3f\n",
                             motionSamples.size(), motionMilliseconds);
            require(offsets.size() == motionSamples.size(), "motion sample count");
            for (size_t index = 0; index < offsets.size(); ++index)
            {
                const auto& input = motionSamples[index];
                const auto expected = Procedural::SampleMotion(static_cast<Procedural::MotionMethod>(input.Method),
                                                               input.Time, input.Speed, input.Amplitude, input.Seed);
                compare(offsets[index].X, expected.X, "object motion x");
                compare(offsets[index].Y, expected.Y, "object motion y");
                compare(offsets[index].Z, expected.Z, "object motion z");
            }
            WriteGraphicsLog("[WorkGraph procedural] PASS compared=%zu maximumError=%g\n", comparedValues,
                             maximumError);
            return true;
        }
        catch (const std::exception& exception)
        {
            WriteGraphicsLog("[WorkGraph procedural] FAIL %s\n", exception.what());
            return false;
        }
    }
}
