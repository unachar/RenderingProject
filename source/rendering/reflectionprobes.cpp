#include "pch.h"
#include "reflectionprobes.h"
#include "camera.h"
#include "frameconstants.h"
#include "componentmanager.h"
#include "graphicsdevice.h"
#include "lightingresources.h"
#include "materialbindings.h"
#include "materialmanager.h"
#include "memoryallocator.h"
#include "modelmanager.h"
#include "renderprofiler.h"
#include "renderersettings.h"
#include "rendererstate.h"
#include "scenepasses.h"
#include "shaderdescription.h"
#include "shaderpipelines.h"
#include "staticmodel.h"
#include "texturemanager.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace ReflectionProbes
{
    namespace
    {
        std::vector<Probe> g_Probes;

        // Prefilter pipeline (EnvironmentPrefilterCS): own root
        // signature (b0 constants, t0 source table, u0 output table,
        // static wrap-U sampler) and a small local descriptor heap
        // (1 SRV + up to kPrefilterLevels UAVs).
        constexpr uint32_t kPrefilterLevels = 9;
        ComPtr<ID3D12RootSignature> g_PrefilterRootSignature;
        ComPtr<ID3D12PipelineState> g_PrefilterPso;
        ComPtr<ID3D12DescriptorHeap> g_PrefilterHeap;

        // Cube -> equirect conversion pipeline (live capture step 1):
        // own root signature (b0 constants, t0 cube table, u0 output
        // table, static sampler) and a 2-slot local heap.
        ComPtr<ID3D12RootSignature> g_ConvertRootSignature;
        ComPtr<ID3D12PipelineState> g_ConvertPso;
        ComPtr<ID3D12DescriptorHeap> g_ConvertHeap;

        bool EnsureConvertPipeline(ID3D12Device* device)
        {
            if (g_ConvertPso)
            {
                return true;
            }
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            CD3DX12_ROOT_PARAMETER parameters[3];
            parameters[0].InitAsConstants(4, 0);
            parameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
            parameters[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0);
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            CD3DX12_ROOT_SIGNATURE_DESC rootDesc(_countof(parameters), parameters, 1, &sampler,
                                                 D3D12_ROOT_SIGNATURE_FLAG_NONE);
            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> errors;
            if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
                FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                   IID_PPV_ARGS(&g_ConvertRootSignature))))
            {
                return false;
            }
            ComPtr<ID3DBlob> shader;
            if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/EnvironmentCubeToEquirectCS.cso", &shader)))
            {
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = g_ConvertRootSignature.Get();
            pso.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
            if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_ConvertPso))))
            {
                return false;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 2;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            return SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_ConvertHeap)));
        }

        bool EnsurePrefilterPipeline(ID3D12Device* device)
        {
            if (g_PrefilterPso)
            {
                return true;
            }
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            CD3DX12_ROOT_PARAMETER parameters[3];
            parameters[0].InitAsConstants(8, 0);
            parameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
            parameters[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0);
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            CD3DX12_ROOT_SIGNATURE_DESC rootDesc(_countof(parameters), parameters, 1, &sampler,
                                                 D3D12_ROOT_SIGNATURE_FLAG_NONE);
            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> errors;
            if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
                FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                   IID_PPV_ARGS(&g_PrefilterRootSignature))))
            {
                return false;
            }
            ComPtr<ID3DBlob> shader;
            if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/EnvironmentPrefilterCS.cso", &shader)))
            {
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = g_PrefilterRootSignature.Get();
            pso.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
            if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_PrefilterPso))))
            {
                return false;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 1 + kPrefilterLevels;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            return SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_PrefilterHeap)));
        }
    } // namespace

    void Clear()
    {
        g_Probes.clear();
    }

    void Add(const Probe& probe)
    {
        g_Probes.push_back(probe);
    }

    uint32_t Count()
    {
        return static_cast<uint32_t>(g_Probes.size());
    }

    const std::vector<Probe>& All()
    {
        return g_Probes;
    }

    int GeneratePrefilteredEnvironment(ID3D12GraphicsCommandList* commandList, int sourceSrvIndex,
                                       const char* registrationName)
    {
        ID3D12Device* device = GraphicsDevice::GetDevice();
        ComPtr<ID3D12Resource> source = TextureManager::GetTextureResource(sourceSrvIndex);
        if (commandList == nullptr || device == nullptr || !source || registrationName == nullptr)
        {
            return sourceSrvIndex;
        }
        RenderProfiler::ScopedEvent profile("Reflection Probe Prefilter", commandList);
        const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
        if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.MipLevels >= kPrefilterLevels)
        {
            return sourceSrvIndex; // already prefiltered (or not a 2D source)
        }
        if (!EnsurePrefilterPipeline(device))
        {
            return sourceSrvIndex;
        }
        const UINT outputWidth = (std::min)(static_cast<UINT>(sourceDesc.Width), 1024u);
        const UINT outputHeight =
            (std::max)(1u, static_cast<UINT>(outputWidth * sourceDesc.Height / (std::max)(1u, static_cast<UINT>(sourceDesc.Width))));
        uint32_t fullChain = 0;
        for (UINT size = outputWidth; size > 0; size >>= 1)
        {
            ++fullChain;
        }
        const uint32_t levels = (std::min)(kPrefilterLevels, fullChain);

        D3D12_RESOURCE_DESC outputDesc{};
        outputDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        outputDesc.Width = outputWidth;
        outputDesc.Height = outputHeight;
        outputDesc.DepthOrArraySize = 1;
        outputDesc.MipLevels = static_cast<UINT16>(levels);
        outputDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        outputDesc.SampleDesc.Count = 1;
        outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> output;
        if (ID3D12Resource* allocated =
                MemoryAllocator::CreateResourceAutoLifetime(outputDesc, D3D12_RESOURCE_STATE_COMMON, nullptr))
        {
            output.Attach(allocated);
        }
        else
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &outputDesc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output))))
            {
                return sourceSrvIndex;
            }
        }

        const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CD3DX12_CPU_DESCRIPTOR_HANDLE heapCpu(g_PrefilterHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = sourceDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = sourceDesc.MipLevels;
        device->CreateShaderResourceView(source.Get(), &srvDesc, heapCpu);
        for (uint32_t level = 0; level < levels; ++level)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = outputDesc.Format;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = level;
            device->CreateUnorderedAccessView(output.Get(), nullptr, &uavDesc, CD3DX12_CPU_DESCRIPTOR_HANDLE(heapCpu, 1 + static_cast<int>(level), increment));
        }

        commandList->SetComputeRootSignature(g_PrefilterRootSignature.Get());
        commandList->SetPipelineState(g_PrefilterPso.Get());
        ID3D12DescriptorHeap* heaps[] = {g_PrefilterHeap.Get()};
        commandList->SetDescriptorHeaps(1, heaps);
        const CD3DX12_GPU_DESCRIPTOR_HANDLE heapGpu(g_PrefilterHeap->GetGPUDescriptorHandleForHeapStart());
        commandList->SetComputeRootDescriptorTable(1, heapGpu);

        D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(
            output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &toUav);
        struct Constants
        {
            uint32_t OutputSize[2];
            uint32_t SourceSize[2];
            float Roughness;
            uint32_t SampleCount;
            float Padding[2];
        };
        for (uint32_t level = 0; level < levels; ++level)
        {
            Constants constants{};
            constants.OutputSize[0] = (std::max)(1u, outputWidth >> level);
            constants.OutputSize[1] = (std::max)(1u, outputHeight >> level);
            constants.SourceSize[0] = static_cast<uint32_t>(sourceDesc.Width);
            constants.SourceSize[1] = sourceDesc.Height;
            constants.Roughness = static_cast<float>(level) / static_cast<float>(kPrefilterLevels - 1);
            constants.SampleCount = 512;
            commandList->SetComputeRoot32BitConstants(0, 8, &constants, 0);
            commandList->SetComputeRootDescriptorTable(
                2, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 1 + static_cast<int>(level), increment));
            commandList->Dispatch((constants.OutputSize[0] + 7) / 8, (constants.OutputSize[1] + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(output.Get());
            commandList->ResourceBarrier(1, &uavBarrier);
        }
        D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toSrv);
        return TextureManager::ReplaceExternalTexture(registrationName, output);
    }

    namespace
    {
        ComPtr<ID3D12Resource> g_CaptureScratch;
        uint32_t g_CaptureScratchSize = 0;
    }

    ID3D12Resource* EnsureCaptureScratch(uint32_t faceSize)
    {
        if (g_CaptureScratch && g_CaptureScratchSize == faceSize)
        {
            return g_CaptureScratch.Get();
        }
        ID3D12Device* device = GraphicsDevice::GetDevice();
        if (device == nullptr || faceSize == 0)
        {
            return nullptr;
        }
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, faceSize, faceSize, 6, 1, 1, 0,
                                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ComPtr<ID3D12Resource> scratch;
        if (ID3D12Resource* allocated =
                MemoryAllocator::CreateResourceAutoLifetime(desc, D3D12_RESOURCE_STATE_COMMON, nullptr))
        {
            scratch.Attach(allocated);
        }
        else
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&scratch))))
            {
                return nullptr;
            }
        }
        g_CaptureScratch = scratch;
        g_CaptureScratchSize = faceSize;
        return g_CaptureScratch.Get();
    }

    void GetCaptureFaceMatrices(uint32_t face, const float position[3], float viewOut[16], float projectionOut[16])
    {
        // Standard cube-face orientations (D3D): +X, -X, +Y, -Y, +Z, -Z.
        static const float kForward[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        static const float kUp[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
        const uint32_t f = face < 6 ? face : 0;
        const XMVECTOR eye = XMVectorSet(position[0], position[1], position[2], 1.0f);
        const XMVECTOR target = XMVectorAdd(eye, XMVectorSet(kForward[f][0], kForward[f][1], kForward[f][2], 0.0f));
        const XMVECTOR up = XMVectorSet(kUp[f][0], kUp[f][1], kUp[f][2], 0.0f);
        XMFLOAT4X4 view{};
        XMFLOAT4X4 projection{};
        XMStoreFloat4x4(&view, XMMatrixTranspose(XMMatrixLookAtLH(eye, target, up)));
        XMStoreFloat4x4(&projection,
                        XMMatrixTranspose(XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, 100.0f)));
        std::memcpy(viewOut, &view, sizeof(view));
        std::memcpy(projectionOut, &projection, sizeof(projection));
    }

    int ConvertCubeToEquirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* cubeResource,
                              uint32_t width, uint32_t height, const char* registrationName)
    {
        ID3D12Device* device = GraphicsDevice::GetDevice();
        if (commandList == nullptr || device == nullptr || cubeResource == nullptr || registrationName == nullptr ||
            width == 0 || height == 0 || !EnsureConvertPipeline(device))
        {
            return -1;
        }
        const D3D12_RESOURCE_DESC cubeDesc = cubeResource->GetDesc();
        if (cubeDesc.DepthOrArraySize < 6)
        {
            return -1;
        }
        ComPtr<ID3D12Resource> output;
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto outputDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1,
                                                                 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (ID3D12Resource* allocated =
                    MemoryAllocator::CreateResourceAutoLifetime(outputDesc, D3D12_RESOURCE_STATE_COMMON, nullptr))
            {
                output.Attach(allocated);
            }
            else if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &outputDesc,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            IID_PPV_ARGS(&output))))
            {
                return -1;
            }
        }
        const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const CD3DX12_CPU_DESCRIPTOR_HANDLE heapCpu(g_ConvertHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrv{};
        cubeSrv.Format = cubeDesc.Format;
        cubeSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        cubeSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cubeSrv.TextureCube.MipLevels = cubeDesc.MipLevels;
        device->CreateShaderResourceView(cubeResource, &cubeSrv, heapCpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(), nullptr, &uavDesc,
                                          CD3DX12_CPU_DESCRIPTOR_HANDLE(heapCpu, 1, increment));

        commandList->SetComputeRootSignature(g_ConvertRootSignature.Get());
        commandList->SetPipelineState(g_ConvertPso.Get());
        ID3D12DescriptorHeap* heaps[] = {g_ConvertHeap.Get()};
        commandList->SetDescriptorHeaps(1, heaps);
        const CD3DX12_GPU_DESCRIPTOR_HANDLE heapGpu(g_ConvertHeap->GetGPUDescriptorHandleForHeapStart());
        struct ConvertConstants
        {
            uint32_t OutputSize[2];
            uint32_t Padding[2];
        };
        ConvertConstants constants{};
        constants.OutputSize[0] = width;
        constants.OutputSize[1] = height;
        commandList->SetComputeRoot32BitConstants(0, 4, &constants, 0);
        commandList->SetComputeRootDescriptorTable(1, heapGpu);
        commandList->SetComputeRootDescriptorTable(2, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapGpu, 1, increment));
        D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(
            output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &toUav);
        commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toSrv);
        return TextureManager::ReplaceExternalTexture(registrationName, output);
    }

    namespace
    {
        // Live capture state per probe entity. SyncFromScene
        // refreshes Center every frame and marks the state dirty
        // when the probe appears or moves; ProcessPendingCaptures
        // (RenderFrame::EndDraw) serves one dirty state per frame.
        struct CaptureState
        {
            int SrvIndex = -1;
            bool Dirty = true;
            float Center[3] = {0.0f, 0.0f, 0.0f};
            float CapturedCenter[3] = {0.0f, 0.0f, 0.0f};
            bool HasCapture = false;
        };

        std::unordered_map<EntityID, CaptureState> g_CaptureStates;
        EntityID g_LastCaptureEntity = g_kINVALID_ENTITY;
        bool g_HasLastCapture = false;

        constexpr uint32_t kCaptureFaceSize = 128;
        constexpr uint32_t kCaptureEquirectWidth = 256;
        constexpr uint32_t kCaptureEquirectHeight = 128;

        // Capture-only render resources: per-slice RTVs over the
        // shared scratch, one depth target, and forward-lit model
        // PSOs (the production model shaders, following the
        // MaterialPreview recipe) in the three material cull
        // variants.
        ComPtr<ID3D12DescriptorHeap> g_CaptureRtvHeap;
        ComPtr<ID3D12DescriptorHeap> g_CaptureDsvHeap;
        ComPtr<ID3D12Resource> g_CaptureDepth;
        ID3D12Resource* g_CaptureViewsFor = nullptr;
        D3D12_RESOURCE_STATES g_CaptureScratchState = D3D12_RESOURCE_STATE_COMMON;
        ComPtr<ID3D12PipelineState> g_CapturePsos[3];
        bool g_CapturePsoFailed = false;

        bool EnsureCaptureViews(ID3D12Device* device, ID3D12Resource* scratch)
        {
            if (g_CaptureViewsFor == scratch && g_CaptureRtvHeap && g_CaptureDepth)
            {
                return true;
            }
            const D3D12_RESOURCE_DESC scratchDesc = scratch->GetDesc();
            const uint32_t faceSize = static_cast<uint32_t>(scratchDesc.Width);
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = 6;
            if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_CaptureRtvHeap))))
            {
                return false;
            }
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            heapDesc.NumDescriptors = 1;
            if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_CaptureDsvHeap))))
            {
                return false;
            }
            const UINT rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            for (uint32_t face = 0; face < 6; ++face)
            {
                D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
                rtvDesc.Format = scratchDesc.Format;
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = 0;
                rtvDesc.Texture2DArray.FirstArraySlice = face;
                rtvDesc.Texture2DArray.ArraySize = 1;
                device->CreateRenderTargetView(scratch, &rtvDesc,
                                               CD3DX12_CPU_DESCRIPTOR_HANDLE(
                                                   g_CaptureRtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                                   static_cast<int>(face), rtvIncrement));
            }
            const auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, faceSize, faceSize, 1, 1, 1,
                                                                0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            D3D12_CLEAR_VALUE clear{};
            clear.Format = DXGI_FORMAT_D32_FLOAT;
            clear.DepthStencil.Depth = 0.0f;
            g_CaptureDepth.Reset();
            if (ID3D12Resource* allocated =
                    MemoryAllocator::CreateResourceAutoLifetime(depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear))
            {
                g_CaptureDepth.Attach(allocated);
            }
            else
            {
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                                           D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                           IID_PPV_ARGS(&g_CaptureDepth))))
                {
                    return false;
                }
            }
            device->CreateDepthStencilView(g_CaptureDepth.Get(), nullptr,
                                           g_CaptureDsvHeap->GetCPUDescriptorHandleForHeapStart());
            g_CaptureViewsFor = scratch;
            g_CaptureScratchState = D3D12_RESOURCE_STATE_COMMON;
            return true;
        }

        bool EnsureCapturePsos(ID3D12Device* device)
        {
            if (g_CapturePsos[0])
            {
                return true;
            }
            if (g_CapturePsoFailed)
            {
                return false;
            }
            ComPtr<ID3DBlob> vertex;
            ComPtr<ID3DBlob> pixel;
            ShaderDescription shader{};
            shader.csoPath = "shader/hlsl/build/modelshaderVS.cso";
            shader.ppBlob = vertex.GetAddressOf();
            if (!ShaderPipelines::LoadShaderBlob(shader))
            {
                g_CapturePsoFailed = true;
                return false;
            }
            shader.csoPath = "shader/hlsl/build/modelshaderPS.cso";
            shader.ppBlob = pixel.GetAddressOf();
            if (!ShaderPipelines::LoadShaderBlob(shader))
            {
                g_CapturePsoFailed = true;
                return false;
            }
            const D3D12_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
            const D3D12_CULL_MODE cullModes[3] = {D3D12_CULL_MODE_BACK, D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_FRONT};
            for (int variant = 0; variant < 3; ++variant)
            {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
                desc.pRootSignature = ShaderPipelines::GetModelRootSignature();
                desc.VS = CD3DX12_SHADER_BYTECODE(vertex.Get());
                desc.PS = CD3DX12_SHADER_BYTECODE(pixel.Get());
                desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
                desc.RasterizerState.CullMode = cullModes[variant];
                desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
                desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reverse-Z
                desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
                desc.SampleMask = UINT_MAX;
                desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                desc.NumRenderTargets = 1;
                desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
                desc.SampleDesc.Count = 1;
                desc.InputLayout = {layout, _countof(layout)};
                if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_CapturePsos[variant]))))
                {
                    g_CapturePsoFailed = true;
                    return false;
                }
            }
            return true;
        }

        // Renders the scene's opaque static models into the six
        // scratch faces from the probe position (forward lit with
        // the scene's own lights, shadow maps and environment via
        // the standard BeginPass bindings), then converts and
        // prefilters the cube and publishes it as the probe's
        // environment. Animated/skinned and GPU-driven-only
        // geometry is not captured in this version.
        bool CaptureProbe(ID3D12GraphicsCommandList* commandList, EntityID entity, CaptureState& state)
        {
            ID3D12Device* device = GraphicsDevice::GetDevice();
            ID3D12Resource* scratch = EnsureCaptureScratch(kCaptureFaceSize);
            if (device == nullptr || scratch == nullptr || !EnsureCaptureViews(device, scratch) ||
                !EnsureCapturePsos(device))
            {
                return false;
            }
            RenderProfiler::ScopedEvent profile("Reflection Probe Capture", commandList);
            const float center[3] = {state.Center[0], state.Center[1], state.Center[2]};
            {
                D3D12_RESOURCE_BARRIER toRt = CD3DX12_RESOURCE_BARRIER::Transition(
                    scratch, g_CaptureScratchState, D3D12_RESOURCE_STATE_RENDER_TARGET);
                commandList->ResourceBarrier(1, &toRt);
                g_CaptureScratchState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            ScenePasses::BeginPass(ShaderPipelines::GetModelRootSignature(),
                                   D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            // b5 (root 17) takes the same light constant buffer the
            // production mesh draws bind.
            if (const D3D12_GPU_VIRTUAL_ADDRESS lights = LightingResources::GetCurrentLightConstantBufferAddress())
            {
                commandList->SetGraphicsRootConstantBufferView(17, lights);
            }
            const UINT rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            const CD3DX12_CPU_DESCRIPTOR_HANDLE rtvBase(g_CaptureRtvHeap->GetCPUDescriptorHandleForHeapStart());
            const CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(g_CaptureDsvHeap->GetCPUDescriptorHandleForHeapStart());
            const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kCaptureFaceSize),
                                          static_cast<float>(kCaptureFaceSize), 0.0f, 1.0f};
            const D3D12_RECT scissor{0, 0, static_cast<LONG>(kCaptureFaceSize),
                                     static_cast<LONG>(kCaptureFaceSize)};
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
            const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            const int defaultTexture = TextureManager::GetDefaultTextureIndex();
            const UINT cbvIncrement = FrameConstants::GetCbvIncrementSize();
            const CD3DX12_GPU_DESCRIPTOR_HANDLE heapStart(FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart());
            bool exhausted = false;
            for (uint32_t face = 0; face < 6 && !exhausted; ++face)
            {
                const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtvBase, static_cast<int>(face), rtvIncrement);
                commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
                commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                float viewData[16];
                float projectionData[16];
                GetCaptureFaceMatrices(face, center, viewData, projectionData);
                XMFLOAT4X4 viewStored{};
                XMFLOAT4X4 projectionStored{};
                std::memcpy(&viewStored, viewData, sizeof(viewStored));
                std::memcpy(&projectionStored, projectionData, sizeof(projectionStored));
                const XMMATRIX viewProjection = XMMatrixTranspose(XMLoadFloat4x4(&viewStored)) *
                                                XMMatrixTranspose(XMLoadFloat4x4(&projectionStored));
                ComponentManager::ForEach<StaticModelComponent, TransformComponent>(
                    [&](EntityID modelEntity, StaticModelComponent& modelComponent,
                        TransformComponent& transform) {
                        if (exhausted)
                        {
                            return;
                        }
                        StaticModelResource* model = ModelManager::GetStaticModel(modelComponent.ModelId);
                        if (model == nullptr)
                        {
                            return;
                        }
                        MaterialComponent fallback{};
                        const MaterialComponent* materialSource = &fallback;
                        if (ComponentManager::HasComponent<MaterialComponent>(modelEntity))
                        {
                            materialSource = &ComponentManager::GetComponentUnchecked<MaterialComponent>(modelEntity);
                        }
                        const MaterialComponent& material = MaterialManager::Resolve(*materialSource);
                        if (material.IsTransparent)
                        {
                            return;
                        }
                        const int cullVariant = material.AssetCull == MaterialCull::None
                                                    ? 1
                                                    : (material.AssetCull == MaterialCull::Front ? 2 : 0);
                        commandList->SetPipelineState(g_CapturePsos[cullVariant].Get());
                        MaterialBindings::SetMaterial(modelEntity, material);
                        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
                        {
                            const StaticMeshData& mesh = model->GetMeshData(meshIndex);
                            if (mesh.IndexCount == 0)
                            {
                                continue;
                            }
                            ConstantBuffer3D constants{};
                            constants.World = XMMatrixTranspose(XMLoadFloat4x4(&transform.WorldMatrix));
                            constants.PreviousWorld = constants.World;
                            constants.View = XMLoadFloat4x4(&viewStored);
                            constants.Projection = XMLoadFloat4x4(&projectionStored);
                            constants.PreviousViewProjection = XMMatrixTranspose(viewProjection);
                            constants.CameraPos = {center[0], center[1], center[2]};
                            constants.ViewportSize = {static_cast<float>(kCaptureFaceSize),
                                                      static_cast<float>(kCaptureFaceSize)};
                            const bool useModelTexture = material.RuntimeHandle == 0 &&
                                                         !material.LiveSurfaceOverride && mesh.TextureIndex >= 0;
                            constants.UseTexture = (material.UseTexture || useModelTexture) ? 1 : 0;
                            constants.UseNormalMap = material.NormalMapID >= 0 ? 1 : 0;
                            constants.FlipNormal = 0;
                            if (materialSource == &fallback)
                            {
                                // No material component: shade as a
                                // plain PBR surface, like the
                                // material preview's house material.
                                constants.MaterialMode = 1;
                                constants.ShaderClass = static_cast<int>(ShaderClass::PBR);
                            }
                            else
                            {
                                constants.MaterialMode = static_cast<int>(material.ShaderClassMode);
                                constants.ShaderClass = static_cast<int>(material.ShaderClass);
                            }
                            constants.MaterialMetallic = material.Metallic;
                            constants.MaterialRoughness = material.Roughness;
                            constants.MaterialFresnel = material.Fresnel;
                            constants.MaterialAlpha = material.Alpha;
                            constants.MaterialIsTransparent = 0;
                            const auto drawConstants = FrameConstants::AllocateTransientConstantBuffer(constants);
                            if (!drawConstants.ptr)
                            {
                                exhausted = true;
                                return;
                            }
                            commandList->SetGraphicsRootDescriptorTable(0, drawConstants);
                            const int albedo = material.TextureID >= 0
                                                   ? material.TextureID
                                                   : (mesh.TextureIndex >= 0 ? mesh.TextureIndex : defaultTexture);
                            const int normal = material.NormalMapID >= 0 ? material.NormalMapID : defaultTexture;
                            commandList->SetGraphicsRootDescriptorTable(
                                1, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapStart, albedo, cbvIncrement));
                            commandList->SetGraphicsRootDescriptorTable(
                                6, CD3DX12_GPU_DESCRIPTOR_HANDLE(heapStart, normal, cbvIncrement));
                            commandList->IASetVertexBuffers(0, 1, &mesh.VertexBufferView);
                            commandList->IASetIndexBuffer(&mesh.IndexBufferView);
                            commandList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
                        }
                    });
            }
            {
                D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                    scratch, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                commandList->ResourceBarrier(1, &toSrv);
                g_CaptureScratchState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            if (exhausted)
            {
                return false;
            }
            const std::string base = "probe://capture/" + std::to_string(entity);
            const int equirect = ConvertCubeToEquirect(commandList, scratch, kCaptureEquirectWidth,
                                                       kCaptureEquirectHeight, base.c_str());
            if (equirect < 0)
            {
                return false;
            }
            const std::string prefiltered = base + "#prefiltered";
            const int srv = GeneratePrefilteredEnvironment(commandList, equirect, prefiltered.c_str());
            if (srv < 0)
            {
                return false;
            }
            state.SrvIndex = srv;
            state.CapturedCenter[0] = center[0];
            state.CapturedCenter[1] = center[1];
            state.CapturedCenter[2] = center[2];
            state.HasCapture = true;
            state.Dirty = false;
            return true;
        }
    } // namespace

    void ProcessPendingCaptures(ID3D12GraphicsCommandList* commandList)
    {
        if (commandList == nullptr || !RendererSettings::GetUseLocalReflectionProbes())
        {
            return;
        }
        // Round-robin: the dirty probe with the smallest entity id
        // after the last served one, wrapping to the smallest.
        EntityID chosen = g_kINVALID_ENTITY;
        EntityID smallest = g_kINVALID_ENTITY;
        for (const auto& [entity, state] : g_CaptureStates)
        {
            if (!state.Dirty)
            {
                continue;
            }
            if (smallest == g_kINVALID_ENTITY || entity < smallest)
            {
                smallest = entity;
            }
            if (g_HasLastCapture && entity > g_LastCaptureEntity &&
                (chosen == g_kINVALID_ENTITY || entity < chosen))
            {
                chosen = entity;
            }
        }
        if (chosen == g_kINVALID_ENTITY)
        {
            chosen = smallest;
        }
        if (chosen == g_kINVALID_ENTITY)
        {
            return;
        }
        CaptureProbe(commandList, chosen, g_CaptureStates[chosen]);
        g_LastCaptureEntity = chosen;
        g_HasLastCapture = true;
    }

    void SyncFromScene(ID3D12GraphicsCommandList* commandList)
    {
        Clear();
        static std::unordered_map<std::string, int> textureCache;
        static std::unordered_set<std::string> prefilterAttempted;
        std::unordered_set<EntityID> liveCaptureEntities;
        ComponentManager::ForEach<ReflectionProbeComponent, TransformComponent>(
            [&](EntityID entity, ReflectionProbeComponent& probeComponent, TransformComponent& transform) {
                if (!probeComponent.IsActive)
                {
                    return;
                }
                int authoredIndex = -1;
                if (!probeComponent.EnvironmentTexturePath.empty())
                {
                    auto found = textureCache.find(probeComponent.EnvironmentTexturePath);
                    if (found == textureCache.end())
                    {
                        int srvIndex = TextureManager::LoadTexture(probeComponent.EnvironmentTexturePath.c_str());
                        if (probeComponent.GeneratePrefilter && commandList != nullptr &&
                            prefilterAttempted.insert(probeComponent.EnvironmentTexturePath).second)
                        {
                            const std::string registration = probeComponent.EnvironmentTexturePath + "#prefiltered";
                            srvIndex = GeneratePrefilteredEnvironment(commandList, srvIndex, registration.c_str());
                        }
                        found = textureCache.emplace(probeComponent.EnvironmentTexturePath, srvIndex).first;
                    }
                    authoredIndex = found->second;
                }
                int srvIndex = authoredIndex;
                if (probeComponent.CaptureLive)
                {
                    liveCaptureEntities.insert(entity);
                    CaptureState& state = g_CaptureStates[entity];
                    state.Center[0] = transform.Position.x;
                    state.Center[1] = transform.Position.y;
                    state.Center[2] = transform.Position.z;
                    if (state.HasCapture)
                    {
                        float moveSq = 0.0f;
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            const float delta = state.Center[axis] - state.CapturedCenter[axis];
                            moveSq += delta * delta;
                        }
                        if (moveSq > 0.05f * 0.05f)
                        {
                            state.Dirty = true;
                        }
                    }
                    // The capture wins once one exists; until the
                    // first capture lands the probe keeps serving
                    // its authored texture (or stays inactive).
                    if (state.SrvIndex >= 0)
                    {
                        srvIndex = state.SrvIndex;
                    }
                }
                if (srvIndex < 0 && !probeComponent.CaptureLive)
                {
                    return;
                }
                Probe probe{};
                probe.Center[0] = transform.Position.x;
                probe.Center[1] = transform.Position.y;
                probe.Center[2] = transform.Position.z;
                probe.HalfExtents[0] = (std::max)(probeComponent.InfluenceSize.x, 0.1f) * 0.5f;
                probe.HalfExtents[1] = (std::max)(probeComponent.InfluenceSize.y, 0.1f) * 0.5f;
                probe.HalfExtents[2] = (std::max)(probeComponent.InfluenceSize.z, 0.1f) * 0.5f;
                probe.EnvironmentSrvIndex = srvIndex;
                probe.Priority = probeComponent.Priority;
                probe.Intensity = probeComponent.Intensity;
                Add(probe);
            });
        // States of probes that were deleted or stopped capturing
        // are dropped; their registered textures stay in
        // TextureManager (bounded by the probe count).
        for (auto it = g_CaptureStates.begin(); it != g_CaptureStates.end();)
        {
            if (liveCaptureEntities.find(it->first) == liveCaptureEntities.end())
            {
                it = g_CaptureStates.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    namespace
    {
        Probe g_ActiveProbe{};
        bool g_HasActiveProbe = false;
        Probe g_FrameProbes[2]{};
        int g_FrameBindless[2] = {-1, -1};
        int g_FrameProbeCount = 0;
    } // namespace

    const Probe* SelectProbe(const float cameraPosition[3])
    {
        const Probe* best = nullptr;
        int bestPriority = 0;
        float bestDistanceSq = 0.0f;
        for (const Probe& probe : g_Probes)
        {
            if (probe.EnvironmentSrvIndex < 0)
            {
                continue;
            }
            float distanceSq = 0.0f;
            bool inside = true;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float delta = cameraPosition[axis] - probe.Center[axis];
                if (std::fabs(delta) > probe.HalfExtents[axis])
                {
                    inside = false;
                    break;
                }
                distanceSq += delta * delta;
            }
            if (!inside)
            {
                continue;
            }
            if (best == nullptr || probe.Priority > bestPriority ||
                (probe.Priority == bestPriority && distanceSq < bestDistanceSq))
            {
                best = &probe;
                bestPriority = probe.Priority;
                bestDistanceSq = distanceSq;
            }
        }
        return best;
    }

    void UpdateActiveEnvironment(ID3D12GraphicsCommandList* commandList)
    {
        g_HasActiveProbe = false;
        g_FrameProbeCount = 0;
        if (!RendererSettings::GetUseLocalReflectionProbes())
        {
            RendererState::m_EnvironmentTextureOverrideSrvIndex = -1;
            return;
        }
        SyncFromScene(commandList);
        const EntityID cameraEntity = Camera::GetCameraEntity();
        if (!ComponentManager::HasComponent<TransformComponent>(cameraEntity))
        {
            RendererState::m_EnvironmentTextureOverrideSrvIndex = -1;
            return;
        }
        TransformComponent& cameraTransform = ComponentManager::GetComponent<TransformComponent>(cameraEntity);
        const float cameraPosition[3] = {cameraTransform.Position.x, cameraTransform.Position.y,
                                         cameraTransform.Position.z};
        g_FrameProbeCount = SelectTopProbes(cameraPosition, g_FrameProbes, g_FrameBindless);
        if (g_FrameProbeCount > 0)
        {
            g_ActiveProbe = g_FrameProbes[0];
            g_HasActiveProbe = true;
        }
        // One probe: the frame-global SRV override (v1 route). Two
        // probes: the per-pixel bindless blend consumes them, so the
        // override stays off and every consumer keeps the global
        // environment texture as the blend remainder.
        RendererState::m_EnvironmentTextureOverrideSrvIndex =
            g_FrameProbeCount == 1 ? g_FrameProbes[0].EnvironmentSrvIndex : -1;
    }

    int GetFrameProbes(Probe out[2], int outBindlessIndices[2])
    {
        for (int i = 0; i < g_FrameProbeCount; ++i)
        {
            out[i] = g_FrameProbes[i];
            outBindlessIndices[i] = g_FrameBindless[i];
        }
        return g_FrameProbeCount;
    }

    int SelectEnvironmentSrvIndex(const float cameraPosition[3])
    {
        const Probe* selected = SelectProbe(cameraPosition);
        return selected != nullptr ? selected->EnvironmentSrvIndex : -1;
    }

    bool GetActiveProbeBox(float center[3], float halfExtents[3], float& intensity)
    {
        if (!g_HasActiveProbe)
        {
            return false;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            center[axis] = g_ActiveProbe.Center[axis];
            halfExtents[axis] = g_ActiveProbe.HalfExtents[axis];
        }
        intensity = g_ActiveProbe.Intensity;
        return true;
    }

    int SelectTopProbes(const float point[3], Probe out[2], int outBindlessIndices[2])
    {
        // Ranked top-2 (priority first, then distance to the probe
        // center - the SelectProbe ordering), maintained by insertion.
        float rankedDistanceSq[2] = {0.0f, 0.0f};
        int count = 0;
        auto outranks = [](const Probe& a, float aDistanceSq, const Probe& b, float bDistanceSq) {
            return a.Priority > b.Priority || (a.Priority == b.Priority && aDistanceSq < bDistanceSq);
        };
        for (const Probe& probe : g_Probes)
        {
            if (probe.EnvironmentSrvIndex < 0)
            {
                continue;
            }
            float distanceSq = 0.0f;
            bool inside = true;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float delta = point[axis] - probe.Center[axis];
                if (std::fabs(delta) > probe.HalfExtents[axis])
                {
                    inside = false;
                    break;
                }
                distanceSq += delta * delta;
            }
            if (!inside)
            {
                continue;
            }
            const int bindless = probe.EnvironmentSrvIndex - FrameConstants::g_kTEXTURE_SRV_START_INDEX;
            if (count < 2)
            {
                out[count] = probe;
                outBindlessIndices[count] = bindless;
                rankedDistanceSq[count] = distanceSq;
                ++count;
            }
            else if (outranks(probe, distanceSq, out[1], rankedDistanceSq[1]))
            {
                out[1] = probe;
                outBindlessIndices[1] = bindless;
                rankedDistanceSq[1] = distanceSq;
            }
            if (count == 2 && outranks(out[1], rankedDistanceSq[1], out[0], rankedDistanceSq[0]))
            {
                std::swap(out[0], out[1]);
                std::swap(outBindlessIndices[0], outBindlessIndices[1]);
                std::swap(rankedDistanceSq[0], rankedDistanceSq[1]);
            }
        }
        return count;
    }
} // namespace ReflectionProbes
