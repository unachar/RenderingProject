#include "pch.h"
#include "proceduralworkgraph.h"
#include "graphicsdevice.h"
#include "workgraphruntime.h"
#include <cstring>
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
        };

        static_assert(offsetof(Parameters, NormalStrength) == 168);
        static_assert(offsetof(Parameters, ImageSeed) == 188);

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
                Initialize(device);
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
                if (!m_Graph.DispatchCpu(m_Commands.Get(), 0, record, sizeof(record)))
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

            void Initialize(ID3D12Device* device)
            {
                if (m_Device.Get() == device && m_Graph.IsInitialized())
                {
                    return;
                }
                m_Graph.Reset();
                m_Device = device;
                CD3DX12_ROOT_PARAMETER roots[3];
                roots[0].InitAsShaderResourceView(0);
                roots[1].InitAsShaderResourceView(1);
                roots[2].InitAsUnorderedAccessView(0);
                CD3DX12_ROOT_SIGNATURE_DESC description(3, roots);
                ComPtr<ID3DBlob> serialized;
                ComPtr<ID3DBlob> errors;
                Require(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
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
                if (!m_Event || !m_Graph.Initialize(device, m_Root.Get(), L"shader/hlsl/ProceduralStudioWorkGraph.hlsl",
                                                    L"Generate"))
                {
                    throw std::runtime_error("Cannot initialize procedural authoring Work Graph.");
                }
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
            WorkGraph::ComputeGraph m_Graph;
        };

        Generator g_Generator;
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
}
