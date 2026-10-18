#pragma once
// =============================================================================================
// AtmosphereLut
//   物理スカイの LUT 化（Hillaire 2020 方式）。
//   ・Transmittance LUT（光学的厚さ, 256x64）: 惑星形状のみに依存するため初回のみ生成
//   ・MultiScattering LUT（32x32）           : 大気係数が変わった時だけ再生成（多重散乱）
//   ・SkyView LUT（192x108, 散乱 + 透過率）   : 毎フレーム（カメラ高度・太陽方向）
//   生成は AsyncCompute のキューで行い、グラフィックス側は GPU 上で完了を待つ。
//   SkyView はフレームスロットごとに 3 組持ち、前フレームの読み取りと次フレームの書き込みが
//   重ならないようにする。テクスチャは SIMULTANEOUS_ACCESS で作成し、キュー間の状態遷移は
//   暗黙の昇格／減衰（COMMON）に任せる。
//   ヘッダーのみで完結（.vcxproj への追加不要）。
// =============================================================================================
#include "pch.h"
#include "rendergraphqueueexecutor.h"
#include "envflags.h"
#include "rendererstate.h"
#include "renderprofiler.h"
#include "asynccompute.h"
#include "graphicslog.h"
#include "camera.h"
#include "atmosphere.h"
#include "daynightcycle.h"
#include "componentmanager.h"
#include "light.h"
#include "world.h"
#include <cmath>
#include <algorithm>

class AtmosphereLut final
{
  public:
    struct Settings
    {
        bool Enabled = true;
        float MultipleScattering = 1.0f; // 0 = 従来と同じ単一散乱のみ
        float GroundAlbedo = 0.1f;       // 多重散乱で地表から返る光
    };

    static Settings& GetSettings()
    {
        return s_Settings;
    }

    static bool IsAvailable()
    {
        return s_Available;
    }

    static bool WasBuiltAsync()
    {
        return s_LastAsync;
    }

