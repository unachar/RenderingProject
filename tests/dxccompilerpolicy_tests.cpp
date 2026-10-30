#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std; // The existing compatibility header uses the engine's std imports.
#include "../source/dxccompiler.h"
#include "../source/d3dx12.h"

namespace
{
    const string CacheProbeValue = to_string(GetTickCount64());

    set<filesystem::path> CacheFiles()
    {
        set<filesystem::path> files;
        const filesystem::path directory = "EditorCache/Shaders";
        if (filesystem::exists(directory))
        {
            for (const auto& file : filesystem::directory_iterator(directory))
            {
                if (file.path().extension() == ".bin")
                {
                    files.insert(file.path());
                }
            }
        }
        return files;
    }

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw runtime_error(message);
        }
    }

    using Parts = map<string, vector<unsigned char>>;

    Parts ReadParts(ID3DBlob* blob)
    {
        const auto* bytes = static_cast<const unsigned char*>(blob->GetBufferPointer());
        const size_t size = blob->GetBufferSize();
        auto word = [&](size_t offset) {
            Require(offset <= size && size - offset >= 4, "DXIL container bounds");
            uint32_t value;
            memcpy(&value, bytes + offset, sizeof(value));
            return value;
        };
        Require(size >= 32 && memcmp(bytes, "DXBC", 4) == 0, "DXIL container header");
        Parts parts;
        const uint32_t count = word(28);
        for (uint32_t index = 0; index < count; ++index)
        {
            const size_t offset = word(32 + size_t(index) * 4);
            const size_t length = word(offset + 4);
            Require(offset + 8 <= size && length <= size - offset - 8, "DXIL part bounds");
            parts.emplace(string(reinterpret_cast<const char*>(bytes + offset), 4),
                          vector<unsigned char>(bytes + offset + 8, bytes + offset + 8 + length));
        }
        return parts;
    }

    ComPtr<ID3DBlob> Compile(const char* entry, const char* profile, UINT flags, const char* keep)
    {
        SetEnvironmentVariableA("DX12_KEEP_SHADER_REFLECTION", keep);
        ComPtr<ID3DBlob> object;
        ComPtr<ID3DBlob> errors;
        // A fresh cache key prevents pre-existing files from hiding a failed Store/Load path.
        const D3D_SHADER_MACRO defines[] = {{"DXC_POLICY_CACHE_PROBE", CacheProbeValue.c_str()}, {}};
        HRESULT result = DxcCompileFromFileCompat(L"tests/shaders/dxil_reflection_fixture.hlsl", defines,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile, flags, 0, &object, &errors);
        if (FAILED(result) && errors)
        {
            fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        }
        Require(SUCCEEDED(result) && object, "production DXC compile failed");
        return object;
    }
}

