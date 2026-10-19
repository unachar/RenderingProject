#pragma once
#include "pch.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

// CachedPSO はドライバーが検証するヒント。拒否されたらバイナリなしで再試行する。
class PersistentPsoCache final
{
    struct Key
    {
        uint64_t Value = 1469598103934665603ull;

        void Add(const void* data, size_t size)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            if (bytes)
            {
                for (size_t i = 0; i < size; ++i)
                {
                    Value ^= bytes[i];
                    Value *= 1099511628211ull;
                }
            }
            Value ^= size;
        }

        template <class T> void Add(const T& data)
        {
            Add(&data, sizeof(data));
        }

        void Text(const char* text)
        {
            Add(text, text ? strlen(text) : 0);
        }
    };

    template <class Fn> static HRESULT Create(ID3D12Device* device, Key key, ComPtr<ID3D12PipelineState>& pso, Fn fn)
    {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        key.Add(device->GetAdapterLuid());
        std::ostringstream name;
        name << std::hex << key.Value << ".pso";
        const auto path = std::filesystem::path("bin/cache/pso") / name.str();
        std::vector<char> data;
        try
        {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (input)
            {
                const auto size = input.tellg();
                if (size > 0 && size < std::streampos(64 * 1024 * 1024))
                {
                    data.resize(static_cast<size_t>(size));
                    input.seekg(0);
                    if (!input.read(data.data(), data.size()))
                    {
                        data.clear();
                    }
                }
            }
        }
        catch (...)
        {
            data.clear();
        }
        HRESULT hr = fn(data.empty() ? nullptr : data.data(), data.size());
        if (FAILED(hr) && !data.empty())
        {
            hr = fn(nullptr, 0);
        }
        if (FAILED(hr))
        {
            return hr;
        }
        try
        {
            ComPtr<ID3DBlob> blob;
            if (SUCCEEDED(pso->GetCachedBlob(&blob)) && blob)
            {
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                auto temporary = path;
                temporary += L".tmp";
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
                out.flush();
                const bool ok = bool(out);
                out.close();
                if (ok)
                {
                    MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
                }
            }
        }
        catch (...)
        {
        }
        return hr;
    }

  public:
    static HRESULT Graphics(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& source,
                            ComPtr<ID3D12PipelineState>& pso)
    {
        if (!device)
        {
            return E_POINTER;
        }
        Key key;
        key.Text("graphics-v2");
        for (auto shader : {source.VS, source.PS, source.DS, source.HS, source.GS})
        {
            key.Add(shader.pShaderBytecode, shader.BytecodeLength);
        }
        key.Add(source.BlendState);
        key.Add(source.SampleMask);
        key.Add(source.RasterizerState);
        key.Add(source.DepthStencilState);
        key.Add(source.IBStripCutValue);
        key.Add(source.PrimitiveTopologyType);
        key.Add(source.NumRenderTargets);
        key.Add(source.RTVFormats);
        key.Add(source.DSVFormat);
        key.Add(source.SampleDesc);
        key.Add(source.NodeMask);
        key.Add(source.Flags);
        key.Add(source.InputLayout.NumElements);
        for (UINT i = 0; i < source.InputLayout.NumElements; ++i)
        {
            const auto& e = source.InputLayout.pInputElementDescs[i];
            key.Text(e.SemanticName);
            key.Add(e.SemanticIndex);
            key.Add(e.Format);
            key.Add(e.InputSlot);
            key.Add(e.AlignedByteOffset);
            key.Add(e.InputSlotClass);
            key.Add(e.InstanceDataStepRate);
        }
        key.Add(source.StreamOutput.NumEntries);
        key.Add(source.StreamOutput.RasterizedStream);
        for (UINT i = 0; i < source.StreamOutput.NumEntries; ++i)
        {
            const auto& e = source.StreamOutput.pSODeclaration[i];
            key.Text(e.SemanticName);
            key.Add(e.Stream);
            key.Add(e.SemanticIndex);
            key.Add(e.StartComponent);
            key.Add(e.ComponentCount);
            key.Add(e.OutputSlot);
        }
        key.Add(source.StreamOutput.pBufferStrides, source.StreamOutput.NumStrides * sizeof(UINT));
        auto desc = source;
        return Create(device, key, pso, [&](const void* bytes, size_t size) {
            desc.CachedPSO = {bytes, size};
            return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.ReleaseAndGetAddressOf()));
        });
    }

    static HRESULT Compute(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC& source,
                           ComPtr<ID3D12PipelineState>& pso)
    {
        if (!device)
        {
            return E_POINTER;
        }
        Key key;
        key.Text("compute-v2");
        key.Add(source.CS.pShaderBytecode, source.CS.BytecodeLength);
        key.Add(source.NodeMask);
        key.Add(source.Flags);
        auto desc = source;
        return Create(device, key, pso, [&](const void* bytes, size_t size) {
            desc.CachedPSO = {bytes, size};
            return device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pso.ReleaseAndGetAddressOf()));
        });
    }
};
