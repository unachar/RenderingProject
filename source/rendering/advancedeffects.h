#pragma once
// =============================================================================================
// 高度な画面効果（AdvancedEffects）
//   ・フロクセル（3Dグリッド）方式のボリュームフォグ（大気 + 局所フォグ + ライトシャフト）
//   ・ヒストグラム方式の自動露出（Eye Adaptation）
//   ・モーションブラー
//   ・被写界深度（DOF）
//   線形HDRシーン（ブルーム・トーンマップ前）に対して実行する。
//   ヘッダーのみで完結（.vcxproj への追加不要）。シェーダーは実行時に DXC でコンパイルする。
// =============================================================================================
#include "pch.h"
#include "rendergraph.h"
#include "envflags.h"
#include "rendererstate.h"
#include "renderprofiler.h"
#include "raytracing.h"
#include "camera.h"
#include "world.h"
#include "atmosphere.h"
#include "daynightcycle.h"
#include "localheightfog.h"
#include "graphicslog.h"
#include "graphicsdevice.h"
#include "renderquality.h"
#include "componentmanager.h"
#include "light.h"
#include "megalights.h"
#include "lightingresources.h"
#include "texturemanager.h"
#include <algorithm>
#include <cmath>

class AdvancedEffects final
{
  public:
    struct Settings
    {
        // ボリュームフォグ（有効時は従来のピクセル単位レイマーチ大気パスを置き換える）
        bool FroxelFog = false;
        int FroxelSliceCount = 64; // 奥行き方向の分割数
        int FroxelTileSize = 8;    // 画面方向のタイルサイズ（ピクセル）
        float FogNear = 0.5f;
        float FogFar = 100.0f;         // 従来の大気レイマーチ距離と同じ
        float FogHistoryWeight = 0.9f; // 時間方向の蓄積
        float FogScattering = 1.0f;    // 散乱強度（大気設定の光芒強度に乗算）
        float FogAmbient = 1.0f;       // 多重散乱近似（空の環境光）
        // 自動露出
        bool AutoExposure = false;
        float ExposureCompensation = 0.0f; // 露出値（EV）
        float ExposureMinEv = -6.0f;
        float ExposureMaxEv = 8.0f;
        float ExposureSpeed = 2.5f;
        float ExposureLowPercent = 0.50f;
        float ExposureHighPercent = 0.95f;
        // モーションブラー
        bool MotionBlur = false;
        bool HalfResolutionLens = false;
        float MotionBlurShutter = 0.5f; // シャッター角 180度 = 0.5
        float MotionBlurMaxPixels = 32.0f;
        int MotionBlurSamples = 8;
        // 被写界深度
        bool DepthOfField = false;
        float FocusDistance = 5.0f;
        float FocusRange = 2.0f;
        float MaxBlurPixels = 12.0f;
    };

    static Settings& GetSettings()
    {
        return s_Settings;
    }

    static bool IsAvailable()
    {
        return s_Available;
    }

    // 有効な間は、デュアルフィルターのブルームが従来のブルーム後処理を置き換える。
    static bool IsDualBloomActive()
    {
        return s_Available && s_BloomAvailable && RenderQuality::Get().DualBloom;
    }

    // 従来のパスと同様、カメラの後処理選択（PostProcessType::BLOOM）に従う。
    static void SetBloomRequested(bool requested)
    {
        s_BloomRequested = requested;
    }

    static bool IsFroxelFogActive()
    {
        return s_Available && s_Settings.FroxelFog;
    }

    static float GetLastExposure()
    {
        return s_LastExposureShown;
    }

