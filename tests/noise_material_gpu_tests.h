#pragma once
#include "procedural/proceduraltextureparams.h"

// Runs the resource-free production material evaluator at sub-texel coordinates on a real D3D12 queue.
void TestNoiseMaterialEvaluation(Harness& harness, int firstBasis = 0,
                                 int endBasis = static_cast<int>(ProcStudio::Basis::Count))
{
    using namespace ProcStudio;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    CD3DX12_ROOT_PARAMETER parameters[2];
    parameters[0].InitAsShaderResourceView(0);
    parameters[1].InitAsUnorderedAccessView(0);
    const CD3DX12_ROOT_SIGNATURE_DESC description(2, parameters);
    ComPtr<ID3DBlob> serialized, diagnostics, shader;
    Require(
        SUCCEEDED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostics)),
        "material evaluation root serialization");
    Require(SUCCEEDED(harness.Device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                          serialized->GetBufferSize(), IID_PPV_ARGS(&root))),
            "material evaluation root");
    const auto compiled =
        D3DCompileFromFile(L"tests/noise_material_shader.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
                           "cs_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &diagnostics);
    Require(SUCCEEDED(compiled),
            diagnostics ? static_cast<const char*>(diagnostics->GetBufferPointer()) : "material evaluator compilation");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
    Require(SUCCEEDED(harness.Device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline))),
            "material evaluation PSO");
    ComPtr<ID3D12Resource> definitions, output, readback;
    const auto allocate = [&](UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                              ComPtr<ID3D12Resource>& resource) {
        const auto heap = CD3DX12_HEAP_PROPERTIES(type);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        if (type == D3D12_HEAP_TYPE_DEFAULT)
        {
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        Require(SUCCEEDED(harness.Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                                  IID_PPV_ARGS(&resource))),
                "material test buffer");
    };
    allocate(sizeof(TextureGpuParams), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, definitions);
    allocate(16 * sizeof(Float4), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output);
    allocate(16 * sizeof(Float4), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readback);
    constexpr float e = 1.0f / 2048;
    for (int basis = firstBasis; basis < endBasis; ++basis)
    {
        TextureParams params;
        params.Noise.BasisType = static_cast<Basis>(basis);
        params.Noise.Fractal = FractalType::None;
        params.Tileable = false;
        auto packed = MakeTextureGpuParams(params);
        packed.Width = 1; // enabled; no baked image dimensions
        void* data = nullptr;
        Require(SUCCEEDED(definitions->Map(0, nullptr, &data)), "material definitions map");
        memcpy(data, &packed, sizeof(packed));
        definitions->Unmap(0, nullptr);
        harness.Begin();
        if (basis != firstBasis)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            harness.Commands->ResourceBarrier(1, &barrier);
        }
        harness.Commands->SetComputeRootSignature(root.Get());
        harness.Commands->SetPipelineState(pipeline.Get());
        harness.Commands->SetComputeRootShaderResourceView(0, definitions->GetGPUVirtualAddress());
        harness.Commands->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        harness.Commands->Dispatch(1, 1, 1);
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
        harness.Commands->ResourceBarrier(1, &barrier);
        harness.Commands->CopyResource(readback.Get(), output.Get());
        harness.Submit();
        Require(SUCCEEDED(readback->Map(0, nullptr, &data)), "material results map");
        const auto* actual = static_cast<const Float4*>(data);
        for (int i = 0; i < 8; ++i)
        {
            const float u = .12345f + i * .000001f, v = .6789f;
            const auto scalar = [&](float x, float y) {
                return TextureScalar(params, x, y, 0, false);
            };
            const float expectedHeight = scalar(u, v);
            const Float3 expectedColor = TextureColor(params, u, v, 0, false);
            const auto& a = actual[i * 2];
            Require(std::abs(a.W - expectedHeight) < .004f, "unbaked scalar CPU/GPU parity");
            Require(std::abs(a.X - expectedColor.X) < .008f && std::abs(a.Y - expectedColor.Y) < .008f &&
                        std::abs(a.Z - expectedColor.Z) < .008f,
                    "unbaked color CPU/GPU parity");
            const float dx = (scalar(u + e, v) - scalar(u - e, v)) / (2 * e) * params.NormalStrength / 256;
            const float dy = (scalar(u, v + e) - scalar(u, v - e)) / (2 * e) * params.NormalStrength / 256;
            const float inverseLength = 1 / std::sqrt(dx * dx + dy * dy + 1);
            const auto& n = actual[i * 2 + 1];
            Require(std::isfinite(n.X) && std::isfinite(n.Y) && std::isfinite(n.Z) &&
                        std::abs(n.X + dx * inverseLength) < .025f && std::abs(n.Y - dy * inverseLength) < .025f &&
                        std::abs(n.Z - inverseLength) < .025f,
                    "resolution-independent normal derivative parity");
        }
        readback->Unmap(0, nullptr);
    }
    std::cout << "Noise material evaluation: " << endBasis - firstBasis
              << " bases, sub-texel scalar/color/normal GPU parity PASS\n";
}
