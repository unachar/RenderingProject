#include "pch.h"
#include "proceduralnoisecache.h"

namespace ProcStudio
{
    bool NoiseSampleCache::Initialize(ID3D12Device* device, std::string& error)
    {
        if (Ready())
        {
            return true;
        }
        if (!InitializeFallback(device, error))
        {
            return false;
        }
        CD3DX12_ROOT_PARAMETER parameters[3];
        parameters[0].InitAsUnorderedAccessView(1, 15);
        parameters[1].InitAsUnorderedAccessView(0, 15);
        parameters[2].InitAsConstants(2, 0, 15);
        const CD3DX12_ROOT_SIGNATURE_DESC description(3, parameters);
        ComPtr<ID3DBlob> signature, diagnostics, shader;
        HRESULT result = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics);
        if (SUCCEEDED(result))
        {
            result = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                  IID_PPV_ARGS(&m_Root));
        }
        if (SUCCEEDED(result))
        {
            result = D3DCompileFromFile(L"shader/hlsl/NoiseCacheCS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                       "main", "cs_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &diagnostics);
        }
        if (SUCCEEDED(result))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = m_Root.Get();
            pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
            result = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_Pipeline));
        }
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto allocate = [&](UINT64 bytes, ComPtr<ID3D12Resource>& resource) {
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                    IID_PPV_ARGS(&resource));
        };
        if (SUCCEEDED(result))
        {
            result = allocate(CacheBytes, m_Samples);
        }
        if (SUCCEEDED(result))
        {
            result = allocate(RequestBytes, m_Requests);
        }
        if (FAILED(result))
        {
            error = diagnostics ? std::string(static_cast<const char*>(diagnostics->GetBufferPointer()))
                                : "Procedural noise cache initialization failed";
            m_Samples.Reset();
            m_Requests.Reset();
            m_Root.Reset();
            m_Pipeline.Reset();
            return false;
        }
        m_Samples->SetName(L"Procedural adaptive tiles (65 MiB)");
        m_Requests->SetName(L"Procedural tile requests (2 MiB)");
        return true;
    }

    bool NoiseSampleCache::InitializeFallback(ID3D12Device* device, std::string& error)
    {
        if (m_FallbackSamples && m_FallbackRequests)
        {
            return true;
        }
        const auto upload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto readDescription = CD3DX12_RESOURCE_DESC::Buffer(32);
        HRESULT result = device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &readDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_FallbackSamples));
        if (SUCCEEDED(result))
        {
            void* data = nullptr;
            result = m_FallbackSamples->Map(0, nullptr, &data);
            if (SUCCEEDED(result))
            {
                memset(data, 0, 32);
                m_FallbackSamples->Unmap(0, nullptr);
            }
        }
        if (SUCCEEDED(result))
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(32, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_FallbackRequests));
        }
        if (FAILED(result))
        {
            error = "Noise cache fallback resource allocation failed";
            m_FallbackSamples.Reset();
            m_FallbackRequests.Reset();
            return false;
        }
        return true;
    }

    void NoiseSampleCache::Release()
    {
        m_Samples.Reset();
        m_Requests.Reset();
        m_Root.Reset();
        m_Pipeline.Reset();
        m_FallbackSamples.Reset();
        m_FallbackRequests.Reset();
        m_Cleared = false;
        m_Frame = 0;
    }

    void NoiseSampleCache::Merge(ID3D12GraphicsCommandList* commands, bool clear)
    {
        commands->SetComputeRootSignature(m_Root.Get());
        commands->SetPipelineState(m_Pipeline.Get());
        commands->SetComputeRootUnorderedAccessView(0, m_Samples->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(1, m_Requests->GetGPUVirtualAddress());
        const UINT constants[] = {clear ? 1u : 0u, m_Frame};
        commands->SetComputeRoot32BitConstants(2, 2, constants, 0);
        // One group owns all four pages of one bucket, including tile generation.
        commands->Dispatch(BucketCount, 1, 1);
    }

    void NoiseSampleCache::BeginFrame(ID3D12GraphicsCommandList* commands)
    {
        if (!Ready() || m_Cleared)
        {
            return;
        }
        Merge(commands, true);
        const D3D12_RESOURCE_BARRIER barriers[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_Samples.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::UAV(m_Requests.Get())};
        commands->ResourceBarrier(_countof(barriers), barriers);
        m_Cleared = true;
    }

    void NoiseSampleCache::FinishFrame(ID3D12GraphicsCommandList* commands)
    {
        if (!Ready() || !m_Cleared)
        {
            return;
        }
        const D3D12_RESOURCE_BARRIER before[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_Samples.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::UAV(m_Requests.Get())};
        commands->ResourceBarrier(_countof(before), before);
        ++m_Frame;
        // Frame counter wrap only affects eviction order, never key validation.
        Merge(commands, false);
        const D3D12_RESOURCE_BARRIER after[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_Samples.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::UAV(m_Requests.Get())};
        commands->ResourceBarrier(_countof(after), after);
    }

    void NoiseSampleCache::WriteViews(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE first, UINT increment) const
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = static_cast<UINT>(CacheBytes / 4);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(m_Samples.Get(), &srv, first);
        first.ptr += increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = static_cast<UINT>(RequestBytes / 4);
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(m_Requests.Get(), nullptr, &uav, first);
    }

    void NoiseSampleCache::WriteFallbackViews(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE first, UINT increment) const
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = 8;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(m_FallbackSamples.Get(), &srv, first);
        first.ptr += increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 8;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(m_FallbackRequests.Get(), nullptr, &uav, first);
    }
}
