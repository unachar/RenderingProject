// Opt-in D3D12 parity executable. Takes the precompiled parity CS path as argv[1].
// No application, mesh-shader hardware, or CPU expansion in the runtime is required.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include "virtualgeometry/virtualgeometrycompression.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
using Microsoft::WRL::ComPtr;
using namespace VirtualGeometryCompression;
void Check(HRESULT result)
{
    if (FAILED(result))
    {
        throw std::runtime_error("D3D12 parity operation failed: " + std::to_string(uint32_t(result)));
    }
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = description.DepthOrArraySize = description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}
class Harness
{
public:
    explicit Harness(const char* shaderPath)
    {
        ComPtr<ID3D12Debug> debug;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> validation;
        Check(debug.As(&validation));
        validation->SetEnableGPUBasedValidation(TRUE);
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapters1(0, &adapter));
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_Device)));
        D3D12_COMMAND_QUEUE_DESC queue{};
        Check(m_Device->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_Queue)));
        Check(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence)));
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[1].Descriptor.ShaderRegister = 0;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.Num32BitValues = 2;
        parameters[2].Constants.ShaderRegister = 0;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = 3;
        root.pParameters = parameters;
        ComPtr<ID3DBlob> signature, errors;
        Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors));
        Check(m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_Root)));
        std::ifstream file(shaderPath, std::ios::binary);
        std::vector<char> shader((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (shader.empty())
        {
            throw std::runtime_error("Missing precompiled parity compute shader");
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = m_Root.Get();
        pipeline.CS = {shader.data(), shader.size()};
        Check(m_Device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_Pipeline)));
    }
    void Run(const std::vector<uint8_t>& encoded, uint32_t vertices, uint32_t triangles)
    {
        std::string error;
        if (!Validate(encoded, vertices, triangles, error))
        {
            throw std::runtime_error(error);
        }
        uint64_t outputBytes = uint64_t(vertices)*80 + uint64_t(triangles)*16;
        auto upload = Buffer(m_Device.Get(), encoded.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto output = Buffer(m_Device.Get(), outputBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback = Buffer(m_Device.Get(), outputBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        Check(upload->Map(0, &noRead, &mapped));
        std::memcpy(mapped, encoded.data(), encoded.size());
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        Check(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        Check(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), m_Pipeline.Get(), IID_PPV_ARGS(&commands)));
        commands->SetComputeRootSignature(m_Root.Get());
        commands->SetComputeRootShaderResourceView(0, upload->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        uint32_t constants[2] = {vertices, triangles};
        commands->SetComputeRoot32BitConstants(2, 2, constants, 0);
        commands->Dispatch((std::max(vertices, triangles) + 63)/64, 1, 1);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = output.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        commands->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, outputBytes);
        Check(commands->Close());
        ID3D12CommandList* lists[] = {commands.Get()};
        m_Queue->ExecuteCommandLists(1, lists);
        Check(m_Queue->Signal(m_Fence.Get(), ++m_Submitted));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
        {
            throw std::runtime_error("Parity fence event creation failed");
        }
        HRESULT eventResult = m_Fence->SetEventOnCompletion(m_Submitted, event);
        DWORD waited = WAIT_FAILED;
        if (SUCCEEDED(eventResult))
        {
            waited = WaitForSingleObject(event, 30000);
        }
        CloseHandle(event);
        Check(eventResult);
        if (waited != WAIT_OBJECT_0)
        {
            // Process ends on failure; do not continue or recycle timed-out GPU resources.
            std::cerr << "Parity GPU wait timed out\n";
            ExitProcess(2);
        }
        D3D12_RANGE range{0, SIZE_T(outputBytes)};
        Check(readback->Map(0, &range, &mapped));
        std::vector<uint8_t> bytes(size_t(outputBytes), 0);
        std::memcpy(bytes.data(), mapped, bytes.size());
        readback->Unmap(0, &noRead);
        for (uint32_t index = 0; index < vertices; ++index)
        {
            auto cpu = DecodeVertexCPU(encoded, index);
            float actual[20];
            std::memcpy(actual, bytes.data() + index*80, sizeof(actual));
            Compare(cpu.Position.data(), actual, 3, 0.0001f);
            Compare(cpu.Normal.data(), actual + 4, 3, 0.0001f);
            Compare(cpu.Tangent.data(), actual + 8, 4, 0.0001f);
            Compare(cpu.Texcoords[0].data(), actual + 12, 2, 0.0001f);
            Compare(cpu.Color.data(), actual + 16, 4, 0.0001f);
        }
        for (uint32_t index = 0; index < triangles; ++index)
        {
            auto cpu = LoadTriangleCPU(encoded, index);
            uint32_t actual[4];
            std::memcpy(actual, bytes.data() + vertices*80 + index*16, sizeof(actual));
            if (!std::equal(cpu.begin(), cpu.end(), actual))
            {
                throw std::runtime_error("CPU/GPU index mismatch");
            }
        }
        ComPtr<ID3D12InfoQueue> diagnostics;
        Check(m_Device.As(&diagnostics));
        for (uint64_t index = 0; index < diagnostics->GetNumStoredMessages(); ++index)
        {
            SIZE_T size = 0;
            Check(diagnostics->GetMessage(index, nullptr, &size));
            std::vector<uint8_t> storage(size);
            auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(diagnostics->GetMessage(index, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                throw std::runtime_error(message->pDescription);
            }
        }
    }
private:
    static void Compare(const float* expected, const float* actual, uint32_t count, float tolerance)
    {
        for (uint32_t component = 0; component < count; ++component)
        {
            if (!std::isfinite(actual[component]) || std::abs(expected[component] - actual[component]) > tolerance)
            {
                throw std::runtime_error("CPU/GPU attribute mismatch at component " + std::to_string(component));
            }
        }
    }
    ComPtr<ID3D12Device> m_Device;
    ComPtr<ID3D12CommandQueue> m_Queue;
    ComPtr<ID3D12Fence> m_Fence;
    ComPtr<ID3D12RootSignature> m_Root;
    ComPtr<ID3D12PipelineState> m_Pipeline;
    uint64_t m_Submitted = 0;
};
}
int main(int argc, char** argv)
{
    try
    {
        if (argc != 2)
        {
            throw std::runtime_error("Usage: virtualgeometry_decoder_parity_tests <parity.cs.cso>");
        }
        Harness gpu(argv[1]);
        for (uint32_t count : {64u, 255u, 256u, 257u})
        {
            std::vector<VirtualGeometryVertex> vertices(count);
            std::vector<std::array<float, 4>> tangents(count);
            for (uint32_t index = 0; index < count; ++index)
            {
                auto& vertex = vertices[index];
                vertex.Position[0] = -0.5f + float(index)/float(count - 1);
                vertex.Position[1] = float(index % 7)/7;
                vertex.Normal[0] = std::cos(float(index));
                vertex.Normal[1] = std::sin(float(index));
                vertex.Normal[2] = 0.25f;
                vertex.TexCoord[0] = -1.25f;
                vertex.TexCoord[1] = 2.5f;
                std::fill(std::begin(vertex.Diffuse), std::end(vertex.Diffuse), 0.5f);
                tangents[index] = {0, 0, 1, -1};
            }
            Input input;
            input.Vertices = vertices;
            input.Tangents = tangents;
            for (bool compressed : {true, false})
            {
                VGCompressionSettings settings;
                settings.AllowPositionQuantization = compressed;
                settings.AllowNormalCompression = compressed;
                settings.AllowTangentCompression = compressed;
                settings.AllowHalfTexcoord = compressed;
                Plan plan;
                std::string error;
                std::vector<uint32_t> indices{0, count - 1, 1};
                std::vector<uint8_t> bytes;
                if (!Select(input, settings, plan, error) || !EncodeCluster(input, plan, 0, count, indices, bytes, error))
                {
                    throw std::runtime_error(error);
                }
                gpu.Run(bytes, count, 1);
            }
        }
        std::cout << "PASS: compressed/full precision CPU/GPU attributes and UInt8/UInt16 indices\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