int main()
{
    try
    {
        const auto previousCacheFiles = CacheFiles();
        vector<vector<char>> expectedCacheObjects;
        auto rememberCacheObject = [&](ID3DBlob* object) {
            const auto* bytes = static_cast<const char*>(object->GetBufferPointer());
            expectedCacheObjects.emplace_back(bytes, bytes + object->GetBufferSize());
        };
        // Exercise cache reads even if the caller normally disables the persistent cache.
        SetEnvironmentVariableA("DX12_DISABLE_SHADER_CACHE", "0");
        Require(DxcCompilerPolicy::ShouldStripReflection(0, false), "release policy");
        Require(!DxcCompilerPolicy::ShouldStripReflection(0, true), "override policy");
        Require(!DxcCompilerPolicy::ShouldStripReflection(D3DCOMPILE_DEBUG, false), "debug policy");
        Require(!DxcCompilerPolicy::ShouldStripReflection(D3DCOMPILE_DEBUG, true), "debug override policy");
        SetEnvironmentVariableA("DX12_KEEP_SHADER_REFLECTION", "1");
        Require(DxcCompilerPolicy::KeepReflectionRequested(), "environment override");
        SetEnvironmentVariableA("DX12_KEEP_SHADER_REFLECTION", "10");
        Require(!DxcCompilerPolicy::KeepReflectionRequested(), "long environment value is not exact 1");
        SetEnvironmentVariableA("DX12_KEEP_SHADER_REFLECTION", nullptr);
        Require(!DxcCompilerPolicy::KeepReflectionRequested(), "unset environment override");
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
        Require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
        Require(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))),
                "D3D12 device");
        ComPtr<ID3DBlob> shaders[3];
        const char* entries[] = {"Vertex", "Pixel", "Compute"};
        const char* profiles[] = {"vs_6_0", "ps_6_0", "cs_6_0"};
        size_t baselineBytes = 0;
        size_t candidateBytes = 0;
        for (size_t stage = 0; stage < 3; ++stage)
        {
            auto baseline = Compile(entries[stage], profiles[stage], D3DCOMPILE_OPTIMIZATION_LEVEL3, "1");
            shaders[stage] = Compile(entries[stage], profiles[stage], D3DCOMPILE_OPTIMIZATION_LEVEL3, nullptr);
            const Parts before = ReadParts(baseline.Get());
            const Parts after = ReadParts(shaders[stage].Get());
            Require(before.count("STAT") == 1, "baseline reflection missing");
            Require(after.count("STAT") == 0, "default non-debug shader still contains reflection");
            Require(after.count("DXIL") == 1 && after.count("RTS0") == 1, "executable/root signature missing");
            for (const auto& part : before)
            {
                if (part.first != "STAT")
                {
                    Require(after.count(part.first) == 1 && after.at(part.first) == part.second,
                            "execution-relevant DXIL container part changed");
                }
            }
            auto cached = Compile(entries[stage], profiles[stage], D3DCOMPILE_OPTIMIZATION_LEVEL3, nullptr);
            Require(ReadParts(cached.Get()) == after, "cache returned wrong compiler-policy variant");
            auto restored = Compile(entries[stage], profiles[stage], D3DCOMPILE_OPTIMIZATION_LEVEL3, "1");
            Require(ReadParts(restored.Get()) == before, "reflection override cache variant lost");
            auto debug = Compile(entries[stage], profiles[stage], D3DCOMPILE_DEBUG, nullptr);
            const Parts debugParts = ReadParts(debug.Get());
            Require(debugParts.count("STAT") == 1 && debugParts.count("ILDB") == 1,
                    "debug reflection or embedded debug data lost");
            rememberCacheObject(baseline.Get());
            rememberCacheObject(shaders[stage].Get());
            rememberCacheObject(debug.Get());
            baselineBytes += baseline->GetBufferSize();
            candidateBytes += shaders[stage]->GetBufferSize();
            printf("%s: %zu -> %zu bytes; executable parts/cache/debug PASS\n", profiles[stage],
                   baseline->GetBufferSize(), shaders[stage]->GetBufferSize());
        }
        ComPtr<ID3D12RootSignature> root;
        // CreateRootSignature consumes the serialized container, not the raw RTS0 payload.
        Require(SUCCEEDED(device->CreateRootSignature(0, shaders[2]->GetBufferPointer(),
                shaders[2]->GetBufferSize(), IID_PPV_ARGS(&root))),
                "root signature creation");
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
        compute.pRootSignature = root.Get();
        compute.CS = {shaders[2]->GetBufferPointer(), shaders[2]->GetBufferSize()};
        ComPtr<ID3D12PipelineState> computePso;
        Require(SUCCEEDED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&computePso))), "compute PSO");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
        graphics.pRootSignature = root.Get();
        graphics.VS = {shaders[0]->GetBufferPointer(), shaders[0]->GetBufferSize()};
        graphics.PS = {shaders[1]->GetBufferPointer(), shaders[1]->GetBufferSize()};
        graphics.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        graphics.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        graphics.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        graphics.DepthStencilState.DepthEnable = FALSE;
        graphics.DepthStencilState.StencilEnable = FALSE;
        graphics.SampleMask = UINT_MAX;
        graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        graphics.NumRenderTargets = 1;
        graphics.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        graphics.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> graphicsPso;
        Require(SUCCEEDED(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&graphicsPso))), "graphics PSO");
        Require(candidateBytes < baselineBytes, "reflection stripping did not reduce fixture bytes");
        vector<bool> persisted(expectedCacheObjects.size(), false);
        for (const auto& file : CacheFiles())
        {
            if (previousCacheFiles.count(file))
            {
                continue;
            }
            vector<char> object;
            string diagnostics;
            if (!DxcShaderCache::Load(file, object, diagnostics))
            {
                continue;
            }
            for (size_t index = 0; index < expectedCacheObjects.size(); ++index)
            {
                if (object == expectedCacheObjects[index])
                {
                    persisted[index] = true;
                }
            }
        }
        for (bool found : persisted)
        {
            Require(found, "a fresh shader-policy variant was not persisted/read back from disk cache");
        }
        printf("Fresh persistent shader variants: %zu stored and checksum-validated\n", persisted.size());
        printf("DXIL policy/PSO tests PASS: %zu -> %zu total bytes\n", baselineBytes, candidateBytes);
        return 0;
    }
    catch (const exception& error)
    {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
