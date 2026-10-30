#include "pch.h"
#include "reflectionprobes.h"
#include "camera.h"
#include "frameconstants.h"
#include "componentmanager.h"
#include "graphicsdevice.h"
#include "memoryallocator.h"
#include "renderersettings.h"
#include "rendererstate.h"
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
        return TextureManager::RegisterExternalTexture(registrationName, output);
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
        return TextureManager::RegisterExternalTexture(registrationName, output);
    }

    void SyncFromScene(ID3D12GraphicsCommandList* commandList)
    {
        Clear();
        static std::unordered_map<std::string, int> textureCache;
        static std::unordered_set<std::string> prefilterAttempted;
        ComponentManager::ForEach<ReflectionProbeComponent, TransformComponent>(
            [&](EntityID entity, ReflectionProbeComponent& probeComponent, TransformComponent& transform) {
                (void)entity;
                if (!probeComponent.IsActive || probeComponent.EnvironmentTexturePath.empty())
                {
                    return;
                }
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
                Probe probe{};
                probe.Center[0] = transform.Position.x;
                probe.Center[1] = transform.Position.y;
                probe.Center[2] = transform.Position.z;
                probe.HalfExtents[0] = (std::max)(probeComponent.InfluenceSize.x, 0.1f) * 0.5f;
                probe.HalfExtents[1] = (std::max)(probeComponent.InfluenceSize.y, 0.1f) * 0.5f;
                probe.HalfExtents[2] = (std::max)(probeComponent.InfluenceSize.z, 0.1f) * 0.5f;
                probe.EnvironmentSrvIndex = found->second;
                probe.Priority = probeComponent.Priority;
                probe.Intensity = probeComponent.Intensity;
                Add(probe);
            });
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
