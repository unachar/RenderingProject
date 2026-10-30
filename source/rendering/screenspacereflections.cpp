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
        ComPtr<ID3D12RootSignature> g_ResolveRootSignature;
        ComPtr<ID3D12PipelineState> g_ResolvePso;
        // Heap slots: 0 result SRV, 1 result UAV, 2 trace SRV,
        // 3 trace UAV, 4 hit-points SRV, 5 hit-points UAV,
        // 6/7 accumulation SRVs, 8/9 accumulation UAVs.
        ComPtr<ID3D12DescriptorHeap> g_Heap;
        ComPtr<ID3D12Resource> g_Result;
        ComPtr<ID3D12Resource> g_TraceResult; // half-resolution trace target (T1)
        ComPtr<ID3D12Resource> g_HitPoints;   // half-res hit anchors (T2)
        ComPtr<ID3D12Resource> g_Accumulation[2]; // full-res temporal ping-pong (T2)
        int g_AccumulationCurrent = 0;
        bool g_HistoryValid = false;
        uint32_t g_FrameCounter = 0;
        bool g_HitInSrvState = false;
        uint32_t g_TraceWidth = 0;
        uint32_t g_TraceHeight = 0;
        bool g_TraceInSrvState = false;

        ComPtr<ID3D12Resource> CreateTarget(ID3D12Device* device, const D3D12_RESOURCE_DESC& description,
                                            D3D12_RESOURCE_STATES initialState)
        {
            ComPtr<ID3D12Resource> target;
            if (ID3D12Resource* allocated =
                    MemoryAllocator::CreateResourceAutoLifetime(description, initialState, nullptr))
            {
                target.Attach(allocated);
                return target;
            }
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, initialState, nullptr,
                                            IID_PPV_ARGS(&target));
            return target;
        }
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
            CD3DX12_DESCRIPTOR_RANGE ranges[7];
            ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
            ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
            ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
            ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);
            ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            ranges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
            CD3DX12_ROOT_PARAMETER parameters[8];
            parameters[0].InitAsConstants(52, 0);
            for (int i = 0; i < 7; ++i)
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
            heapDesc.NumDescriptors = 10;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_Heap))))
            {
                return false;
            }
            g_DescriptorIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            // Resolve pipeline (bilateral upsample + temporal blend).
            CD3DX12_DESCRIPTOR_RANGE resolveRanges[8];
            for (int i = 0; i < 6; ++i)
            {
                resolveRanges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i);
            }
            resolveRanges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            resolveRanges[7].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
            CD3DX12_ROOT_PARAMETER resolveParameters[9];
            resolveParameters[0].InitAsConstants(26, 0);
            for (int i = 0; i < 8; ++i)
            {
                resolveParameters[1 + i].InitAsDescriptorTable(1, &resolveRanges[i], D3D12_SHADER_VISIBILITY_ALL);
            }
            CD3DX12_STATIC_SAMPLER_DESC resolveSampler{};
            resolveSampler.Init(0);
            resolveSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            resolveSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            resolveSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            resolveSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            CD3DX12_ROOT_SIGNATURE_DESC resolveRootDesc(_countof(resolveParameters), resolveParameters, 1,
                                                        &resolveSampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
            ComPtr<ID3DBlob> resolveSerialized;
            if (FAILED(D3D12SerializeRootSignature(&resolveRootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &resolveSerialized,
                                                   &errors)) ||
                FAILED(device->CreateRootSignature(0, resolveSerialized->GetBufferPointer(),
                                                   resolveSerialized->GetBufferSize(),
                                                   IID_PPV_ARGS(&g_ResolveRootSignature))))
            {
                return false;
            }
            ComPtr<ID3DBlob> resolveShader;
            if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/ScreenSpaceReflectionsResolveCS.cso", &resolveShader)))
            {
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC resolvePso{};
            resolvePso.pRootSignature = g_ResolveRootSignature.Get();
            resolvePso.CS = CD3DX12_SHADER_BYTECODE(resolveShader.Get());
            if (FAILED(device->CreateComputePipelineState(&resolvePso, IID_PPV_ARGS(&g_ResolvePso))))
            {
                return false;
            }
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
        // Half-resolution trace target (T1), same format.
        const uint32_t traceWidth = (width + 1) / 2;
        const uint32_t traceHeight = (height + 1) / 2;
        const auto traceDescription = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, traceWidth,
                                                                   traceHeight, 1, 1, 1, 0,
                                                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> traceResult;
        if (ID3D12Resource* traceAllocated = MemoryAllocator::CreateResourceAutoLifetime(
                traceDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr))
        {
            traceResult.Attach(traceAllocated);
        }
        else
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &traceDescription,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&traceResult))))
            {
                return false;
            }
        }
        device->CreateShaderResourceView(traceResult.Get(), &srvDesc,
                                         CD3DX12_CPU_DESCRIPTOR_HANDLE(heapCpu, 2, increment));
        device->CreateUnorderedAccessView(traceResult.Get(), nullptr, &uavDesc,
                                          CD3DX12_CPU_DESCRIPTOR_HANDLE(heapCpu, 3, increment));
        // Hit-point anchors (half res, world positions need float32).
        const auto hitDescription = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, traceWidth,
                                                                 traceHeight, 1, 1, 1, 0,
                                                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        g_HitPoints = CreateTarget(device, hitDescription, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!g_HitPoints)
        {
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC hitSrvDesc{};
        hitSrvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        hitSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hitSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hitSrvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(g_HitPoints.Get(), &hitSrvDesc,
                                         CD3DX12_CPU_DESCRIPTOR_HANDLE(heapCpu, 4, increment));
        D3D12_UNORDERED_ACCESS_VIEW_DESC hitUavDesc{};
        hitUavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        hitUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(g_HitPoints.Get(), nullptr, &hitUavDesc,
                                          CD3DX12_CPU_DESCRIPTOR_HANDLE(heapCpu, 5, increment));
        g_HitInSrvState = false;
        // Temporal accumulation ping-pong (full res).
        for (int i = 0; i < 2; ++i)
        {
            g_Accumulation[i] = CreateTarget(device, description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (!g_Accumulation[i])
            {
                return false;
            }
            device->CreateShaderResourceView(g_Accumulation[i].Get(), &srvDesc,
                                             CD3DX12_CPU_DESCRIPTOR_HANDLE(heapCpu, 6 + i, increment));
            device->CreateUnorderedAccessView(g_Accumulation[i].Get(), nullptr, &uavDesc,
                                              CD3DX12_CPU_DESCRIPTOR_HANDLE(heapCpu, 8 + i, increment));
        }
        g_AccumulationCurrent = 0;
        g_HistoryValid = false;
        g_TraceResult = traceResult;
        g_TraceWidth = traceWidth;
        g_TraceHeight = traceHeight;
        g_TraceInSrvState = false;
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
        g_TraceResult.Reset();
        g_HitPoints.Reset();
        g_Accumulation[0].Reset();
        g_Accumulation[1].Reset();
        g_HistoryValid = false;
        g_HitInSrvState = false;
        g_TraceWidth = 0;
        g_TraceHeight = 0;
        g_TraceInSrvState = false;
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
        if (commandList == nullptr || engineHeap == nullptr || !g_Result || !g_TraceResult || !g_HitPoints || !g_Pso ||
            !g_ResolvePso)
        {
            return false;
        }
        ID3D12DescriptorHeap* heaps[] = {engineHeap, g_Heap.Get()};
        commandList->SetDescriptorHeaps(2, heaps);
        const auto heapGpu = g_Heap->GetGPUDescriptorHandleForHeapStart();
        // Trace at half resolution into the trace target.
        if (g_TraceInSrvState)
        {
            D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(
                g_TraceResult.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1, &toUav);
            g_TraceInSrvState = false;
        }
        TraceConstants traceConstants = inputs.Constants;
        traceConstants.Resolution[0] = static_cast<float>(g_TraceWidth);
        traceConstants.Resolution[1] = static_cast<float>(g_TraceHeight);
        traceConstants.InvResolution[0] = 1.0f / static_cast<float>(g_TraceWidth);
        traceConstants.InvResolution[1] = 1.0f / static_cast<float>(g_TraceHeight);
        traceConstants.GBufferScale = 2;
        // Checkerboard parity alternates per executed frame; the
        // same parity feeds the resolve's tap exclusion.
        const int checkerboardParity = inputs.Checkerboard ? static_cast<int>(g_FrameCounter & 1u) : -1;
        traceConstants.CheckerboardParity = static_cast<float>(checkerboardParity);
        ++g_FrameCounter;
        commandList->SetComputeRootSignature(g_RootSignature.Get());
        commandList->SetPipelineState(g_Pso.Get());
        commandList->SetComputeRoot32BitConstants(0, 52, &traceConstants, 0);
        commandList->SetComputeRootDescriptorTable(1, inputs.DepthSrv);
        commandList->SetComputeRootDescriptorTable(2, inputs.HiZSrv);
        commandList->SetComputeRootDescriptorTable(3, inputs.NormalSrv);
        commandList->SetComputeRootDescriptorTable(4, inputs.PreviousColorSrv);
        commandList->SetComputeRootDescriptorTable(5, inputs.MaterialSrv);
        commandList->SetComputeRootDescriptorTable(6, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 3, g_DescriptorIncrement));
        if (g_HitInSrvState)
        {
            D3D12_RESOURCE_BARRIER hitToUav = CD3DX12_RESOURCE_BARRIER::Transition(
                g_HitPoints.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1, &hitToUav);
            g_HitInSrvState = false;
        }
        commandList->SetComputeRootDescriptorTable(7, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 5, g_DescriptorIncrement));
        commandList->Dispatch((g_TraceWidth + 7) / 8, (g_TraceHeight + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER traceToSrv[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(g_TraceResult.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(g_HitPoints.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        commandList->ResourceBarrier(2, traceToSrv);
        g_TraceInSrvState = true;
        g_HitInSrvState = true;
        // Bilateral resolve into the full-resolution result.
        if (g_ResultInSrvState)
        {
            D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(
                g_Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1, &toUav);
            g_ResultInSrvState = false;
        }
        struct ResolveConstants
        {
            uint32_t FullResolution[2];
            float InvFullResolution[2];
            float DepthNumerator;
            float DepthDenominator;
            uint32_t HalfResolution[2];
            float PrevViewProjection[16];
            float HistoryBlend;
            uint32_t HistoryValid;
            int32_t CheckerboardParity;
            float Padding;
        };
        const int accumulationPrevious = 1 - g_AccumulationCurrent;
        // Invariant: both accumulation buffers are in SRV state at
        // frame start (they are created that way and the write side
        // transitions back below), so the current one can always be
        // moved to UAV here.
        {
            D3D12_RESOURCE_BARRIER accumToUav = CD3DX12_RESOURCE_BARRIER::Transition(
                g_Accumulation[g_AccumulationCurrent].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1, &accumToUav);
        }
        ResolveConstants resolve{};
        resolve.FullResolution[0] = g_ResultWidth;
        resolve.FullResolution[1] = g_ResultHeight;
        resolve.InvFullResolution[0] = 1.0f / static_cast<float>(g_ResultWidth);
        resolve.InvFullResolution[1] = 1.0f / static_cast<float>(g_ResultHeight);
        resolve.DepthNumerator = inputs.Constants.DepthNumerator;
        resolve.DepthDenominator = inputs.Constants.DepthDenominator;
        resolve.HalfResolution[0] = g_TraceWidth;
        resolve.HalfResolution[1] = g_TraceHeight;
        std::memcpy(resolve.PrevViewProjection, inputs.PrevViewProjection, sizeof(resolve.PrevViewProjection));
        resolve.HistoryBlend = 0.85f;
        resolve.HistoryValid = (g_HistoryValid && !inputs.ResetHistory) ? 1u : 0u;
        resolve.CheckerboardParity = checkerboardParity;
        commandList->SetComputeRootSignature(g_ResolveRootSignature.Get());
        commandList->SetPipelineState(g_ResolvePso.Get());
        commandList->SetComputeRoot32BitConstants(0, 26, &resolve, 0);
        commandList->SetComputeRootDescriptorTable(1, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 2, g_DescriptorIncrement));
        commandList->SetComputeRootDescriptorTable(2, inputs.DepthSrv);
        commandList->SetComputeRootDescriptorTable(3, inputs.NormalSrv);
        commandList->SetComputeRootDescriptorTable(4, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 4, g_DescriptorIncrement));
        commandList->SetComputeRootDescriptorTable(
            5, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 6 + accumulationPrevious, g_DescriptorIncrement));
        commandList->SetComputeRootDescriptorTable(6, inputs.MaterialSrv);
        commandList->SetComputeRootDescriptorTable(7, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 1, g_DescriptorIncrement));
        commandList->SetComputeRootDescriptorTable(
            8, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 8 + g_AccumulationCurrent, g_DescriptorIncrement));
        commandList->Dispatch((g_ResultWidth + 7) / 8, (g_ResultHeight + 7) / 8, 1);
        {
            D3D12_RESOURCE_BARRIER accumToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                g_Accumulation[g_AccumulationCurrent].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &accumToSrv);
        }
        g_AccumulationCurrent = accumulationPrevious;
        g_HistoryValid = true;
        D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            g_Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toSrv);
        g_ResultInSrvState = true;
        g_ResultProduced = true;
        return true;
    }
} // namespace ScreenSpaceReflections
