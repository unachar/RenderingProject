#include "assets/materialgraphcompiler.h"
#include "d3dx12.h"
#include <dxgi1_6.h>
#include <wrl.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void Succeeded(HRESULT result, const char* message)
    {
        Require(SUCCEEDED(result), message);
    }

    struct Inputs
    {
        std::array<float, 4> UVTime{0.25f, 0.5f, 0.0f, 0.0f};
        std::array<float, 4> Position{0.0f, 0.0f, 0.0f, 0.0f};
        std::array<float, 4> Normal{0.0f, 0.0f, 1.0f, 0.0f};
        std::array<float, 4> Camera{0.0f, 0.0f, 1.0f, 0.0f};
    };

    struct GraphCase
    {
        const char* Name;
        MaterialGraph Graph;
        uint64_t Target = 0;
    };

    std::vector<GraphCase> Cases()
    {
        std::vector<GraphCase> cases;
        GraphCase panner{"panner"};
        panner.Graph.AddNode(MaterialNodeKind::Output, 0, 0);
        panner.Target = panner.Graph.AddNode(MaterialNodeKind::Panner, 0, 0);
        const auto speed = panner.Graph.AddNode(MaterialNodeKind::Constant2Vector, 0, 0);
        panner.Graph.FindNode(speed)->Value = {0.125f, -0.25f, 0.0f, 0.0f};
        std::string error;
        Require(panner.Graph.Connect(panner.Graph.FindNode(speed)->OutputPin,
                                    panner.Graph.FindNode(panner.Target)->InputPins[2], error), error.c_str());
        cases.push_back(std::move(panner));

        GraphCase fresnel{"fresnel"};
        fresnel.Graph.AddNode(MaterialNodeKind::Output, 0, 0);
        fresnel.Target = fresnel.Graph.AddNode(MaterialNodeKind::Fresnel, 0, 0);
        cases.push_back(std::move(fresnel));

        GraphCase position{"position"};
        position.Graph.AddNode(MaterialNodeKind::Output, 0, 0);
        position.Target = position.Graph.AddNode(MaterialNodeKind::WorldPosition, 0, 0);
        cases.push_back(std::move(position));
        return cases;
    }

    void WriteShaders(const std::vector<GraphCase>& cases)
    {
        for (const auto& test : cases)
        {
            const auto program = MaterialGraphCompiler::GenerateRuntime(test.Graph, test.Target);
            Require(program.Success, program.Error.c_str());
            std::ofstream shader(std::string("build/material-graph-gpu/") + test.Name + ".hlsl");
            shader << "Texture2D<float4> TestHeap[512] : register(t0,space1);\n"
                      "Texture2D<float4> LoadMaterialTexture(uint index) { return TestHeap[index]; }\n"
                      "SamplerState TestSampler : register(s0);\n"
                      "cbuffer TestInput : register(b0) { float4 TestUVTime; float4 TestPosition; float4 TestNormal; float4 TestCamera; };\n"
                      "RWStructuredBuffer<float4> Result : register(u0);\n"
                      "namespace RuntimeGraph {\n"
                   << program.Hlsl
                   << "\n}\n[numthreads(1,1,1)] void main() {\n"
                      "RuntimeGraph::MaterialGraphContext c = (RuntimeGraph::MaterialGraphContext)0;\n"
                      "c.UV=TestUVTime.xy; c.Time=TestUVTime.z; c.ParameterTexture=0;\n"
                      "c.WorldPosition=TestPosition.xyz; c.WorldNormal=TestNormal.xyz; c.CameraVector=TestCamera.xyz;\n"
                      "c.Tangent=float3(1,0,0); c.Bitangent=float3(0,-1,0);\n"
                      "Result[0]=RuntimeGraph::EvaluateMaterialNode(c,TestSampler); }\n";
        }
    }

    class Gpu
    {
      public:
        explicit Gpu(bool warp)
        {
            ComPtr<IDXGIFactory4> factory;
            Succeeded(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "create DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            if (warp)
            {
                Succeeded(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "select WARP adapter");
            }
            Succeeded(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device)),
                      "create D3D12 device");
            D3D12_COMMAND_QUEUE_DESC queue{};
            Succeeded(m_Device->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_Queue)), "create command queue");
            Succeeded(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_Allocator)),
                      "create command allocator");
            Succeeded(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&m_Commands)), "create command list");
            Succeeded(m_Commands->Close(), "close initial command list");
            Succeeded(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence)), "create fence");
            m_Event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            Require(m_Event != nullptr, "create completion event");

            CD3DX12_DESCRIPTOR_RANGE textures(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 512, 0, 1);
            CD3DX12_ROOT_PARAMETER parameters[3];
            parameters[0].InitAsDescriptorTable(1, &textures);
            parameters[1].InitAsConstantBufferView(0);
            parameters[2].InitAsUnorderedAccessView(0);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0);
            CD3DX12_ROOT_SIGNATURE_DESC description(3, parameters, 1, &sampler);
            ComPtr<ID3DBlob> bytes;
            ComPtr<ID3DBlob> errors;
            Succeeded(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors),
                      "serialize root signature");
            Succeeded(m_Device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                                   IID_PPV_ARGS(&m_Root)), "create root signature");
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = 512;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Succeeded(m_Device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_Heap)), "create material heap");
        }

        ~Gpu()
        {
            if (m_Event)
            {
                CloseHandle(m_Event);
            }
        }

        std::array<float, 4> Evaluate(const GraphCase& test, const Inputs& inputs,
                                      std::vector<std::array<float, 4>> parameters)
        {
            if (parameters.empty())
            {
                parameters.push_back({});
            }
            ComPtr<ID3DBlob> code;
            const auto path = std::filesystem::path("build/material-graph-gpu") / (std::string(test.Name) + ".cso");
            Succeeded(D3DReadFileToBlob(path.c_str(), &code), "load compiled graph");
            D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
            description.pRootSignature = m_Root.Get();
            description.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            ComPtr<ID3D12PipelineState> pipeline;
            Succeeded(m_Device->CreateComputePipelineState(&description, IID_PPV_ARGS(&pipeline)), "create graph pipeline");

            const auto textureDescription = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,
                                                                         parameters.size(), 1, 1, 1);
            const CD3DX12_HEAP_PROPERTIES defaults(D3D12_HEAP_TYPE_DEFAULT);
            ComPtr<ID3D12Resource> texture;
            Succeeded(m_Device->CreateCommittedResource(&defaults, D3D12_HEAP_FLAG_NONE, &textureDescription,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&texture)), "create graph parameter texture");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 uploadBytes = 0;
            m_Device->GetCopyableFootprints(&textureDescription, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBytes);
            auto upload = Buffer(uploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            Write(upload.Get(), parameters.data(), parameters.size() * sizeof(parameters[0]));
            auto constants = Buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            Write(constants.Get(), &inputs, sizeof(inputs));
            auto result = Buffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback = Buffer(16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = textureDescription.Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            const UINT increment = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for (UINT index = 0; index < 512; ++index)
            {
                const CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_Heap->GetCPUDescriptorHandleForHeapStart(), index, increment);
                m_Device->CreateShaderResourceView(index == 0 ? texture.Get() : nullptr, &view, handle);
            }
            Succeeded(m_Allocator->Reset(), "reset allocator");
            Succeeded(m_Commands->Reset(m_Allocator.Get(), pipeline.Get()), "reset graph commands");
            const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(), footprint);
            const CD3DX12_TEXTURE_COPY_LOCATION destination(texture.Get(), 0);
            m_Commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_Commands->ResourceBarrier(1, &barrier);
            ID3D12DescriptorHeap* heaps[] = {m_Heap.Get()};
            m_Commands->SetDescriptorHeaps(1, heaps);
            m_Commands->SetComputeRootSignature(m_Root.Get());
            m_Commands->SetComputeRootDescriptorTable(0, m_Heap->GetGPUDescriptorHandleForHeapStart());
            m_Commands->SetComputeRootConstantBufferView(1, constants->GetGPUVirtualAddress());
            m_Commands->SetComputeRootUnorderedAccessView(2, result->GetGPUVirtualAddress());
            m_Commands->Dispatch(1, 1, 1);
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
            m_Commands->ResourceBarrier(1, &barrier);
            m_Commands->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, 16);
            Succeeded(m_Commands->Close(), "close graph commands");
            ID3D12CommandList* lists[] = {m_Commands.Get()};
            m_Queue->ExecuteCommandLists(1, lists);
            Succeeded(m_Queue->Signal(m_Fence.Get(), ++m_Submission), "signal graph completion");
            Succeeded(m_Fence->SetEventOnCompletion(m_Submission, m_Event), "wait for graph completion");
            Require(WaitForSingleObject(m_Event, 30000) == WAIT_OBJECT_0, "GPU graph timed out");
            std::array<float, 4> values{};
            void* mapped = nullptr;
            const D3D12_RANGE range{0, sizeof(values)};
            Succeeded(readback->Map(0, &range, &mapped), "read graph result");
            std::memcpy(values.data(), mapped, sizeof(values));
            const D3D12_RANGE untouched{0, 0};
            readback->Unmap(0, &untouched);
            return values;
        }

      private:
        ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                      D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
        {
            const CD3DX12_HEAP_PROPERTIES heap(type);
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
            ComPtr<ID3D12Resource> buffer;
            Succeeded(m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                                         IID_PPV_ARGS(&buffer)), "create graph buffer");
            return buffer;
        }

        static void Write(ID3D12Resource* resource, const void* source, size_t size)
        {
            void* mapped = nullptr;
            const D3D12_RANGE empty{0, 0};
            Succeeded(resource->Map(0, &empty, &mapped), "map graph upload");
            std::memcpy(mapped, source, size);
            resource->Unmap(0, nullptr);
        }

        ComPtr<ID3D12Device> m_Device;
        ComPtr<ID3D12CommandQueue> m_Queue;
        ComPtr<ID3D12CommandAllocator> m_Allocator;
        ComPtr<ID3D12GraphicsCommandList> m_Commands;
        ComPtr<ID3D12Fence> m_Fence;
        ComPtr<ID3D12RootSignature> m_Root;
        ComPtr<ID3D12DescriptorHeap> m_Heap;
        HANDLE m_Event = nullptr;
        UINT64 m_Submission = 0;
    };

    void Near(float actual, float expected, const char* message)
    {
        Require(std::isfinite(actual) && std::abs(actual - expected) < 1e-5f, message);
    }
}