    static bool Initialize(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap)
    {
        Shutdown();
        if (!device || !engineHeap)
        {
            return false;
        }
        s_Device = device;
        s_EngineHeap = engineHeap;
        s_Increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        // The null pair is written first: the deferred lighting pass always binds this table,
        // so it must be valid even if the LUT cannot be created.
        WriteSrv(nullptr, EngineCpu(kFrames * 2));
        WriteSrv(nullptr, EngineCpu(kFrames * 2 + 1));

        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0);
        CD3DX12_ROOT_PARAMETER parameters[3];
        parameters[0].InitAsConstantBufferView(0);
        parameters[1].InitAsDescriptorTable(1, &ranges[0]);
        parameters[2].InitAsDescriptorTable(1, &ranges[1]);
        const CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                  D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        const CD3DX12_ROOT_SIGNATURE_DESC root(3, parameters, 1, &sampler);
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(
                device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s_Root))))
        {
            return Fail("root signature");
        }

        const struct
        {
            const char* Entry;
            ComPtr<ID3D12PipelineState>* Pipeline;
        } entries[] = {{"BuildTransmittance", &s_Transmittance},
                       {"BuildMultiScatter", &s_MultiScatter},
                       {"BuildSkyView", &s_SkyView}};

        for (const auto& entry : entries)
        {
            blob.Reset();
            errors.Reset();
            if (FAILED(D3DCompileFromFile(L"shader/hlsl/AtmosphereLutCS.hlsl", nullptr,
                                          D3D_COMPILE_STANDARD_FILE_INCLUDE, entry.Entry, "cs_6_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
            {
                WriteGraphicsLog("AtmosphereLut: %s: %s\n", entry.Entry,
                                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                return Fail("shader compile");
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = s_Root.Get();
            desc.CS = CD3DX12_SHADER_BYTECODE(blob.Get());
            if (FAILED(
                    device->CreateComputePipelineState(&desc, IID_PPV_ARGS(entry.Pipeline->ReleaseAndGetAddressOf()))))
            {
                return Fail("pipeline");
            }
        }

        if (!CreateLut(s_TransmittanceLut, 256, 64, DXGI_FORMAT_R32G32_FLOAT, L"Atmosphere Transmittance LUT") ||
            !CreateLut(s_MultiScatterLut, 32, 32, DXGI_FORMAT_R16G16B16A16_FLOAT, L"Atmosphere MultiScattering LUT"))
        {
            return Fail("LUT textures");
        }
        for (UINT slot = 0; slot < kFrames; ++slot)
        {
            if (!CreateLut(s_SkyScatter[slot], 192, 108, DXGI_FORMAT_R16G16B16A16_FLOAT,
                           L"Atmosphere SkyView Scatter LUT") ||
                !CreateLut(s_SkyTransmission[slot], 192, 108, DXGI_FORMAT_R16G16B16A16_FLOAT,
                           L"Atmosphere SkyView Transmission LUT"))
            {
                return Fail("LUT textures");
            }
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heap.NumDescriptors = kRegionSize * kRegionCount;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s_Heap))))
        {
            return Fail("descriptor heap");
        }
        const auto upload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(256ull * kFrames);
        if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&s_Constants))))
        {
            return Fail("constants");
        }

        // Region 0: transmittance build, region 1: multi scattering build, region 2+slot: sky view.
        WriteRegion(0, nullptr, nullptr, nullptr, nullptr, s_TransmittanceLut.Get());
        WriteRegion(1, s_TransmittanceLut.Get(), nullptr, s_MultiScatterLut.Get(), nullptr, nullptr);
        for (UINT slot = 0; slot < kFrames; ++slot)
        {
            WriteRegion(2 + slot, s_TransmittanceLut.Get(), s_MultiScatterLut.Get(), s_SkyScatter[slot].Get(),
                        s_SkyTransmission[slot].Get(), nullptr);
        }

        // Engine heap: (scatter, transmission) per slot, plus a null pair for "disabled".
        // Freshly created committed textures are zero (alpha 0) -> consumers fall back to the
        // analytic sky until the slot has been built once.
        for (UINT slot = 0; slot < kFrames; ++slot)
        {
            WriteSrv(s_SkyScatter[slot].Get(), EngineCpu(slot * 2));
            WriteSrv(s_SkyTransmission[slot].Get(), EngineCpu(slot * 2 + 1));
        }
        WriteSrv(nullptr, EngineCpu(kFrames * 2));
        WriteSrv(nullptr, EngineCpu(kFrames * 2 + 1));
        s_TransmittanceBuilt = false;
        s_LastSignature = 0;
        s_Available = true;
        WriteGraphicsLog(
            "AtmosphereLut: initialized (transmittance 256x64, multiple scattering 32x32, sky view 192x108)\n");
        return true;
    }

    static void Shutdown()
    {
        ReleaseResources();
        s_EngineHeap.Reset();
        s_Device.Reset();
    }

    static void ReleaseResources()
    {
        s_GraphExecutor.reset();
        AsyncCompute::WaitIdle();
        s_Available = false;
        s_Valid = false;
        s_Root.Reset();
        s_Transmittance.Reset();
        s_MultiScatter.Reset();
        s_SkyView.Reset();
        s_TransmittanceLut.Reset();
        s_MultiScatterLut.Reset();
        for (auto& texture : s_SkyScatter)
        {
            texture.Reset();
        }
        for (auto& texture : s_SkyTransmission)
        {
            texture.Reset();
        }
        s_Heap.Reset();
        s_Constants.Reset();
    }

    // Called once per frame at the start of recording (RenderFrame::BeginDraw).
    // graphicsList is the fallback when no async compute queue is active.
    static void Update(ID3D12GraphicsCommandList* graphicsList, ID3D12CommandQueue* graphicsQueue, UINT frameSlot)
    {
        s_Valid = false;
        if (!s_Available || !s_Settings.Enabled)
        {
            return;
        }
        const UINT slot = frameSlot % kFrames;
        const auto& air = Atmosphere::GetParameters();
        if (!air.Enabled || !air.PhysicalSky)
        {
            return;
        }

        struct alignas(16) Constants
        {
            XMFLOAT4 SunDirection, Coefficients, Parameters;
        } constants{};

        const XMFLOAT3 sun = ResolveSkySunDirection();
        constants.SunDirection = {sun.x, sun.y, sun.z, 0.0f};
        constants.Coefficients = {(std::max)(0.0f, air.RayleighStrength), (std::max)(0.0f, air.MieStrength),
                                  (std::max)(0.0f, air.Density), std::clamp(air.MieG, -0.95f, 0.95f)};
        constants.Parameters = {CameraAltitude(), (std::max)(0.0f, s_Settings.MultipleScattering),
                                std::clamp(s_Settings.GroundAlbedo, 0.0f, 1.0f), 0.0f};
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(s_Constants->Map(0, &noRead, &mapped)))
        {
            return;
        }
        memcpy(static_cast<BYTE*>(mapped) + slot * 256, &constants, sizeof(constants));
        s_Constants->Unmap(0, nullptr);

        // Multiple scattering depends on the coefficients and the ground albedo only.
        uint64_t signature = 1469598103934665603ull;
        const float keys[] = {constants.Coefficients.x, constants.Coefficients.y, constants.Coefficients.z,
                              constants.Parameters.z};
        for (float key : keys)
        {
            uint32_t bits;
            memcpy(&bits, &key, 4);
            signature = (signature ^ bits) * 1099511628211ull;
        }
        const bool buildTransmittance = !s_TransmittanceBuilt;
        const bool buildMultiScatter = buildTransmittance || signature != s_LastSignature;

        auto recordLuts = [&](ID3D12GraphicsCommandList* list, bool async) {
            ID3D12DescriptorHeap* heaps[] = {s_Heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(s_Root.Get());
            list->SetComputeRootConstantBufferView(0, s_Constants->GetGPUVirtualAddress() + slot * 256);
            constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            // All LUTs are SIMULTANEOUS_ACCESS: at the start of every ExecuteCommandLists they are
            // in COMMON (decayed), so explicit transitions start from COMMON.
            if (buildTransmittance)
            {
                Barrier(list, s_TransmittanceLut.Get(), D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                BindRegion(list, 0);
                list->SetPipelineState(s_Transmittance.Get());
                list->Dispatch(256 / 8, 64 / 8, 1);
                Barrier(list, s_TransmittanceLut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            }
            if (buildMultiScatter)
            {
                Barrier(list, s_MultiScatterLut.Get(), D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                BindRegion(list, 1);
                list->SetPipelineState(s_MultiScatter.Get());
                list->Dispatch(32 / 8, 32 / 8, 1);
                Barrier(list, s_MultiScatterLut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            }
            D3D12_RESOURCE_BARRIER toWrite[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(s_SkyScatter[slot].Get(), D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                CD3DX12_RESOURCE_BARRIER::Transition(s_SkyTransmission[slot].Get(), D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            list->ResourceBarrier(2, toWrite);
            BindRegion(list, 2 + slot);
            list->SetPipelineState(s_SkyView.Get());
            list->Dispatch((192 + 7) / 8, (108 + 7) / 8, 1);
            // Graphics queue (fallback): make the LUT readable by pixel and compute shaders now.
            // Compute queue: leave it in UAV; it decays to COMMON when the list completes and the
            // graphics queue promotes it implicitly on first read.
            if (!async)
            {
                constexpr auto kShaderRead =
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                D3D12_RESOURCE_BARRIER toRead[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(s_SkyScatter[slot].Get(),
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead),
                    CD3DX12_RESOURCE_BARRIER::Transition(s_SkyTransmission[slot].Get(),
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead)};
                list->ResourceBarrier(2, toRead);
            }
        };
        static const bool graphQueue = ReadEnvironmentFlagOnce("DX12_RENDERGRAPH_ASYNC");
        if (graphQueue)
        {
            if (!s_GraphExecutor)
            {
                s_GraphExecutor = std::make_unique<RenderGraphQueueExecutor>();
                if (!s_GraphExecutor->Initialize(s_Device.Get(), graphicsQueue))
                {
                    s_GraphExecutor.reset();
                    return;
                }
            }
            RenderGraph graph;
            graph.AddPass(
                "Atmosphere LUT compute",
                [](RenderGraph::PassBuilder&) {
                },
                [&](ID3D12GraphicsCommandList* list) {
                    recordLuts(list, true);
                });
            graph.SetLastPassQueue(RenderGraph::Queue::Compute);
            if (!graph.Compile() || !s_GraphExecutor->Execute(graph, slot))
            {
                s_Valid = false;
                return;
            }
            s_TransmittanceBuilt = true;
            s_LastSignature = signature;
            s_Slot = slot;
            s_Valid = true;
            s_LastAsync = true;
            return;
        }
        ID3D12GraphicsCommandList* list = AsyncCompute::Begin(slot);
        const bool async = list != nullptr;
        if (!async)
        {
            list = graphicsList;
        }
        if (!list)
        {
            return;
        }
        recordLuts(list, async);

        if (async)
        {
            if (!AsyncCompute::Submit(graphicsQueue))
            {
                return;
            }
        }
        else
        {
            // Restore the engine heap for the rest of the graphics frame.
            ID3D12DescriptorHeap* engine[] = {s_EngineHeap.Get()};
            graphicsList->SetDescriptorHeaps(1, engine);
        }
        s_TransmittanceBuilt = true;
        s_LastSignature = signature;
        s_Slot = slot;
        s_Valid = true;
        s_LastAsync = async;
    }

    // Descriptor table (t40/t41) for the post process root signature.
    static D3D12_GPU_DESCRIPTOR_HANDLE GetGraphicsSrv()
    {
        if (!s_EngineHeap)
        {
            return D3D12_GPU_DESCRIPTOR_HANDLE{0};
        }
        return EngineGpu(s_Valid ? s_Slot * 2 : kFrames * 2);
    }

    // Writes the current sky view LUT (or null descriptors) into another shader-visible heap.
    static void WriteSrvs(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE scatter,
                          D3D12_CPU_DESCRIPTOR_HANDLE transmission)
    {
        const bool valid = s_Valid && s_Available;
        WriteSrv(valid ? s_SkyScatter[s_Slot].Get() : nullptr, scatter, device);
        WriteSrv(valid ? s_SkyTransmission[s_Slot].Get() : nullptr, transmission, device);
    }

    // Same selection as LightingResources (SkySunDirection): the first SunComponent's physical
    // directional light, otherwise the day/night cycle direction.
    static XMFLOAT3 ResolveSkySunDirection()
    {
        const XMFLOAT4 cycle = DayNightCycle::GetSunDirection();
        XMFLOAT3 result = {cycle.x, cycle.y, cycle.z};
        const auto suns = World::GetView<SunComponent, LightComponent>();
        if (!suns.empty())
        {
            const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(*suns.begin());
            if (light.IsActive && light.Type == LightType::Directional && light.RenderMode == LightRenderMode::Physical)
            {
                result = light.Direction;
            }
        }
        XMVECTOR direction = XMLoadFloat3(&result);
        if (XMVectorGetX(XMVector3LengthSq(direction)) < 1e-8f)
        {
            direction = XMVectorSet(0.0f, 1.0f, -1.0f, 0.0f);
        }
        XMStoreFloat3(&result, XMVector3Normalize(direction));
        return result;
    }

  private:
    static inline std::unique_ptr<RenderGraphQueueExecutor> s_GraphExecutor;
    static constexpr UINT kFrames = RendererState::g_kFRAME_COUNT;
    static constexpr UINT kRegionSize = 5; // t0, t1, u0, u1, u2
    static constexpr UINT kRegionCount = 2 + kFrames;

    static inline Settings s_Settings{};
    static inline bool s_Available = false, s_Valid = false, s_TransmittanceBuilt = false, s_LastAsync = false;
    static inline UINT s_Slot = 0, s_Increment = 0;
    static inline uint64_t s_LastSignature = 0;
    static inline ComPtr<ID3D12Device> s_Device;
    static inline ComPtr<ID3D12DescriptorHeap> s_EngineHeap, s_Heap;
    static inline ComPtr<ID3D12RootSignature> s_Root;
    static inline ComPtr<ID3D12PipelineState> s_Transmittance, s_MultiScatter, s_SkyView;
    static inline ComPtr<ID3D12Resource> s_TransmittanceLut, s_MultiScatterLut, s_Constants;
    static inline ComPtr<ID3D12Resource> s_SkyScatter[kFrames], s_SkyTransmission[kFrames];

    static bool Fail(const char* what)
    {
        WriteGraphicsLog("AtmosphereLut: %s failed, using the analytic sky\n", what);
        ReleaseResources(); // keeps the engine heap so the null descriptors stay bindable
        return false;
    }

    static float CameraAltitude()
    {
        const EntityID camera = Camera::GetCameraEntity();
        if (camera != g_kINVALID_ENTITY && ComponentManager::HasComponent<TransformComponent>(camera))
        {
            return ComponentManager::GetComponentUnchecked<TransformComponent>(camera).Position.y;
        }
        return 0.0f; // LightingResources falls back to (0, 0, -5)
    }

    static bool CreateLut(ComPtr<ID3D12Resource>& texture, UINT width, UINT height, DXGI_FORMAT format,
                          const wchar_t* name)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                                           D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
        if (FAILED(s_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf()))))
        {
            return false;
        }
        texture->SetName(name);
        return true;
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE LocalCpu(UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(s_Heap->GetCPUDescriptorHandleForHeapStart(), index, s_Increment);
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE LocalGpu(UINT index)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(s_Heap->GetGPUDescriptorHandleForHeapStart(), index, s_Increment);
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE EngineCpu(UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(s_EngineHeap->GetCPUDescriptorHandleForHeapStart(),
                                             RendererState::g_kATMOSPHERE_LUT_DESCRIPTOR_START + index, s_Increment);
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE EngineGpu(UINT index)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(s_EngineHeap->GetGPUDescriptorHandleForHeapStart(),
                                             RendererState::g_kATMOSPHERE_LUT_DESCRIPTOR_START + index, s_Increment);
    }

    static void WriteSrv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Device* device = nullptr)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = resource ? resource->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        (device ? device : s_Device.Get())->CreateShaderResourceView(resource, &desc, handle);
    }

    static void WriteUav(ID3D12Resource* resource, DXGI_FORMAT nullFormat, D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = resource ? resource->GetDesc().Format : nullFormat;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        s_Device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
    }

    static void WriteRegion(UINT region, ID3D12Resource* srv0, ID3D12Resource* srv1, ID3D12Resource* uav0,
                            ID3D12Resource* uav1, ID3D12Resource* uavDepth)
    {
        const UINT base = region * kRegionSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        s_Device->CreateShaderResourceView(srv0, &desc, LocalCpu(base + 0));
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        s_Device->CreateShaderResourceView(srv1, &desc, LocalCpu(base + 1));
        WriteUav(uav0, DXGI_FORMAT_R16G16B16A16_FLOAT, LocalCpu(base + 2));
        WriteUav(uav1, DXGI_FORMAT_R16G16B16A16_FLOAT, LocalCpu(base + 3));
        WriteUav(uavDepth, DXGI_FORMAT_R32G32_FLOAT, LocalCpu(base + 4));
    }

    static void BindRegion(ID3D12GraphicsCommandList* list, UINT region)
    {
        list->SetComputeRootDescriptorTable(1, LocalGpu(region * kRegionSize));
        list->SetComputeRootDescriptorTable(2, LocalGpu(region * kRegionSize + 2));
    }

    static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        list->ResourceBarrier(1, &barrier);
    }
};
