#pragma once
#include <array>
#include <limits>
#include "../source/rendering/gpuscene/gpuscenetypes.h"

// Included by the native AA runner; reuses its D3D12 device, bounded waits and readback buffers.
void TestVisibilityReconstruction(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* commands,
    ID3D12Resource* output, ID3D12Resource* readback, ID3D12Fence* fence, HANDLE completed)
{
    struct Vertex
    {
        float Position[3];
        float Normal[3];
        float Uv[2];
        float Color[4];
    };
    static_assert(sizeof(Vertex) == 48);
    using Record = std::array<uint32_t, 4>;
    std::array<Vertex, 18> vertices{};
    std::array<uint32_t, 18> indices{};
    std::array<Record, 6> geometry{};
    std::array<GPUInstanceData, 2> instances{};
    std::array<float, 128> camera{};
    for (unsigned matrix = 0; matrix < 3; ++matrix)
    {
        for (unsigned diagonal = 0; diagonal < 4; ++diagonal)
        {
            camera[matrix * 16 + diagonal * 5] = 1;
        }
    }
    for (auto& instance : instances)
    {
        DirectX::XMStoreFloat4x4(&instance.World, DirectX::XMMatrixIdentity());
    }
    for (unsigned mesh = 0; mesh < geometry.size(); ++mesh)
    {
        geometry[mesh] = {mesh * 3, 3, 0, 1};
        vertices[mesh * 3] = {{-1, -1, 0.5f}, {0, 1, 0}, {}, {}};
        vertices[mesh * 3 + 1] = {{1, -1, 0.5f}, {1, 1, 0}, {}, {}};
        vertices[mesh * 3 + 2] = {{-1, 1, 0.5f}, {0, 1, 1}, {}, {}};
    }
    // A separately indexed adjacent triangle can have unrelated shading normals.
    vertices[3] = {{1, 1, 0.5f}, {1, 0, 0}, {}, {}};
    vertices[4] = {{-1, 1, 0.5f}, {1, 1, 0}, {}, {}};
    vertices[5] = {{1, -1, 0.5f}, {1, 0, 1}, {}, {}};
    vertices[7] = vertices[6];
    vertices[8] = vertices[6];
    geometry[3][3] = 0;
    vertices[12].Position[2] = 1;
    vertices[13].Position[2] = 2;
    vertices[14].Position[2] = 1;
    for (unsigned vertex = 15; vertex < 18; ++vertex)
    {
        vertices[vertex].Normal[0] = std::numeric_limits<float>::quiet_NaN();
    }
    for (unsigned index = 0; index < indices.size(); ++index)
    {
        indices[index] = index;
    }

    auto upload = [&](const void* bytes, size_t size) {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(size);
        ComPtr<ID3D12Resource> resource;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)), "visibility upload allocation");
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        Check(resource->Map(0, &noRead, &mapped), "visibility upload map");
        memcpy(mapped, bytes, size);
        resource->Unmap(0, nullptr);
        return resource;
    };
    auto indexBuffer = upload(indices.data(), sizeof(indices));
    auto geometryBuffer = upload(geometry.data(), sizeof(geometry));
    std::array<std::array<float, 32>, 3> regularResults{};
    std::array<float, 8> perspectiveResult{};
    UINT64 serial = 10;
    for (unsigned run = 0; run < 3; ++run)
    {
        if (run == 1)
        {
            for (unsigned vertex = 3; vertex < 6; ++vertex)
            {
                vertices[vertex].Normal[0] = -1;
                vertices[vertex].Normal[1] = 0;
                vertices[vertex].Normal[2] = 0;
            }
            // Perturb the neighboring instance too, while keeping instance 0 untouched.
            instances[1].World._11 = 2;
            instances[1].World._22 = 0.5f;
        }
        if (run == 2)
        {
            for (unsigned index = 32; index < 48; ++index)
            {
                camera[index] = 0;
            }
            // Column-major upload of a row-vector projection with clip.w = world.z.
            camera[32] = 1;
            camera[37] = 1;
            camera[42] = 0.5f;
            camera[46] = 1;
        }
        auto vertexBuffer = upload(vertices.data(), sizeof(vertices));
        auto instanceBuffer = upload(instances.data(), sizeof(instances));
        auto cameraBuffer = upload(camera.data(), sizeof(camera));
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        const char* entry = "TestVisibility";
        if (run == 2)
        {
            entry = "TestPerspective";
        }
        HRESULT result = DxcCompileFromFileCompat(L"tests/shaders/visibility_reconstruction_fixture.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, "cs_6_5", D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &code, &errors);
        if (FAILED(result) && errors)
        {
            fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        }
        Check(result, "production visibility fixture compile");
        ComPtr<ID3D12RootSignature> root;
        Check(device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(),
            IID_PPV_ARGS(&root)), "visibility root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root.Get();
        pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pso;
        Check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)), "visibility compute PSO");
        Check(allocator->Reset(), "visibility allocator reset");
        Check(commands->Reset(allocator, pso.Get()), "visibility command reset");
        commands->SetComputeRootSignature(root.Get());
        commands->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(1, instanceBuffer->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(2, vertexBuffer->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(3, indexBuffer->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(4, geometryBuffer->GetGPUVirtualAddress());
        commands->SetComputeRootConstantBufferView(5, cameraBuffer->GetGPUVirtualAddress());
        commands->Dispatch(1, 1, 1);
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->ResourceBarrier(1, &barrier);
        commands->CopyResource(readback, output);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->ResourceBarrier(1, &barrier);
        Check(commands->Close(), "visibility command close");
        ID3D12CommandList* lists[] = {commands};
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence, serial), "visibility queue signal");
        Check(fence->SetEventOnCompletion(serial++, completed), "visibility completion event");
        if (WaitForSingleObject(completed, 30000) != WAIT_OBJECT_0)
        {
            throw std::runtime_error("visibility GPU timeout");
        }
        const D3D12_RANGE readRange{0, 32 * sizeof(float)};
        float* mapped = nullptr;
        Check(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "visibility readback map");
        if (run < 2)
        {
            memcpy(regularResults[run].data(), mapped, sizeof(regularResults[run]));
        }
        else
        {
            memcpy(perspectiveResult.data(), mapped, sizeof(perspectiveResult));
        }
        const D3D12_RANGE noWrite{0, 0};
        readback->Unmap(0, &noWrite);
    }
    for (unsigned sample = 0; sample < 2; ++sample)
    {
        for (unsigned channel = 0; channel < 4; ++channel)
        {
            Near(regularResults[0][sample * 4 + channel], regularResults[1][sample * 4 + channel],
                 "neighbor triangle/instance changed selected triangle output");
        }
        Near(regularResults[0][sample * 4 + 3], 1, "selected triangle did not reconstruct");
        if (regularResults[0][sample * 4] <= 0.04f)
        {
            throw std::runtime_error("visibility PBR filter did not execute");
        }
    }
    // Prove the perturbation is observable when that other instance is selected.
    if (regularResults[0][24] == regularResults[1][24])
    {
        throw std::runtime_error("neighbor-instance perturbation did not affect its own output");
    }
    if (regularResults[0][8] == regularResults[1][8])
    {
        throw std::runtime_error("neighbor-triangle perturbation did not affect its own output");
    }
    for (unsigned sample : {3u, 4u, 7u})
    {
        Near(regularResults[0][sample * 4], 0.04f, "failed reconstruction altered roughness");
        Near(regularResults[0][sample * 4 + 3], 0, "invalid geometry unexpectedly reconstructed");
    }
    Near(regularResults[0][20], 0.04f, "non-finite reconstructed normal altered roughness");
    Near(regularResults[0][21], 0, "non-finite normal X gradient");
    Near(regularResults[0][22], 0, "non-finite normal Y gradient");
    Near(perspectiveResult[3], 1, "perspective center reconstruction failed");
    Near(perspectiveResult[1], 0, "failed perspective X reconstruction contributed a gradient");
    Near(perspectiveResult[4], 0, "perspective neighbor unexpectedly reconstructed");
    printf("Production visibility reconstruction: triangle/instance isolation, invalid geometry/normal, failed direction PASS\n");
}