int main(int argc, char** argv)
{
    try
    {
        const auto cases = Cases();
        if (argc > 1 && std::string(argv[1]) == "--generate")
        {
            WriteShaders(cases);
            return 0;
        }
        Gpu gpu(argc > 1 && std::string(argv[1]) == "--warp");
        Inputs inputs;
        const auto panner = MaterialGraphCompiler::GenerateRuntime(cases[0].Graph, cases[0].Target);
        auto values = gpu.Evaluate(cases[0], inputs, panner.Parameters);
        Near(values[0], 0.25f, "Panner starts at supplied U");
        Near(values[1], 0.5f, "Panner starts at supplied V");
        inputs.UVTime[2] = 2.0f;
        values = gpu.Evaluate(cases[0], inputs, panner.Parameters);
        Near(values[0], 0.5f, "Time changes Panner U on the GPU");
        Near(values[1], 0.0f, "Time changes Panner V on the GPU");
        auto edited = panner.Parameters;
        edited[0][0] = 0.25f;
        values = gpu.Evaluate(cases[0], inputs, edited);
        Near(values[0], 0.75f, "parameter edits change GPU output without recompiling the shader");
        values = gpu.Evaluate(cases[1], inputs, {});
        Near(values[0], 0.0f, "Fresnel at normal incidence");
        inputs.Camera = {1, 0, 0, 0};
        values = gpu.Evaluate(cases[1], inputs, {});
        Near(values[0], 1.0f, "camera direction changes Fresnel on the GPU");
        inputs.Position = {1.25f, -2.5f, 0.75f, 0};
        values = gpu.Evaluate(cases[2], inputs, {});
        for (size_t component = 0; component < 3; ++component)
        {
            Near(values[component], inputs.Position[component], "world position reaches the GPU graph");
        }
        std::cout << "Material graph GPU parameter/time/view/world tests: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
