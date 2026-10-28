#pragma once
#include "procedural/proceduralnoisecache.h"
#include "procedural/proceduraltextureparams.h"
#include <cstring>
#include <iomanip>
#include <vector>

void TestNoiseSampleCache(Harness& harness)
{
    using namespace ProcStudio;
    NoiseSampleCache cache;
    Require(cache.Initialize(harness.Device.Get(), harness.Error), harness.Error);
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    CD3DX12_ROOT_PARAMETER parameters[5];
    parameters[0].InitAsShaderResourceView(0);
    parameters[1].InitAsUnorderedAccessView(0);
    parameters[2].InitAsConstants(5, 0);
    parameters[3].InitAsShaderResourceView(0, 15);
    parameters[4].InitAsUnorderedAccessView(0, 15);
    const CD3DX12_ROOT_SIGNATURE_DESC description(5, parameters);
    ComPtr<ID3DBlob> signature, diagnostics, shader;
    Require(SUCCEEDED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics)),
            "noise cache test root serialization");
    Require(SUCCEEDED(harness.Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root))), "noise cache test root");
    const HRESULT compiled = D3DCompileFromFile(L"tests/noise_cache_shader.hlsl", nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &diagnostics);
    Require(SUCCEEDED(compiled), diagnostics ? static_cast<const char*>(diagnostics->GetBufferPointer()) : "cache shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    Require(SUCCEEDED(harness.Device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline))), "cache shader PSO");
    // 57,600 unique lattice nodes plus 7,936 repeated requests; 5,632 definition tiles.
    constexpr UINT count = 65536;
    constexpr UINT basisCount = static_cast<UINT>(Basis::Count);
    ComPtr<ID3D12Resource> definitions, output, readback;
    const auto allocate = [&](UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                              ComPtr<ID3D12Resource>& resource) {
        const auto heap = CD3DX12_HEAP_PROPERTIES(type);
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(size, type == D3D12_HEAP_TYPE_DEFAULT
            ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        Require(SUCCEEDED(harness.Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, state,
            nullptr, IID_PPV_ARGS(&resource))), "cache test buffer");
    };
    allocate(sizeof(TextureGpuParams) * basisCount, D3D12_HEAP_TYPE_UPLOAD,
             D3D12_RESOURCE_STATE_GENERIC_READ, definitions);
    allocate(sizeof(Float4) * count, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output);
    allocate(sizeof(Float4) * count, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readback);
    std::vector<TextureGpuParams> gpuDefinitions(basisCount);
    const auto update = [&](UINT edit) {
        for (UINT basis = 0; basis < basisCount; ++basis)
        {
            TextureParams params;
            params.Noise.BasisType = static_cast<Basis>(basis);
            params.Noise.Octaves = 4;
            // Exercise the legal asset maximum; higher values amplify compiler sin/cos differences.
            params.Noise.Frequency = 64.0f;
            params.Noise.Fractal = FractalType::FBm;
            params.Tileable = (basis & 1u) != 0;
            params.Noise.Seed += (edit & 1u) != 0 ? 977u : 0u;
            if ((edit & 2u) != 0)
            {
                params.ColorA = {0.9f, 0.3f, 0.7f};
            }
            gpuDefinitions[basis] = MakeTextureGpuParams(params);
        }
        void* mapped = nullptr;
        Require(SUCCEEDED(definitions->Map(0, nullptr, &mapped)), "cache definitions map");
        memcpy(mapped, gpuDefinitions.data(), gpuDefinitions.size() * sizeof(TextureGpuParams));
        definitions->Unmap(0, nullptr);
    };
    bool first = true;
    double milliseconds = 0;
    const auto run = [&](UINT referenceMode, UINT offset, bool nullCache = false, UINT scenario = 0) {
        harness.Begin();
        cache.BeginFrame(harness.Commands.Get());
        if (!first)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            harness.Commands->ResourceBarrier(1, &barrier);
        }
        first = false;
        harness.Commands->SetComputeRootSignature(root.Get());
        harness.Commands->SetPipelineState(pipeline.Get());
        harness.Commands->SetComputeRootShaderResourceView(0, definitions->GetGPUVirtualAddress());
        harness.Commands->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        const UINT values[] = {count, basisCount, referenceMode, offset, scenario};
        harness.Commands->SetComputeRoot32BitConstants(2, 5, values, 0);
        harness.Commands->SetComputeRootShaderResourceView(3,
            (nullCache ? cache.FallbackSamples() : cache.Samples())->GetGPUVirtualAddress());
        harness.Commands->SetComputeRootUnorderedAccessView(4,
            (nullCache ? cache.FallbackRequests() : cache.Requests())->GetGPUVirtualAddress());
        harness.Commands->Dispatch((count + 63) / 64, 1, 1);
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        harness.Commands->ResourceBarrier(1, &barrier);
        harness.Commands->CopyResource(readback.Get(), output.Get());
        cache.FinishFrame(harness.Commands.Get());
        const auto start = std::chrono::steady_clock::now();
        harness.Submit();
        milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        void* mapped = nullptr;
        Require(SUCCEEDED(readback->Map(0, nullptr, &mapped)), "cache output map");
        const auto* data = static_cast<const Float4*>(mapped);
        std::vector<Float4> result(data, data + count);
        readback->Unmap(0, nullptr);
        return result;
    };
    UINT parityCheck = 0;
    const auto parity = [&](const std::vector<Float4>& expected, const std::vector<Float4>& actual,
                            float tolerance = 0.0006f, const char* stage = "resident vertices") {
        ++parityCheck;
        for (UINT index = 0; index < count; ++index)
        {
            const bool matches = std::abs(expected[index].X - actual[index].X) <= tolerance &&
                std::abs(expected[index].Y - actual[index].Y) <= tolerance &&
                std::abs(expected[index].Z - actual[index].Z) <= tolerance;
            if (!matches)
            {
                std::cerr << std::setprecision(9) << "Noise tile parity check=" << parityCheck << " stage=" << stage
                          << " index=" << index << " expected=(" << expected[index].X << ',' << expected[index].Y
                          << ',' << expected[index].Z << ") actual=(" << actual[index].X << ',' << actual[index].Y
                          << ',' << actual[index].Z << ") tolerance=" << tolerance << " hit=" << actual[index].W << '\n';
            }
            Require(matches, "tile cache RGB parity at every UV, including negative page borders");
        }
    };
    const auto hitCount = [&](const std::vector<Float4>& values) {
        UINT hits = 0;
        for (const auto& value : values)
            hits += value.W == 1.0f ? 1u : 0u;
        return hits;
    };
    const auto warm = [&](UINT offset, UINT scenario = 0) {
        std::vector<Float4> values;
        UINT frameHits[16]{};
        const auto printWarmup = [&](UINT frames) {
            std::cout << "Noise tile warmup offset=" << offset << " scenario=" << scenario << " hits/frame=";
            for (UINT index = 0; index < frames; ++index)
                std::cout << (index == 0 ? "" : ",") << frameHits[index];
            std::cout << "/" << count << '\n';
        };
        for (UINT frame = 0; frame < 16; ++frame)
        {
            values = run(0, offset, false, scenario);
            frameHits[frame] = hitCount(values);
            if (frame >= 7 && frameHits[frame] > count * 99 / 100)
            {
                printWarmup(frame + 1);
                return values;
            }
        }
        printWarmup(16);
        Require(hitCount(values) > count * 99 / 100, "warm tile cache must reuse >99% of samples after bounded warmup");
        return values;
    };
    update(0);
    const auto baseline = run(1, 0);
    const double baselineMs = milliseconds;
    const auto fallback = run(0, 0, true);
    parity(baseline, fallback, 0.00002f, "fallback");
    Require(hitCount(fallback) == 0, "real small fallback resources must always miss");
    const auto cold = run(0, 0);
    const double coldMs = milliseconds;
    parity(baseline, cold, 0.00002f, "cold");
    Require(hitCount(cold) == 0, "first tile frame must be analytical");
    const auto warmed = warm(0);
    const double warmMs = milliseconds;
    parity(baseline, warmed);
    const auto repeated = run(0, 0);
    for (UINT index = 0; index < count; ++index)
    {
        if (warmed[index].W == 1 && repeated[index].W == 1)
        {
            Require(warmed[index].X == repeated[index].X && warmed[index].Y == repeated[index].Y &&
                    warmed[index].Z == repeated[index].Z, "resident FP16 tile colors must repeat exactly");
        }
    }
    std::cout << "Noise tiles: " << hitCount(warmed) << '/' << count << " warm hits (57600 unique nodes, 5632 definition tiles); uncached=" << baselineMs
              << "ms cold=" << coldMs << "ms warm=" << warmMs
              << "ms (submit/copy/merge/fence elapsed time, not scene FPS)\n";

    // Quarter-cell samples reuse the same pages; the independent reference rounds
    // analytical corners through FP16 before bilinear interpolation.
    run(1, 0, false, 1);
    const double movingUncachedMs = milliseconds;
    const auto interpolated = run(0, 0, false, 1);
    const double movingCachedMs = milliseconds;
    const auto interpolationReference = run(2, 0, false, 1);
    for (UINT index = 0; index < count; ++index)
    {
        if (interpolated[index].W == 1)
        {
            Require(std::abs(interpolated[index].X - interpolationReference[index].X) <= 0.000002f &&
                    std::abs(interpolated[index].Y - interpolationReference[index].Y) <= 0.000002f &&
                    std::abs(interpolated[index].Z - interpolationReference[index].Z) <= 0.000002f,
                    "off-node hit must interpolate independently evaluated FP16 corners");
        }
    }
    Require(hitCount(interpolated) > count * 99 / 100, "subpixel motion must reuse resident tiles");
    std::cout << "Noise tiles subpixel motion: uncached=" << movingUncachedMs << "ms cached=" << movingCachedMs
              << "ms (submit/copy/merge/fence elapsed time, not scene FPS)\n";

    const auto fineReference = run(1, 0, false, 2);
    const auto fineCold = run(0, 0, false, 2);
    parity(fineReference, fineCold, 0.00002f, "fine cold");
    Require(hitCount(fineCold) == 0, "finer footprint must request a new grid level");
    parity(fineReference, warm(0, 2));
    const auto invalidReference = run(1, 0, false, 3);
    const auto invalid = run(0, 0, false, 3);
    parity(invalidReference, invalid, 0.00002f, "invalid footprint");
    Require(hitCount(invalid) == 0, "zero footprint must use analytical fallback");

    // Distinct regions exceed physical residency, exercising bucket replacement.
    // Every miss remains analytical; resident vertices retain FP16 precision.
    for (UINT region = 1; region <= 12; ++region)
    {
        const UINT offset = region * 4096;
        const auto reference = run(1, offset);
        parity(reference, run(0, offset));
    }
    const auto revisited = run(0, 0);
    parity(baseline, revisited);
    Require(hitCount(revisited) < count * 99 / 100, "new regions must evict previously resident tiles");
    parity(baseline, warm(0));
    update(1);
    const auto editedReference = run(1, 0);
    const auto edited = run(0, 0);
    parity(editedReference, edited, 0.00002f, "seed edit cold");
    Require(hitCount(edited) == 0, "edited seed must miss old definition pages");
    parity(editedReference, warm(0));
    parity(editedReference, run(0, 0, true), 0.00002f, "edited fallback");
    update(3);
    const auto recoloredReference = run(1, 0);
    const auto recolored = run(0, 0);
    parity(recoloredReference, recolored, 0.00002f, "color edit cold");
    Require(hitCount(recolored) == 0, "edited colors alone must miss old definition pages");
    parity(recoloredReference, warm(0));
    harness.CheckDiagnostics();
    std::cout << "Noise tile GPU parity: all 22 bases, negative seams, subpixel interpolation, finer LOD, edits, fallback and eviction PASS\n";
}