    static bool IsHalfResolutionLensActive()
    {
        return s_HalfActive;
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
        for (auto& revision : s_IesDescriptorRevisions)
        {
            revision = 0;
        }

        CD3DX12_DESCRIPTOR_RANGE ranges[5];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0);
        // 5〜8：MegaLights（M6）のグリッド定数 / ライト / セル / セル内ライト（フロクセルのローカルライト）
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 11);
        ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 11);
        ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, RendererState::g_kMAX_SRVS, 0, 5);
        CD3DX12_ROOT_PARAMETER parameters[13];
        parameters[0].InitAsConstantBufferView(0);
        parameters[1].InitAsDescriptorTable(1, &ranges[0]);
        parameters[2].InitAsDescriptorTable(1, &ranges[1]);
        parameters[3].InitAsShaderResourceView(10);
        parameters[4].InitAsShaderResourceView(11);
        parameters[5].InitAsConstantBufferView(1);
        parameters[6].InitAsShaderResourceView(12);
        parameters[7].InitAsShaderResourceView(13);
        parameters[8].InitAsShaderResourceView(14);
        parameters[9].InitAsDescriptorTable(1, &ranges[2]);
        parameters[10].InitAsDescriptorTable(1, &ranges[3]);
        parameters[11].InitAsConstants(4, 2);
        parameters[12].InitAsDescriptorTable(1, &ranges[4]);
        CD3DX12_STATIC_SAMPLER_DESC samplers[2] = {
            CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
            CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP)};
        const CD3DX12_ROOT_SIGNATURE_DESC root(_countof(parameters), parameters, 2, samplers);
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(
                device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s_Root))))
        {
            WriteGraphicsLog("AdvancedEffects: root signature failed\n");
            return false;
        }

        const struct
        {
            const char* Entry;
            ComPtr<ID3D12PipelineState>* Pipeline;
        } entries[] = {{"FogInject", &s_FogInject},
                       {"FogIntegrate", &s_FogIntegrate},
                       {"Composite", &s_Composite},
                       {"HistogramClear", &s_HistogramClear},
                       {"HistogramBuild", &s_HistogramBuild},
                       {"ExposureAdapt", &s_ExposureAdapt},
                       {"Resolve", &s_Resolve},
                       {"DepthOfField", &s_DepthOfField}};

        for (const auto& entry : entries)
        {
            blob.Reset();
            errors.Reset();
            if (FAILED(D3DCompileFromFile(L"shader/hlsl/AdvancedEffectsCS.hlsl", nullptr,
                                          D3D_COMPILE_STANDARD_FILE_INCLUDE, entry.Entry, "cs_6_5",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
            {
                WriteGraphicsLog("AdvancedEffects: %s compile failed: %s\n", entry.Entry,
                                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                Shutdown();
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = s_Root.Get();
            desc.CS = CD3DX12_SHADER_BYTECODE(blob.Get());
            if (FAILED(
                    device->CreateComputePipelineState(&desc, IID_PPV_ARGS(entry.Pipeline->ReleaseAndGetAddressOf()))))
            {
                Shutdown();
                return false;
            }
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heap.NumDescriptors = kDescriptorsPerFrame * kFrames;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s_Heap))) ||
            !CreateBuffer(s_Constants, kConstantStride * kFrames, D3D12_HEAP_TYPE_UPLOAD,
                          L"AdvancedEffects Constants") ||
            !CreateBuffer(s_Lights, kLightStride * kFrames, D3D12_HEAP_TYPE_UPLOAD, L"AdvancedEffects Fog Lights") ||
            !CreateBuffer(s_Histogram, 64 * sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT, L"AdvancedEffects Histogram") ||
            !CreateBuffer(s_Exposure, 4 * sizeof(float), D3D12_HEAP_TYPE_DEFAULT, L"AdvancedEffects Exposure"))
        {
            Shutdown();
            return false;
        }
        s_Available = true;
        s_BloomAvailable = InitializeBloom(device);
        WriteGraphicsLog("AdvancedEffects: initialized (froxel fog, auto exposure, motion blur, DOF, bloom=%d)\n",
                         s_BloomAvailable ? 1 : 0);
        return true;
    }

    static void Shutdown()
    {
        s_HalfLens[0].Reset();
        s_HalfLens[1].Reset();
        s_ResolveHalf.Reset();
        s_DofHalf.Reset();
        s_UpsampleLens.Reset();
        s_HalfWidth = s_HalfHeight = 0;
        s_HalfFormat = DXGI_FORMAT_UNKNOWN;
        s_HalfTried = s_HalfActive = false;
        s_Transient.Reset();
        s_AliasActive = false;
        s_Available = false;
        s_Root.Reset();
        s_Heap.Reset();
        s_FogInject.Reset();
        s_FogIntegrate.Reset();
        s_Composite.Reset();
        s_HistogramClear.Reset();
        s_HistogramBuild.Reset();
        s_ExposureAdapt.Reset();
        s_Resolve.Reset();
        s_DepthOfField.Reset();
        s_Constants.Reset();
        s_Lights.Reset();
        s_Histogram.Reset();
        s_Exposure.Reset();
        s_Froxels[0].Reset();
        s_Froxels[1].Reset();
        s_Integrated.Reset();
        s_PostA.Reset();
        s_PostB.Reset();
        s_BloomAvailable = false;
        s_BloomRoot.Reset();
        s_BloomDownPso.Reset();
        s_BloomUpPso.Reset();
        s_BloomCombinePso.Reset();
        s_BloomHeap.Reset();
        s_BloomDown.Reset();
        s_BloomUp.Reset();
        s_BloomLevels = 0;
        s_Device.Reset();
        s_EngineHeap.Reset();
        s_Width = s_Height = 0;
        s_FroxelExtent = {};
        s_FogHistoryValid = false;
        s_ExposureReset = true;
    }

    // シーンは線形 HDR。開始時と終了時の状態は PIXEL_SHADER_RESOURCE。
    // 深度と速度は GBuffer のターゲット。開始時と終了時は PIXEL_SHADER_RESOURCE。
    static void Execute(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene, ID3D12Resource* depth,
                        ID3D12Resource* velocity, UINT frameIndex)
    {
        const Settings& settings = s_Settings;
        const bool fog = settings.FroxelFog;
        const bool exposure = settings.AutoExposure;
        const bool motionBlur = settings.MotionBlur && velocity;
        const bool dof = settings.DepthOfField;
        if (!fog)
        {
            s_FogHistoryValid = false;
        }
        if (!exposure)
        {
            s_ExposureReset = true;
        }
        const auto& quality = RenderQuality::Get();
        const bool weather = quality.WeatherEnabled;
        const bool dualBloom = quality.DualBloom && s_BloomRequested;
        const bool bloomPass =
            s_BloomAvailable && (dualBloom || quality.LensFlare ||
                                 (weather && (quality.RainIntensity > 0.001f || quality.SnowFall > 0.001f)));
        if (!s_Available || !commands || !scene || !depth || !(fog || exposure || motionBlur || dof || bloomPass))
        {
            return;
        }
        RenderProfiler::ScopedEvent profile("Advanced Effects (Fog / Exposure / MB / DOF / Bloom)", commands);
        const auto sceneDesc = scene->GetDesc();
        if (!EnsureTargets(static_cast<UINT>(sceneDesc.Width), sceneDesc.Height, sceneDesc.Format))
        {
            return;
        }

        const bool halfLens = s_Settings.HalfResolutionLens && (motionBlur || dof) && EnsureHalfLens();
        s_HalfActive = halfLens;
        const UINT slot = frameIndex % kFrames;
        ++s_Frame;
        const UINT current = s_Frame & 1u;
        // M6：グリッドがフォグ用に構築されていれば、グリッドのライトはフォグでも MegaLights が受け持つ
        s_MegaLightsFog = fog && MegaLights::IsVolumetricGridActive();
        const UINT lightCount = UploadLights(slot);
        UploadConstants(slot, lightCount, fog, exposure, motionBlur, dof);
        WriteDescriptors(slot, scene, depth, velocity ? velocity : depth, current);

        ID3D12DescriptorHeap* heaps[] = {s_Heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(s_Root.Get());
        commands->SetComputeRootConstantBufferView(0, s_Constants->GetGPUVirtualAddress() + slot * kConstantStride);
        commands->SetComputeRootDescriptorTable(1, Gpu(slot * kDescriptorsPerFrame));
        commands->SetComputeRootDescriptorTable(2, Gpu(slot * kDescriptorsPerFrame + kSrvCount));
        s_IesDescriptorRevisions[slot] = TextureManager::WriteIesProfileViews(
            Cpu(slot * kDescriptorsPerFrame + kIesOffset), s_IesDescriptorRevisions[slot]);
        commands->SetComputeRootDescriptorTable(12, Gpu(slot * kDescriptorsPerFrame + kIesOffset));
        const D3D12_GPU_VIRTUAL_ADDRESS lights = s_Lights->GetGPUVirtualAddress() + slot * kLightStride;
        const D3D12_GPU_VIRTUAL_ADDRESS tlas = Raytracing::GetSceneAddress();
        // FogMieColor.w（影を利用可能）が立っている場合だけ TLAS スロットを読む。
        commands->SetComputeRootShaderResourceView(3, tlas ? tlas : lights);
        commands->SetComputeRootShaderResourceView(4, lights);
        MegaLights::BindFogCompute(commands, 5, 6, 7, 8, s_Constants->GetGPUVirtualAddress() + slot * kConstantStride,
                                   lights);

        constexpr D3D12_RESOURCE_STATES kRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        vector<D3D12_RESOURCE_BARRIER> begin;
        for (ID3D12Resource* input : {scene, depth, velocity})
        {
            if (input)
            {
                begin.push_back(
                    CD3DX12_RESOURCE_BARRIER::Transition(input, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kRead));
            }
        }
        commands->ResourceBarrier(static_cast<UINT>(begin.size()), begin.data());

        const UINT groupsX = (s_Width + 7) / 8, groupsY = (s_Height + 7) / 8;
        if (fog)
        {
            ID3D12Resource* froxel = s_Froxels[current].Get();
            Barrier(commands, froxel, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetPipelineState(s_FogInject.Get());
            commands->Dispatch((s_FroxelExtent.x + 7) / 8, (s_FroxelExtent.y + 7) / 8, s_FroxelExtent.z);
            Barrier(commands, froxel, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            Barrier(commands, s_Integrated.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetPipelineState(s_FogIntegrate.Get());
            commands->Dispatch((s_FroxelExtent.x + 7) / 8, (s_FroxelExtent.y + 7) / 8, 1);
            Barrier(commands, s_Integrated.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            s_FogHistoryValid = true;
        }
        Barrier(commands, s_PostA.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->SetPipelineState(s_Composite.Get());
        commands->Dispatch(groupsX, groupsY, 1);
        Barrier(commands, s_PostA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        if (exposure)
        {
            if (s_AliasActive)
            {
                s_Transient.Activate(0, commands);
            }
            commands->SetPipelineState(s_HistogramClear.Get());
            commands->Dispatch(1, 1, 1);
            UavBarrier(commands, s_Histogram.Get());
            commands->SetPipelineState(s_HistogramBuild.Get());
            commands->Dispatch((s_Width / 2 + 15) / 16, (s_Height / 2 + 15) / 16, 1);
            UavBarrier(commands, s_Histogram.Get());
            commands->SetPipelineState(s_ExposureAdapt.Get());
            commands->Dispatch(1, 1, 1);
            UavBarrier(commands, s_Exposure.Get());
            s_ExposureReset = false;
        }
        if (s_AliasActive)
        {
            s_Transient.Activate(1, commands);
        }
        ID3D12Resource* result = s_PostB.Get();
        if (halfLens)
        {
            RenderProfiler::ScopedEvent profile("Half resolution MB / DOF", commands);
            const UINT parameters[] = {s_HalfWidth, s_HalfHeight, dof ? 1u : 0u, 0};
            commands->SetComputeRoot32BitConstants(11, 4, parameters, 0);
            commands->SetComputeRootDescriptorTable(9, Gpu(slot * kDescriptorsPerFrame + kHalfSrvOffset));
            commands->SetComputeRootDescriptorTable(10, Gpu(slot * kDescriptorsPerFrame + kHalfUavOffset));
            Barrier(commands, s_HalfLens[0].Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetPipelineState(s_ResolveHalf.Get());
            commands->Dispatch((s_HalfWidth + 7) / 8, (s_HalfHeight + 7) / 8, 1);
            Barrier(commands, s_HalfLens[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            if (dof)
            {
                Barrier(commands, s_HalfLens[1].Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commands->SetPipelineState(s_DofHalf.Get());
                commands->Dispatch((s_HalfWidth + 7) / 8, (s_HalfHeight + 7) / 8, 1);
                Barrier(commands, s_HalfLens[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            }
            Barrier(commands, s_PostB.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetPipelineState(s_UpsampleLens.Get());
            commands->Dispatch(groupsX, groupsY, 1);
            Barrier(commands, s_PostB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        }
        else
        {
            Barrier(commands, s_PostB.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetPipelineState(s_Resolve.Get());
            commands->Dispatch(groupsX, groupsY, 1);
            Barrier(commands, s_PostB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
            result = s_PostB.Get();
            if (dof)
            {
                Barrier(commands, s_PostA.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commands->SetPipelineState(s_DepthOfField.Get());
                commands->Dispatch(groupsX, groupsY, 1);
                Barrier(commands, s_PostA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
                result = s_PostA.Get();
            }
        }

        if (bloomPass && EnsureBloomTargets())
        {
            result = ExecuteBloom(commands, slot, result, depth, dualBloom);
        }

        // シーンターゲットには UAV フラグがないため、結果をコピーして戻す。
        vector<D3D12_RESOURCE_BARRIER> copy = {
            CD3DX12_RESOURCE_BARRIER::Transition(result, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(scene, kRead, D3D12_RESOURCE_STATE_COPY_DEST)};
        commands->ResourceBarrier(2, copy.data());
        commands->CopyResource(scene, result);
        vector<D3D12_RESOURCE_BARRIER> end = {
            CD3DX12_RESOURCE_BARRIER::Transition(result, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead),
            CD3DX12_RESOURCE_BARRIER::Transition(scene, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        for (ID3D12Resource* input : {depth, velocity})
        {
            if (input)
            {
                end.push_back(
                    CD3DX12_RESOURCE_BARRIER::Transition(input, kRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
            }
        }
        commands->ResourceBarrier(static_cast<UINT>(end.size()), end.data());

        ID3D12DescriptorHeap* engine[] = {s_EngineHeap.Get()};
        commands->SetDescriptorHeaps(1, engine);
    }

  private:
    static constexpr UINT kFrames = RendererState::g_kFRAME_COUNT;
    static constexpr UINT kSrvCount = 10; // t0..t9（t8、t9 は予約済み）
    static constexpr UINT kUavCount = 6;  // 書き込み先レジスター：u0～u5
    static constexpr UINT kHalfSrvOffset = kSrvCount + kUavCount, kHalfUavOffset = kHalfSrvOffset + 2;
    static constexpr UINT kIesOffset = kHalfUavOffset + 2;
    inline static uint64_t s_IesDescriptorRevisions[kFrames]{};
    static constexpr UINT kDescriptorsPerFrame = kIesOffset + RendererState::g_kMAX_SRVS;
    static constexpr UINT kMaxLights = 32;
    static constexpr UINT kMaxLocalFog = 16;

    struct alignas(16) FogLight
    {
        XMFLOAT4 PositionType, DirectionRange, Color, Extras, IesProfile;
    };

    struct alignas(16) Constants
    {
        XMFLOAT4X4 InvViewProjection, PrevViewProjection;
        XMFLOAT4 CameraPosition, PrevCameraPosition;
        XMUINT4 Extent, FroxelExtent;
        XMFLOAT4 FogDepth, FogMedium0, FogMedium1, FogRayleighColor, FogMieColor, FogAmbientColor;
        XMFLOAT4 ExposureParams, ExposureLimits, MotionBlurParams, DofParams;
        XMUINT4 Features;
        XMFLOAT4 LocalFogData0[kMaxLocalFog], LocalFogData1[kMaxLocalFog], LocalFogColor[kMaxLocalFog];
        XMUINT4 MegaLightsFog; // x: MegaLights のグリッドを使う, y: フロクセルあたりのサンプル数
    };

    static constexpr UINT kConstantStride = (sizeof(Constants) + 255u) & ~255u;
    static constexpr UINT kLightStride = (sizeof(FogLight) * kMaxLights + 255u) & ~255u;

    static inline Settings s_Settings{};
    static inline bool s_Available = false;
    static inline ComPtr<ID3D12Device> s_Device;
    static inline ComPtr<ID3D12DescriptorHeap> s_EngineHeap, s_Heap;
    static inline ComPtr<ID3D12RootSignature> s_Root;
    static inline ComPtr<ID3D12PipelineState> s_FogInject, s_FogIntegrate, s_Composite, s_HistogramClear,
        s_HistogramBuild, s_ExposureAdapt, s_Resolve, s_DepthOfField;
    static inline ComPtr<ID3D12Resource> s_Constants, s_Lights, s_Histogram, s_Exposure;
    static inline ComPtr<ID3D12Resource> s_Froxels[2], s_Integrated, s_PostA, s_PostB;
    static inline UINT s_Increment = 0, s_Width = 0, s_Height = 0, s_Frame = 0;
    static inline DXGI_FORMAT s_Format = DXGI_FORMAT_UNKNOWN;
    static inline XMUINT3 s_FroxelExtent{};
    static inline bool s_MegaLightsFog = false;
    static inline bool s_FogHistoryValid = false, s_ExposureReset = true;
    static inline XMFLOAT4X4 s_PrevViewProjection{};
    static inline XMFLOAT3 s_PrevCamera{};
    static inline float s_LastExposureShown = 1.0f;
    // --- デュアルフィルターのブルーム / レンズフレア / 天候の重ね合わせ ---
    static constexpr UINT kMaxBloomLevels = 8;
    static constexpr UINT kBloomTablesPerFrame = kMaxBloomLevels * 2 + 1;
    static constexpr UINT kBloomTableSize = 5; // 読み取り元レジスター：t0～t3、書き込み先レジスター：u0
    static inline bool s_BloomAvailable = false;
    static inline bool s_BloomRequested = false;
    static inline ComPtr<ID3D12RootSignature> s_BloomRoot;
    static inline ComPtr<ID3D12PipelineState> s_BloomDownPso, s_BloomUpPso, s_BloomCombinePso;
    static inline ComPtr<ID3D12DescriptorHeap> s_BloomHeap;
    static inline ComPtr<ID3D12Resource> s_BloomDown, s_BloomUp;
    static inline UINT s_BloomLevels = 0, s_BloomWidth = 0, s_BloomHeight = 0;

    struct BloomConstants
    {
        UINT DestinationWidth, DestinationHeight, SourceWidth, SourceHeight;
        float Threshold, Radius, Intensity, Normalization;
        float FlareIntensity, FlareHalo, RainIntensity, SnowIntensity;
        float Time, WindX;
        UINT FirstLevel, Flags;
    };

    static_assert(sizeof(BloomConstants) == 64);

    static bool InitializeBloom(ID3D12Device* device)
    {
        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 4);
        CD3DX12_ROOT_PARAMETER parameters[2];
        parameters[0].InitAsConstants(sizeof(BloomConstants) / 4, 0);
        parameters[1].InitAsDescriptorTable(2, ranges);
        const CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                  D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        const CD3DX12_ROOT_SIGNATURE_DESC root(2, parameters, 1, &sampler);
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&s_BloomRoot))))
        {
            return false;
        }

        const struct
        {
            const char* Entry;
            ComPtr<ID3D12PipelineState>* Pipeline;
        } entries[] = {{"BloomDownsample", &s_BloomDownPso},
                       {"BloomUpsample", &s_BloomUpPso},
                       {"BloomCombine", &s_BloomCombinePso}};

        for (const auto& entry : entries)
        {
            blob.Reset();
            errors.Reset();
            if (FAILED(D3DCompileFromFile(L"shader/hlsl/BloomCS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                          entry.Entry, "cs_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
            {
                WriteGraphicsLog("Bloom: %s compile failed: %s\n", entry.Entry,
                                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = s_BloomRoot.Get();
            desc.CS = CD3DX12_SHADER_BYTECODE(blob.Get());
            if (FAILED(
                    device->CreateComputePipelineState(&desc, IID_PPV_ARGS(entry.Pipeline->ReleaseAndGetAddressOf()))))
            {
                return false;
            }
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heap.NumDescriptors = kBloomTablesPerFrame * kBloomTableSize * kFrames;
        return SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s_BloomHeap)));
    }

    static bool EnsureBloomTargets()
    {
        const UINT width = (std::max)(1u, s_Width / 2), height = (std::max)(1u, s_Height / 2);
        UINT levels =
            static_cast<UINT>(std::clamp(RenderQuality::Get().BloomLevels, 2, static_cast<int>(kMaxBloomLevels)));
        while (levels > 1 && ((width >> (levels - 1)) < 4 || (height >> (levels - 1)) < 4))
        {
            --levels;
        }
        if (s_BloomDown && s_BloomWidth == width && s_BloomHeight == height && s_BloomLevels == levels)
        {
            return true;
        }
        if (s_BloomDown && !GraphicsDevice::WaitForGpuIdle())
        {
            return false;
        }
        const auto desc =
            CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, static_cast<UINT16>(levels));
        if (!CreateTexture(s_BloomDown, desc, L"Bloom Downsample Chain") ||
            !CreateTexture(s_BloomUp, desc, L"Bloom Upsample Chain"))
        {
            s_BloomDown.Reset();
            s_BloomUp.Reset();
            return false;
        }
        s_BloomWidth = width;
        s_BloomHeight = height;
        s_BloomLevels = levels;
        return true;
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE BloomCpu(UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(s_BloomHeap->GetCPUDescriptorHandleForHeapStart(), index, s_Increment);
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE BloomGpu(UINT index)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(s_BloomHeap->GetGPUDescriptorHandleForHeapStart(), index, s_Increment);
    }

    // srvs は最大4個（リソース、ミップ。nullptr は null 記述子）。GPU テーブルのハンドルを返す。
    static D3D12_GPU_DESCRIPTOR_HANDLE WriteBloomTable(UINT table,
                                                       std::initializer_list<std::pair<ID3D12Resource*, UINT>> srvs,
                                                       ID3D12Resource* uav, UINT uavMip)
    {
        const UINT base = table * kBloomTableSize;
        UINT index = 0;
        for (const auto& srv : srvs)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = srv.first ? srv.first->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            if (desc.Format == DXGI_FORMAT_R32_TYPELESS)
            {
                desc.Format = DXGI_FORMAT_R32_FLOAT;
            }
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MostDetailedMip = srv.first ? srv.second : 0;
            desc.Texture2D.MipLevels = 1;
            s_Device->CreateShaderResourceView(srv.first, &desc, BloomCpu(base + index));
            ++index;
        }
        for (; index < 4; ++index)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = index == 3 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels = 1;
            s_Device->CreateShaderResourceView(nullptr, &desc, BloomCpu(base + index));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = uav->GetDesc().Format;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = uavMip;
        s_Device->CreateUnorderedAccessView(uav, nullptr, &desc, BloomCpu(base + 4));
        return BloomGpu(base);
    }

    // ミップチェーンのブルーム、レンズフレア、雨雪を合成し、結果のテクスチャを返す。
    // 結果は入力に使わなかった Post テクスチャに置き、全テクスチャをシェーダー読み取り状態にする。
    static ID3D12Resource* ExecuteBloom(ID3D12GraphicsCommandList* commands, UINT slot, ID3D12Resource* input,
                                        ID3D12Resource* depth, bool dualBloom)
    {
        constexpr D3D12_RESOURCE_STATES kRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto& quality = RenderQuality::Get();
        ID3D12Resource* output = input == s_PostA.Get() ? s_PostB.Get() : s_PostA.Get();
        const UINT tableBase = slot * kBloomTablesPerFrame;
        ID3D12DescriptorHeap* heaps[] = {s_BloomHeap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(s_BloomRoot.Get());
        BloomConstants constants{};
        constants.Threshold = (std::max)(0.0f, quality.BloomThreshold);
        constants.Radius = std::clamp(quality.BloomRadius, 0.0f, 1.0f);
        const auto mipSize = [](UINT size, UINT mip) {
            return (std::max)(1u, size >> mip);
        };
        // 縮小ミップチェーン。
        commands->SetPipelineState(s_BloomDownPso.Get());
        Barrier(commands, s_BloomDown.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (UINT level = 0; level < s_BloomLevels; ++level)
        {
            // ミップごとに遷移する。入力ミップは読み取り状態、出力ミップは UAV のままにする。
            const bool first = level == 0;
            if (!first)
            {
                const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
                    s_BloomDown.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead, level - 1);
                commands->ResourceBarrier(1, &toRead);
            }
            constants.DestinationWidth = mipSize(s_BloomWidth, level);
            constants.DestinationHeight = mipSize(s_BloomHeight, level);
            constants.SourceWidth = first ? s_Width : mipSize(s_BloomWidth, level - 1);
            constants.SourceHeight = first ? s_Height : mipSize(s_BloomHeight, level - 1);
            constants.FirstLevel = first ? 1u : 0u;
            commands->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            commands->SetComputeRootDescriptorTable(
                1, WriteBloomTable(tableBase + level, {{first ? input : s_BloomDown.Get(), first ? 0u : level - 1}},
                                   s_BloomDown.Get(), level));
            commands->Dispatch((constants.DestinationWidth + 7) / 8, (constants.DestinationHeight + 7) / 8, 1);
        }
        const auto lastToRead = CD3DX12_RESOURCE_BARRIER::Transition(
            s_BloomDown.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead, s_BloomLevels - 1);
        commands->ResourceBarrier(1, &lastToRead);
        // 拡大ミップチェーン：up[i] = down[i] + tent(up[i + 1]) * radius（最粗ミップは down を使用）。
        commands->SetPipelineState(s_BloomUpPso.Get());
        Barrier(commands, s_BloomUp.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        float normalization = 1.0f, weight = 1.0f;
        for (int level = static_cast<int>(s_BloomLevels) - 2; level >= 0; --level)
        {
            const bool coarsest = level == static_cast<int>(s_BloomLevels) - 2;
            if (!coarsest)
            {
                const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
                    s_BloomUp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead, level + 1);
                commands->ResourceBarrier(1, &toRead);
            }
            constants.DestinationWidth = mipSize(s_BloomWidth, level);
            constants.DestinationHeight = mipSize(s_BloomHeight, level);
            constants.SourceWidth = mipSize(s_BloomWidth, level + 1);
            constants.SourceHeight = mipSize(s_BloomHeight, level + 1);
            commands->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            commands->SetComputeRootDescriptorTable(
                1, WriteBloomTable(tableBase + kMaxBloomLevels + level,
                                   {{s_BloomDown.Get(), static_cast<UINT>(level)},
                                    {coarsest ? s_BloomDown.Get() : s_BloomUp.Get(), static_cast<UINT>(level + 1)}},
                                   s_BloomUp.Get(), level));
            commands->Dispatch((constants.DestinationWidth + 7) / 8, (constants.DestinationHeight + 7) / 8, 1);
            weight *= constants.Radius;
            normalization += weight;
        }
        // 未書き込みの最粗ミップ以外は読み取り状態になっているため、
        // リソース全体を再び読み取り可能な状態にそろえる。
        for (UINT level = 0; level < s_BloomLevels; ++level)
        {
            if (level == 0 || level == s_BloomLevels - 1)
            {
                // ミップ0はまだ UAV。最粗ミップは未書き込みだが UAV 状態にある。
                const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
                    s_BloomUp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead, level);
                commands->ResourceBarrier(1, &toRead);
            }
        }
        // 合成。
        const bool weather = quality.WeatherEnabled;
        constants.DestinationWidth = s_Width;
        constants.DestinationHeight = s_Height;
        constants.SourceWidth = s_BloomWidth;
        constants.SourceHeight = s_BloomHeight;
        constants.Intensity = dualBloom ? std::clamp(quality.BloomIntensity, 0.0f, 1.0f) : 0.0f;
        constants.Normalization = 1.0f / normalization;
        constants.FlareIntensity = (std::max)(0.0f, quality.LensFlareIntensity);
        constants.FlareHalo = (std::max)(0.0f, quality.LensFlareHalo);
        constants.RainIntensity = weather ? std::clamp(quality.RainIntensity, 0.0f, 1.0f) : 0.0f;
        constants.SnowIntensity = weather ? std::clamp(quality.SnowFall, 0.0f, 1.0f) : 0.0f;
        static float s_Time = 0.0f;
        s_Time = fmodf(s_Time + std::clamp(World::GetRawDeltaTime(), 0.0f, 0.25f), 3600.0f);
        constants.Time = s_Time;
        constants.WindX = quality.WindX;
        constants.FirstLevel = 0;
        constants.Flags = (dualBloom ? 1u : 0u) | (quality.LensFlare ? 2u : 0u) |
                          (constants.RainIntensity > 0.001f ? 4u : 0u) | (constants.SnowIntensity > 0.001f ? 8u : 0u);
        commands->SetPipelineState(s_BloomCombinePso.Get());
        commands->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
        const UINT flareMip = (std::min)(2u, s_BloomLevels - 1);
        commands->SetComputeRootDescriptorTable(
            1, WriteBloomTable(tableBase + 2 * kMaxBloomLevels,
                               {{input, 0u}, {s_BloomUp.Get(), 0u}, {s_BloomDown.Get(), flareMip}, {depth, 0u}}, output,
                               0));
        Barrier(commands, output, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->Dispatch((s_Width + 7) / 8, (s_Height + 7) / 8, 1);
        Barrier(commands, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        return output;
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(s_Heap->GetCPUDescriptorHandleForHeapStart(), index, s_Increment);
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(s_Heap->GetGPUDescriptorHandleForHeapStart(), index, s_Increment);
    }

    static void Barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        commands->ResourceBarrier(1, &barrier);
    }

    static void UavBarrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(resource);
        commands->ResourceBarrier(1, &barrier);
    }

    static bool CreateBuffer(ComPtr<ID3D12Resource>& buffer, UINT64 bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(type);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, type == D3D12_HEAP_TYPE_DEFAULT
                                                                   ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                                   : D3D12_RESOURCE_FLAG_NONE);
        const auto state =
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (FAILED(s_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                     IID_PPV_ARGS(&buffer))))
        {
            return false;
        }
        buffer->SetName(name);
        return true;
    }

    static bool CreateTexture(ComPtr<ID3D12Resource>& texture, D3D12_RESOURCE_DESC desc, const wchar_t* name)
    {
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(s_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                     nullptr, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf()))))
        {
            return false;
        }
        texture->SetName(name);
        return true;
    }

    static inline TransientArena s_Transient;
    static inline bool s_AliasActive = false;
    static inline ComPtr<ID3D12Resource> s_HalfLens[2];
    static inline ComPtr<ID3D12PipelineState> s_ResolveHalf, s_DofHalf, s_UpsampleLens;
    static inline UINT s_HalfWidth = 0, s_HalfHeight = 0;
    static inline DXGI_FORMAT s_HalfFormat = DXGI_FORMAT_UNKNOWN;
    static inline bool s_HalfTried = false, s_HalfActive = false;

    static bool EnsureHalfLens()
    {
        if (!s_HalfTried)
        {
            s_HalfTried = true;
            auto compile = [](const char* entry, ComPtr<ID3D12PipelineState>& pso) {
                ComPtr<ID3DBlob> code, errors;
                if (FAILED(D3DCompileFromFile(L"shader/hlsl/AdvancedEffectsCS.hlsl", nullptr,
                                              D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, "cs_6_5",
                                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
                {
                    WriteGraphicsLog("Half lens %s: %s\n", entry,
                                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                    return false;
                }
                D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
                d.pRootSignature = s_Root.Get();
                d.CS = {code->GetBufferPointer(), code->GetBufferSize()};
                return SUCCEEDED(s_Device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)));
            };
            if (!compile("ResolveHalf", s_ResolveHalf) || !compile("DofHalf", s_DofHalf) ||
                !compile("UpsampleLens", s_UpsampleLens))
            {
                s_ResolveHalf.Reset();
                s_DofHalf.Reset();
                s_UpsampleLens.Reset();
            }
        }
        if (!s_ResolveHalf || !s_DofHalf || !s_UpsampleLens)
        {
            return false;
        }
        const UINT width = (s_Width + 1) / 2, height = (s_Height + 1) / 2;
        if (s_HalfLens[0] && s_HalfLens[1] && s_HalfWidth == width && s_HalfHeight == height &&
            s_HalfFormat == s_Format)
        {
            return true;
        }
        if (!GraphicsDevice::WaitForGpuIdle())
        {
            return false;
        }
        ComPtr<ID3D12Resource> a, b;
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(s_Format, width, height, 1, 1);
        if (!CreateTexture(a, desc, L"Half Motion Blur") || !CreateTexture(b, desc, L"Half DOF"))
        {
            return false;
        }
        s_HalfLens[0] = a;
        s_HalfLens[1] = b;
        s_HalfWidth = width;
        s_HalfHeight = height;
        s_HalfFormat = s_Format;
        return true;
    }

    static bool EnsureTargets(UINT width, UINT height, DXGI_FORMAT format)
    {
        const UINT tile = static_cast<UINT>(std::clamp(s_Settings.FroxelTileSize, 4, 32));
        const XMUINT3 froxels = {(std::max)(1u, (width + tile - 1) / tile), (std::max)(1u, (height + tile - 1) / tile),
                                 static_cast<UINT>(std::clamp(s_Settings.FroxelSliceCount, 16, 128))};
        const bool sceneChanged = width != s_Width || height != s_Height || format != s_Format;
        const bool froxelChanged =
            froxels.x != s_FroxelExtent.x || froxels.y != s_FroxelExtent.y || froxels.z != s_FroxelExtent.z;
        if (!sceneChanged && !froxelChanged)
        {
            return true;
        }
        // 先行フレームが古いテクスチャを参照している可能性がある。
        if (!GraphicsDevice::WaitForGpuIdle())
        {
            return false;
        }
        if (sceneChanged)
        {
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1);
            if (!CreateTexture(s_PostA, desc, L"AdvancedEffects Post A") ||
                !CreateTexture(s_PostB, desc, L"AdvancedEffects Post B"))
            {
                return false;
            }
            static const bool alias = ReadEnvironmentFlagOnce("DX12_TRANSIENT_ALIAS");
            s_AliasActive = false;
            s_Transient.Reset();
            if (alias)
            {
                RenderGraph graph;
                auto histogramDesc =
                    CD3DX12_RESOURCE_DESC::Buffer(64 * sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                auto postDesc = desc;
                postDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                const auto histogram = graph.CreateTransientResource("Exposure histogram", histogramDesc,
                                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                const auto post = graph.CreateTransientResource("Lens post output", postDesc,
                                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                graph.AddPass(
                    "Histogram",
                    [&](auto& b) {
                        b.Write(histogram, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    },
                    [](auto*) {
                    });
                graph.AddPass(
                    "Exposure",
                    [&](auto& b) {
                        b.Read(histogram, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    },
                    [](auto*) {
                    });
                graph.AddPass(
                    "Lens output",
                    [&](auto& b) {
                        b.Write(post, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    },
                    [](auto*) {
                    });
                if (graph.Compile() && graph.AllocateTransients(s_Device.Get(), s_Transient))
                {
                    s_Histogram = graph.GetResource(histogram);
                    s_PostB = graph.GetResource(post);
                    s_AliasActive = true;
                }
            }
            s_Width = width;
            s_Height = height;
            s_Format = format;
        }
        if (froxelChanged)
        {
            const auto desc = CD3DX12_RESOURCE_DESC::Tex3D(DXGI_FORMAT_R16G16B16A16_FLOAT, froxels.x, froxels.y,
                                                           static_cast<UINT16>(froxels.z), 1);
            if (!CreateTexture(s_Froxels[0], desc, L"Froxel Fog Medium 0") ||
                !CreateTexture(s_Froxels[1], desc, L"Froxel Fog Medium 1") ||
                !CreateTexture(s_Integrated, desc, L"Froxel Fog Integrated"))
            {
                return false;
            }
            s_FroxelExtent = froxels;
            s_FogHistoryValid = false;
        }
        return true;
    }

    static void WriteDescriptors(UINT slot, ID3D12Resource* scene, ID3D12Resource* depth, ID3D12Resource* velocity,
                                 UINT current)
    {
        const UINT base = slot * kDescriptorsPerFrame;
        const auto srv2D = [&](ID3D12Resource* resource, UINT index) {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = resource->GetDesc().Format;
            if (desc.Format == DXGI_FORMAT_R32_TYPELESS)
            {
                desc.Format = DXGI_FORMAT_R32_FLOAT;
            }
            if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS)
            {
                desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            }
            if (desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
            {
                desc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            }
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels = 1;
            s_Device->CreateShaderResourceView(resource, &desc, Cpu(base + index));
        };
        for (UINT index = 0; index < 2; ++index)
        {
            auto* resource = s_HalfActive ? s_HalfLens[index].Get() : nullptr;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = s_Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            s_Device->CreateShaderResourceView(resource, &srv, Cpu(base + kHalfSrvOffset + index));
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = s_Format;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            s_Device->CreateUnorderedAccessView(resource, nullptr, &uav, Cpu(base + kHalfUavOffset + index));
        }
        const auto srv3D = [&](ID3D12Resource* resource, UINT index) {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture3D.MipLevels = 1;
            s_Device->CreateShaderResourceView(resource, &desc, Cpu(base + index));
        };
        const auto uavTexture = [&](ID3D12Resource* resource, UINT index, bool volume) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = resource->GetDesc().Format;
            if (volume)
            {
                desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                desc.Texture3D.WSize = resource->GetDesc().DepthOrArraySize;
            }
            else
            {
                desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            }
            s_Device->CreateUnorderedAccessView(resource, nullptr, &desc, Cpu(base + kSrvCount + index));
        };
        const auto uavBuffer = [&](ID3D12Resource* resource, UINT index, UINT elements) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.NumElements = elements;
            desc.Buffer.StructureByteStride = 4;
            s_Device->CreateUnorderedAccessView(resource, nullptr, &desc, Cpu(base + kSrvCount + index));
        };
        srv2D(scene, 0);
        srv2D(depth, 1);
        srv2D(velocity, 2);
        srv3D(s_Froxels[current ^ 1u].Get(), 3);
        srv3D(s_Froxels[current].Get(), 4);
        srv3D(s_Integrated.Get(), 5);
        srv2D(s_PostA.Get(), 6);
        srv2D(s_PostB.Get(), 7);
        srv2D(s_PostA.Get(), 8); // 予約済み
        srv2D(s_PostB.Get(), 9); // 予約済み
        uavTexture(s_Froxels[current].Get(), 0, true);
        uavTexture(s_Integrated.Get(), 1, true);
        uavTexture(s_PostA.Get(), 2, false);
        uavTexture(s_PostB.Get(), 3, false);
        uavBuffer(s_Histogram.Get(), 4, 64);
        uavBuffer(s_Exposure.Get(), 5, 4);
    }

    static UINT UploadLights(UINT slot)
    {
        FogLight lights[kMaxLights]{};
        UINT count = 0;
        for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
        {
            if (count >= kMaxLights)
            {
                break;
            }
            const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
            if (!light.IsActive || light.RenderMode != LightRenderMode::Physical || light.Intensity <= 0.0f)
            {
                continue;
            }
            if (s_MegaLightsFog && MegaLights::IsEntityInGrid(entity))
            {
                continue; // M6：MegaLights 側で評価
            }
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            XMFLOAT3 direction;
            XMStoreFloat3(&direction, XMVector3Normalize(XMLoadFloat3(&light.Direction)));
            FogLight& entry = lights[count++];
            entry.PositionType = {transform.Position.x, transform.Position.y, transform.Position.z,
                                  static_cast<float>(light.Type)};
            entry.DirectionRange = {direction.x, direction.y, direction.z, light.Range};
            entry.Color = {light.Color.x * light.Intensity, light.Color.y * light.Intensity,
                           light.Color.z * light.Intensity, (std::max)(light.VolumeDensity, 0.0f)};
            entry.Extras = {cosf(XMConvertToRadians(light.InnerAngle)), cosf(XMConvertToRadians(light.OuterAngle)),
                            static_cast<float>(light.CastShadow), 0.0f};
            entry.IesProfile = LightingResources::GetIesProfileParameters(light);
        }
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        if (SUCCEEDED(s_Lights->Map(0, &noRead, &mapped)))
        {
            memcpy(static_cast<BYTE*>(mapped) + slot * kLightStride, lights, sizeof(lights));
            s_Lights->Unmap(0, nullptr);
        }
        return count;
    }

    static void UploadConstants(UINT slot, UINT lightCount, bool fog, bool exposure, bool motionBlur, bool dof)
    {
        const Settings& settings = s_Settings;
        Constants constants{};
        const EntityID camera = Camera::GetCameraEntity();
        XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
        if (camera != g_kINVALID_ENTITY && ComponentManager::HasComponent<TransformComponent>(camera))
        {
            position = ComponentManager::GetComponentUnchecked<TransformComponent>(camera).Position;
        }
        XMMATRIX view, projection;
        Camera::GetCameraMatrices(camera, view, projection);
        const XMMATRIX viewProjection = view * projection;
        XMStoreFloat4x4(&constants.InvViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjection)));
        constants.PrevViewProjection = s_FogHistoryValid ? s_PrevViewProjection : XMFLOAT4X4{};
        const float deltaTime = std::clamp(World::GetRawDeltaTime(), 0.0f, 0.25f);
        constants.CameraPosition = {position.x, position.y, position.z, deltaTime};
        constants.PrevCameraPosition = {s_PrevCamera.x, s_PrevCamera.y, s_PrevCamera.z,
                                        s_FogHistoryValid ? 1.0f : 0.0f};
        XMStoreFloat4x4(&s_PrevViewProjection, XMMatrixTranspose(viewProjection));
        s_PrevCamera = position;

        const float nearDistance = (std::max)(0.05f, settings.FogNear);
        const float farDistance = (std::max)(nearDistance + 1.0f, settings.FogFar);
        const auto& air = Atmosphere::GetParameters();
        const auto& fogVolumes = LocalHeightFog::GetVolumes();
        const UINT fogCount = static_cast<UINT>((std::min)(fogVolumes.size(), static_cast<size_t>(kMaxLocalFog)));
        constants.Extent = {s_Width, s_Height, s_Frame, lightCount};
        constants.FroxelExtent = {s_FroxelExtent.x, s_FroxelExtent.y, s_FroxelExtent.z, fogCount};
        constants.FogDepth = {nearDistance, farDistance, log2f(farDistance / nearDistance),
                              std::clamp(settings.FogHistoryWeight, 0.0f, 0.97f)};
        const float density = air.Enabled ? (std::max)(0.0f, air.Density) : 0.0f;
        constants.FogMedium0 = {density, (std::max)(0.0f, air.HeightFalloff), (std::max)(0.0f, air.Extinction),
                                (std::max)(0.0001f, air.DistanceScale)};
        constants.FogMedium1 = {(std::max)(0.0f, air.RayleighStrength), (std::max)(0.0f, air.MieStrength),
                                std::clamp(air.MieG, -0.95f, 0.95f),
                                (std::max)(0.0f, air.LightShaftStrength) * (std::max)(0.0f, settings.FogScattering)};
        constants.FogRayleighColor = {air.RayleighColor.x, air.RayleighColor.y, air.RayleighColor.z,
                                      (std::max)(0.0f, air.AmbientStrength) * (std::max)(0.0f, settings.FogAmbient)};
        constants.FogMieColor = {air.MieColor.x, air.MieColor.y, air.MieColor.z,
                                 Raytracing::GetSceneAddress() ? 1.0f : 0.0f};
        const float daylight = DayNightCycle::GetDaylight();
        constants.FogAmbientColor = {0.35f * daylight + 0.01f, 0.45f * daylight + 0.012f, 0.6f * daylight + 0.02f,
                                     0.0f};
        for (UINT index = 0; index < fogCount; ++index)
        {
            const auto& volume = fogVolumes[index];
            constants.LocalFogData0[index] = {volume.Position.x, volume.Position.y, volume.Position.z,
                                              (std::max)(volume.Radius, 0.01f)};
            constants.LocalFogData1[index] = {(std::max)(volume.HeightFalloff, 0.0f), (std::max)(volume.Density, 0.0f),
                                              static_cast<float>(volume.Shape), static_cast<float>(volume.Enabled)};
            constants.LocalFogColor[index] = {volume.Color.x, volume.Color.y, volume.Color.z, 0.0f};
        }
        constants.MegaLightsFog = {s_MegaLightsFog ? 1u : 0u, static_cast<UINT>(MegaLights::GetFogSamples()), 0u, 0u};
        const float minLog = -10.0f, maxLog = 6.0f;
        constants.ExposureParams = {minLog, maxLog - minLog, settings.ExposureCompensation,
                                    (std::max)(0.01f, settings.ExposureSpeed)};
        const float lowPercent = std::clamp(settings.ExposureLowPercent, 0.0f, 0.99f);
        constants.ExposureLimits = {exp2f(settings.ExposureMinEv),
                                    exp2f((std::max)(settings.ExposureMinEv, settings.ExposureMaxEv)), lowPercent,
                                    std::clamp(settings.ExposureHighPercent, lowPercent + 0.01f, 1.0f)};
        constants.MotionBlurParams = {
            std::clamp(settings.MotionBlurShutter, 0.0f, 2.0f), std::clamp(settings.MotionBlurMaxPixels, 1.0f, 128.0f),
            static_cast<float>(std::clamp(settings.MotionBlurSamples, 2, 32)), motionBlur ? 1.0f : 0.0f};
        constants.DofParams = {(std::max)(0.05f, settings.FocusDistance), (std::max)(0.0f, settings.FocusRange),
                               std::clamp(settings.MaxBlurPixels, 1.0f, 48.0f), dof ? 1.0f : 0.0f};
        // ビット1：物理スカイは自身の大気をすでに積分しているため、従来の大気パスと同様に、
        // 背景には追加のフォグを適用しない。
        UINT fogFlags = 0;
        if (fog)
        {
            fogFlags |= 1u;
            if (air.Enabled && air.PhysicalSky)
            {
                fogFlags |= 2u;
            }
        }
        constants.Features = {fogFlags, exposure ? 1u : 0u, motionBlur ? 1u : 0u, dof ? 1u : 0u};
        // w = 1 の場合、ExposureAdapt は保存状態を無視する（未初期化バッファ、再有効化）。
        constants.FogAmbientColor.w = exposure && s_ExposureReset ? 1.0f : 0.0f;
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        if (SUCCEEDED(s_Constants->Map(0, &noRead, &mapped)))
        {
            memcpy(static_cast<BYTE*>(mapped) + slot * kConstantStride, &constants, sizeof(constants));
            s_Constants->Unmap(0, nullptr);
        }
    }
};
