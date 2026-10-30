#include "pch.h"
#include "screenspacereflections.h"
#include "memoryallocator.h"
#include "renderersettings.h"
#include "texturemanager.h"

#include <string>

namespace ScreenSpaceReflections
{
    namespace
    {
        ComPtr<ID3D12RootSignature> g_RootSignature;
        ComPtr<ID3D12PipelineState> g_Pso;
        ComPtr<ID3D12DescriptorHeap> g_Heap; // 0: result SRV, 1: result UAV
        ComPtr<ID3D12Resource> g_Result;
        uint32_t g_ResultWidth = 0;
        uint32_t g_ResultHeight = 0;
        UINT g_DescriptorIncrement = 0;
        Source g_FrameSource = Source::Environment;
        bool g_ResultProduced = false;
        bool g_ResultInSrvState = false;
        int g_ResultEngineSrvIndex = -1;

        bool EnsurePipeline(ID3D12Device* device)
        {
            if (g_Pso)
            {
                return true;
            }
            CD3DX12_DESCRIPTOR_RANGE ranges[6];
            ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
            ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
            ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
            ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);
            ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            CD3DX12_ROOT_PARAMETER parameters[7];
            parameters[0].InitAsConstants(52, 0);
            for (int i = 0; i < 6; ++i)
            {
                parameters[1 + i].InitAsDescriptorTable(1, &ranges[i], D3D12_SHADER_VISIBILITY_ALL);
            }
            CD3DX12_STATIC_SAMPLER_DESC samplers[2];
            samplers[0].Init(0);
            samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[1].Init(1);
            samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            CD3DX12_ROOT_SIGNATURE_DESC rootDesc(_countof(parameters), parameters, _countof(samplers), samplers,
                                                 D3D12_ROOT_SIGNATURE_FLAG_NONE);
            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> errors;
            if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
                FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                   IID_PPV_ARGS(&g_RootSignature))))
            {
                return false;
            }
            ComPtr<ID3DBlob> shader;
            if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/ScreenSpaceReflectionsTraceCS.cso", &shader)))
            {
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = g_RootSignature.Get();
            pso.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
            if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_Pso))))
            {
                return false;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 2;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_Heap))))
            {
                return false;
            }
            g_DescriptorIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            return true;
        }
    } // namespace

    Source SelectSource(bool dxrReflectionActive, bool ssrRequested, bool ssrResultReady)
    {
        if (dxrReflectionActive)
        {
            return Source::Raytraced;
        }
        if (ssrRequested && ssrResultReady)
        {
            return Source::ScreenSpace;
        }
        return Source::Environment;
    }

    void UpdateFrameSelection(bool dxrReflectionActive)
    {
        g_FrameSource = SelectSource(dxrReflectionActive, RendererSettings::GetUseScreenSpaceReflections(),
                                     g_ResultProduced && g_Result != nullptr);
    }

    Source GetFrameSource()
    {
        return g_FrameSource;
    }

    bool IsScreenSpaceSelected()
    {
        return g_FrameSource == Source::ScreenSpace;
    }

    int GetResultEngineSrvIndex()
    {
        return g_ResultEngineSrvIndex;
    }

    bool EnsureResultTarget(ID3D12Device* device, uint32_t width, uint32_t height)
    {
        if (device == nullptr || width == 0 || height == 0)
        {
            return false;
        }
        if (g_Result && g_ResultWidth == width && g_ResultHeight == height)
        {
            return true;
        }
        if (!EnsurePipeline(device))
        {
            return false;
        }
        const auto description = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1, 1,
                                                               0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> result;
        if (ID3D12Resource* allocated =
                MemoryAllocator::CreateResourceAutoLifetime(description, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr))
        {
            result.Attach(allocated);
        }
        else
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&result))))
            {
                return false;
            }
        }
        const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CD3DX12_CPU_DESCRIPTOR_HANDLE heapCpu(g_Heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(result.Get(), &srvDesc, heapCpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(result.Get(), nullptr, &uavDesc,
                                          CD3DX12_CPU_DESCRIPTOR_HANDLE(heapCpu, 1, increment));
        g_Result = result;
        g_ResultWidth = width;
        g_ResultHeight = height;
        g_ResultProduced = false;
        g_ResultInSrvState = false;
        // Register in the engine heap so the lighting pass can bind
        // the result exactly like the raytraced reflection texture.
        const std::string registration = "ssr://result/" + std::to_string(width) + "x" + std::to_string(height);
        g_ResultEngineSrvIndex = TextureManager::RegisterExternalTexture(registration.c_str(), result);
        return true;
    }

    void ReleaseResultTarget()
    {
        g_Result.Reset();
        g_ResultWidth = 0;
        g_ResultHeight = 0;
        g_ResultProduced = false;
        g_ResultInSrvState = false;
        g_ResultEngineSrvIndex = -1;
    }

    bool IsResultReady()
    {
        return g_Result != nullptr;
    }

    ID3D12Resource* GetResultResource()
    {
        return g_Result.Get();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetResultSrv()
    {
        return g_Heap ? g_Heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }

    bool Execute(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* engineHeap, const TraceInputs& inputs)
    {
        if (commandList == nullptr || engineHeap == nullptr || !g_Result || !g_Pso)
        {
            return false;
        }
        commandList->SetComputeRootSignature(g_RootSignature.Get());
        commandList->SetPipelineState(g_Pso.Get());
        ID3D12DescriptorHeap* heaps[] = {engineHeap, g_Heap.Get()};
        commandList->SetDescriptorHeaps(2, heaps);
        commandList->SetComputeRoot32BitConstants(0, 52, &inputs.Constants, 0);
        commandList->SetComputeRootDescriptorTable(1, inputs.DepthSrv);
        commandList->SetComputeRootDescriptorTable(2, inputs.HiZSrv);
        commandList->SetComputeRootDescriptorTable(3, inputs.NormalSrv);
        commandList->SetComputeRootDescriptorTable(4, inputs.PreviousColorSrv);
        commandList->SetComputeRootDescriptorTable(5, inputs.MaterialSrv);
        commandList->SetComputeRootDescriptorTable(
            6, CD3DX12_GPU_DESCRIPTOR_HANDLE(g_Heap->GetGPUDescriptorHandleForHeapStart(), 1, g_DescriptorIncrement));
        if (g_ResultInSrvState)
        {
            D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(
                g_Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1, &toUav);
            g_ResultInSrvState = false;
        }
        commandList->Dispatch((g_ResultWidth + 7) / 8, (g_ResultHeight + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            g_Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toSrv);
        g_ResultInSrvState = true;
        g_ResultProduced = true;
        return true;
    }
} // namespace ScreenSpaceReflections
