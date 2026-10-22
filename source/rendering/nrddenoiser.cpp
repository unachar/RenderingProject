#include "pch.h"
#include "nrddenoiser.h"
#include "graphicslog.h"
#include <map>

#if defined(RENDERINGPROJECT_USE_NRD)
#include <NRD.h>

namespace
{
    constexpr UINT kFrames = 3;
    constexpr UINT kMaxDispatches = 128;
    constexpr nrd::Identifier kIdentifier = 0;

    DXGI_FORMAT ToDxgi(nrd::Format format)
    {
        constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R8_UNORM,
                                           DXGI_FORMAT_R8_SNORM,
                                           DXGI_FORMAT_R8_UINT,
                                           DXGI_FORMAT_R8_SINT,
                                           DXGI_FORMAT_R8G8_UNORM,
                                           DXGI_FORMAT_R8G8_SNORM,
                                           DXGI_FORMAT_R8G8_UINT,
                                           DXGI_FORMAT_R8G8_SINT,
                                           DXGI_FORMAT_R8G8B8A8_UNORM,
                                           DXGI_FORMAT_R8G8B8A8_SNORM,
                                           DXGI_FORMAT_R8G8B8A8_UINT,
                                           DXGI_FORMAT_R8G8B8A8_SINT,
                                           DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                           DXGI_FORMAT_R16_UNORM,
                                           DXGI_FORMAT_R16_SNORM,
                                           DXGI_FORMAT_R16_UINT,
                                           DXGI_FORMAT_R16_SINT,
                                           DXGI_FORMAT_R16_FLOAT,
                                           DXGI_FORMAT_R16G16_UNORM,
                                           DXGI_FORMAT_R16G16_SNORM,
                                           DXGI_FORMAT_R16G16_UINT,
                                           DXGI_FORMAT_R16G16_SINT,
                                           DXGI_FORMAT_R16G16_FLOAT,
                                           DXGI_FORMAT_R16G16B16A16_UNORM,
                                           DXGI_FORMAT_R16G16B16A16_SNORM,
                                           DXGI_FORMAT_R16G16B16A16_UINT,
                                           DXGI_FORMAT_R16G16B16A16_SINT,
                                           DXGI_FORMAT_R16G16B16A16_FLOAT,
                                           DXGI_FORMAT_R32_UINT,
                                           DXGI_FORMAT_R32_SINT,
                                           DXGI_FORMAT_R32_FLOAT,
                                           DXGI_FORMAT_R32G32_UINT,
                                           DXGI_FORMAT_R32G32_SINT,
                                           DXGI_FORMAT_R32G32_FLOAT,
                                           DXGI_FORMAT_R32G32B32_UINT,
                                           DXGI_FORMAT_R32G32B32_SINT,
                                           DXGI_FORMAT_R32G32B32_FLOAT,
                                           DXGI_FORMAT_R32G32B32A32_UINT,
                                           DXGI_FORMAT_R32G32B32A32_SINT,
                                           DXGI_FORMAT_R32G32B32A32_FLOAT,
                                           DXGI_FORMAT_R10G10B10A2_UNORM,
                                           DXGI_FORMAT_R10G10B10A2_UINT,
                                           DXGI_FORMAT_R11G11B10_FLOAT,
                                           DXGI_FORMAT_R9G9B9E5_SHAREDEXP};
        static_assert(size(formats) == static_cast<size_t>(nrd::Format::MAX_NUM));
        const auto index = static_cast<size_t>(format);
        return index < size(formats) ? formats[index] : DXGI_FORMAT_UNKNOWN;
    }

    struct Texture
    {
        ComPtr<ID3D12Resource> Resource;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
    };

    // ---------------------------------------------------------------------------------------------
    // REBLUR の調整。任意メンバーは、リンク対象の NRD ヘッダーに宣言がある場合だけ設定し、
    // 近接する 4.x SDK の版でも、誤った設定を黙って使わずに同じソースをビルドできるようにする。
    // ---------------------------------------------------------------------------------------------
    // ヒット距離の正規化は RayReflectionsCS.hlsl（REBLUR_FrontEnd_GetNormHitDist）と共有する。
    constexpr float kHitDistanceA = 3.0f;
    constexpr float kHitDistanceB = 0.1f;
    constexpr float kHitDistanceC = 20.0f;

    // リアルタイム GI は2ピクセルにつき1本のチェッカーボードレイ（0.5 rpp）を使う。
    // 広い空間ぼかしの代わりに、制限した長い履歴で安定させ、形状の輪郭を保つ。
    constexpr uint32_t kGiMaxAccumulatedFrames = 40;    // 120 fps で約0.33秒。変化時はアンチラグで短縮する
    constexpr uint32_t kGiMaxFastAccumulatedFrames = 8; // 制限とアンチラグに使う短期履歴
    constexpr uint32_t kGiMaxStabilizedFrames = 63;     // 履歴フレーム数の上限（REBLUR_MAX_HISTORY_FRAME_NUM）
    constexpr float kGiMinBlurRadius = 2.0f;            // 収束後のフィルター範囲（ピクセル）
    constexpr float kGiMaxBlurRadius = 32.0f;           // 履歴が短い間のフィルター範囲（ピクセル）
    constexpr float kGiPrepassBlurRadius = 40.0f;       // 前処理でチェッカーボードの欠損も補う
    constexpr float kGiFireflyRelativeScale = 1.5f;

    template <class Settings> void ApplyHitDistance(Settings& settings, float a, float b, float c)
    {
        settings.hitDistanceParameters.A = a;
        settings.hitDistanceParameters.B = b;
        settings.hitDistanceParameters.C = c;
    }

    template <class Settings> void EnableAntiFirefly(Settings& settings)
    {
        if constexpr (requires(Settings& value) { value.enableAntiFirefly = true; })
        {
            settings.enableAntiFirefly = true;
        }
    }

    template <class Settings> void ConfigureStableDiffuse(Settings& settings)
    {
        // TraceDiffuse は各行の追跡済み半分を [0, width/2) に詰めて書き、書き込む側の偶奇を
        // CommonSettings::frameIndex で交互に変える。NRD が要求する配置は次のとおり。
        // データのあるピクセルは（x >> 1, y）から読み、残りは前処理で復元する。
        settings.checkerboardMode = nrd::CheckerboardMode::BLACK;
        ApplyHitDistance(settings, kHitDistanceA, kHitDistanceB, kHitDistanceC);
        settings.maxAccumulatedFrameNum = kGiMaxAccumulatedFrames;
        settings.maxFastAccumulatedFrameNum = kGiMaxFastAccumulatedFrames;
        settings.minBlurRadius = kGiMinBlurRadius;
        settings.maxBlurRadius = kGiMaxBlurRadius;
        settings.fireflySuppressorMinRelativeScale = kGiFireflyRelativeScale;
        if constexpr (requires(Settings& value) { value.maxStabilizedFrameNum = 1u; })
        {
            settings.maxStabilizedFrameNum = kGiMaxStabilizedFrames;
        }
        if constexpr (requires(Settings& value) { value.diffusePrepassBlurRadius = 1.0f; })
        {
            settings.diffusePrepassBlurRadius = kGiPrepassBlurRadius;
        }
        EnableAntiFirefly(settings);
    }

    void ConfigureStableSpecular(nrd::ReblurSettings& settings, bool stableProfile)
    {
        ApplyHitDistance(settings, kHitDistanceA, kHitDistanceB, kHitDistanceC);
        EnableAntiFirefly(settings);
        if (stableProfile)
        {
            // 低解像度の鏡面では短い履歴に粒状ノイズが残る。
            // 空間ぼかしを広げず、変化への追従に使う短期履歴も維持する。
            settings.maxAccumulatedFrameNum = 63;
            settings.maxFastAccumulatedFrameNum = 8;
            settings.maxStabilizedFrameNum = 63;
        }
    }

    void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                    D3D12_RESOURCE_STATES target)
    {
        if (state != target)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, state, target);
            commands->ResourceBarrier(1, &barrier);
            state = target;
        }
        else if (target == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(resource);
            commands->ResourceBarrier(1, &barrier);
        }
    }
} // 名前空間

struct NrdDenoiser::Impl
{
    nrd::Instance* Instance = nullptr;
    ComPtr<ID3D12Device> Device;
    ComPtr<ID3D12RootSignature> Root;
    ComPtr<ID3D12DescriptorHeap> Heap;
    ComPtr<ID3D12Resource> Constants;
    vector<ComPtr<ID3D12PipelineState>> Pipelines;
    vector<Texture> Permanent, Transient;
    Texture Output;
    NrdSignal Signal = NrdSignal::Shadow;
    UINT Width = 0, Height = 0;
    UINT Increment = 0, DescriptorsPerFrame = 0, ConstantStride = 0;
    UINT SrvCount = 0, UavCount = 0, Dispatches = 0;
    bool FirstFrame = true;
    UINT LastSequence = 0;
    bool RestartedHistory = false;

    ~Impl()
    {
        if (Instance)
        {
            nrd::DestroyInstance(*Instance);
        }
    }

    bool CreateTexture(Texture& texture, DXGI_FORMAT format, UINT factor, const wchar_t* name)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description =
            CD3DX12_RESOURCE_DESC::Tex2D(format, (Width + factor - 1) / factor, (Height + factor - 1) / factor, 1, 1, 1,
                                         0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, texture.State, nullptr,
                                                   IID_PPV_ARGS(&texture.Resource))))
        {
            return false;
        }
        texture.Resource->SetName(name);
        return true;
    }

    bool Create(NrdSignal signal, UINT width, UINT height)
    {
        Signal = signal;
        Width = width;
        Height = height;
        const nrd::DenoiserDesc denoiser{kIdentifier,
                                         signal == NrdSignal::Shadow ? nrd::Denoiser::SIGMA_SHADOW
                                         : (signal == NrdSignal::Specular || signal == NrdSignal::SpecularCheckerboard)
                                             ? nrd::Denoiser::REBLUR_SPECULAR
                                         : (signal == NrdSignal::Diffuse || signal == NrdSignal::MegaLights)
                                             ? nrd::Denoiser::REBLUR_DIFFUSE
                                             : nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION};
        const nrd::InstanceCreationDesc creation{{}, &denoiser, 1};
        if (nrd::CreateInstance(creation, Instance) != nrd::Result::SUCCESS)
        {
            return false;
        }
        const auto& description = *nrd::GetInstanceDesc(*Instance);
        SrvCount = (max)(1u, description.descriptorPoolDesc.perSetTexturesMaxNum);
        UavCount = (max)(1u, description.descriptorPoolDesc.perSetStorageTexturesMaxNum);
        ConstantStride = (description.constantBufferMaxDataSize + 255u) & ~255u;
        DescriptorsPerFrame = (SrvCount + UavCount) * kMaxDispatches;
        Increment = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, SrvCount, description.resourcesBaseRegisterIndex,
                       description.resourcesSpaceIndex);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UavCount, description.resourcesBaseRegisterIndex,
                       description.resourcesSpaceIndex);
        CD3DX12_ROOT_PARAMETER parameters[3];
        parameters[0].InitAsConstantBufferView(description.constantBufferRegisterIndex,
                                               description.constantBufferAndSamplersSpaceIndex);
        parameters[1].InitAsDescriptorTable(1, &ranges[0]);
        parameters[2].InitAsDescriptorTable(1, &ranges[1]);
        vector<CD3DX12_STATIC_SAMPLER_DESC> samplers;
        for (UINT index = 0; index < description.samplersNum; ++index)
        {
            samplers.emplace_back(
                description.samplersBaseRegisterIndex + index,
                description.samplers[index] == nrd::Sampler::NEAREST_CLAMP ? D3D12_FILTER_MIN_MAG_MIP_POINT
                                                                           : D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0,
                1, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0, D3D12_FLOAT32_MAX,
                D3D12_SHADER_VISIBILITY_ALL, description.constantBufferAndSamplersSpaceIndex);
        }
        const CD3DX12_ROOT_SIGNATURE_DESC rootDescription(3, parameters, static_cast<UINT>(samplers.size()),
                                                          samplers.data());
        ComPtr<ID3DBlob> serialized, errors;
        if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
            FAILED(Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&Root))))
        {
            return false;
        }
        Pipelines.resize(description.pipelinesNum);
        for (UINT index = 0; index < description.pipelinesNum; ++index)
        {
            const auto shader = description.pipelines[index].computeShaderDXIL;
            if (!shader.bytecode || !shader.size)
            {
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = Root.Get();
            pipeline.CS = {shader.bytecode, static_cast<SIZE_T>(shader.size)};
            if (FAILED(Device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&Pipelines[index]))))
            {
                return false;
            }
        }
        D3D12_DESCRIPTOR_HEAP_DESC descriptorHeap{};
        descriptorHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptorHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        descriptorHeap.NumDescriptors = DescriptorsPerFrame * kFrames;
        if (FAILED(Device->CreateDescriptorHeap(&descriptorHeap, IID_PPV_ARGS(&Heap))))
        {
            return false;
        }
        const auto upload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(UINT64(ConstantStride) * kMaxDispatches * kFrames);
        if (FAILED(Device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&Constants))))
        {
            return false;
        }
        Permanent.resize(description.permanentPoolSize);
        Transient.resize(description.transientPoolSize);
        const auto allocatePool = [&](auto& pool, const nrd::TextureDesc* textures, const wchar_t* name) {
            for (size_t index = 0; index < pool.size(); ++index)
            {
                if (!CreateTexture(pool[index], ToDxgi(textures[index].format), textures[index].downsampleFactor, name))
                {
                    return false;
                }
            }
            return true;
        };
        if (!allocatePool(Permanent, description.permanentPool, L"NRD Permanent History") ||
            !allocatePool(Transient, description.transientPool, L"NRD Transient Pool") ||
            !CreateTexture(Output,
                           Signal == NrdSignal::Shadow ? DXGI_FORMAT_R8_UNORM
                           : (Signal == NrdSignal::Specular || Signal == NrdSignal::SpecularCheckerboard ||
                              Signal == NrdSignal::Diffuse || Signal == NrdSignal::MegaLights)
                               ? DXGI_FORMAT_R16G16B16A16_FLOAT
                               : DXGI_FORMAT_R16_UNORM,
                           1, L"NVIDIA NRD Denoised Signal"))
        {
            return false;
        }
        WriteGraphicsLog("NRD: %s %s initialized, extent=%ux%u pipelines=%u history=%u transient=%u\n",
                         NrdDenoiser::GetVersion(), nrd::GetDenoiserString(denoiser.denoiser), Width, Height,
                         description.pipelinesNum, description.permanentPoolSize, description.transientPoolSize);
        return true;
    }

    bool Execute(ID3D12GraphicsCommandList* commands, const NrdInputs& inputs, const NrdFrame& frame)
    {
        Dispatches = 0;
        if (!commands || !inputs.Motion.Resource || !inputs.NormalRoughness.Resource || !inputs.ViewZ.Resource ||
            !inputs.NoisySignal.Resource)
        {
            return false;
        }
        const auto matchesExtent = [&](ID3D12Resource* resource, UINT minimumWidth) {
            const auto description = resource->GetDesc();
            return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.Width >= minimumWidth &&
                   description.Width <= Width && description.Height == Height && description.DepthOrArraySize == 1;
        };
        const bool checkerboard = Signal == NrdSignal::Diffuse || Signal == NrdSignal::SpecularCheckerboard;
        const UINT signalWidth = checkerboard ? (Width + 1) / 2 : Width;
        if (!matchesExtent(inputs.Motion.Resource, Width) || !matchesExtent(inputs.NormalRoughness.Resource, Width) ||
            !matchesExtent(inputs.ViewZ.Resource, Width) || !matchesExtent(inputs.NoisySignal.Resource, signalWidth))
        {
            WriteGraphicsLog(
                "NRD: input extent contract rejected signal=%u expected=%ux%u packedMinimum=%u sequence=%u\n",
                static_cast<UINT>(Signal), Width, Height, signalWidth, frame.Sequence);
            return false;
        }
        if (abs(frame.Jitter.x) > 0.5f || abs(frame.Jitter.y) > 0.5f || abs(frame.PreviousJitter.x) > 0.5f ||
            abs(frame.PreviousJitter.y) > 0.5f)
        {
            WriteGraphicsLog(
                "NRD: rejected projection jitter sequence=%u size=%ux%u current=(%.7f,%.7f) previous=(%.7f,%.7f)\n",
                frame.Sequence, Width, Height, frame.Jitter.x, frame.Jitter.y, frame.PreviousJitter.x,
                frame.PreviousJitter.y);
            return false;
        }
        nrd::CommonSettings common;
        memcpy(common.worldToViewMatrix, &frame.View, sizeof(frame.View));
        memcpy(common.viewToClipMatrix, &frame.Projection, sizeof(frame.Projection));
        memcpy(common.worldToViewMatrixPrev, &frame.PreviousView, sizeof(frame.PreviousView));
        memcpy(common.viewToClipMatrixPrev, &frame.PreviousProjection, sizeof(frame.PreviousProjection));
        common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] =
            static_cast<uint16_t>(Width);
        common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] =
            static_cast<uint16_t>(Height);
        common.cameraJitter[0] = frame.Jitter.x;
        common.cameraJitter[1] = frame.Jitter.y;
        common.cameraJitterPrev[0] = frame.PreviousJitter.x;
        common.cameraJitterPrev[1] = frame.PreviousJitter.y;
        common.frameIndex = frame.Sequence;
        // この設定がないと NRD は内部の実時間時計を使うため、同じ記録済み入力でも
        // チェッカーボードの復元やアンチラグの重みが変わってしまう。
        common.timeDeltaBetweenFrames = frame.StableProfile ? 1000.f / 60.f : 0.f;
        common.motionVectorScale[0] = common.motionVectorScale[1] = 1.f;
        common.motionVectorScale[2] = 0.f;
        common.isMotionVectorInWorldSpace = false;
        common.disocclusionThreshold = frame.StableProfile ? .015f : .01f;
        common.denoisingRange = frame.Range;
        common.accumulationMode = FirstFrame ? nrd::AccumulationMode::CLEAR_AND_RESTART
                                  : (frame.ResetHistory || frame.Sequence - LastSequence != 1u)
                                      ? nrd::AccumulationMode::RESTART
                                      : nrd::AccumulationMode::CONTINUE;
        if (FirstFrame || frame.ResetHistory)
        {
            WriteGraphicsLog(
                "NRD contract: signal=%u extent=%ux%u noisyWidth=%llu checkerboard=%u stable=%u dtMs=%.6f reset=%u sequence=%u\n",
                static_cast<UINT>(Signal), Width, Height,
                static_cast<unsigned long long>(inputs.NoisySignal.Resource->GetDesc().Width), checkerboard,
                frame.StableProfile, common.timeDeltaBetweenFrames, frame.HistoryResetReasons, frame.Sequence);
        }
        if (nrd::SetCommonSettings(*Instance, common) != nrd::Result::SUCCESS)
        {
            return false;
        }
        if (Signal == NrdSignal::Shadow)
        {
            nrd::SigmaSettings settings;
            settings.lightDirection[0] = frame.LightDirection.x;
            settings.lightDirection[1] = frame.LightDirection.y;
            settings.lightDirection[2] = frame.LightDirection.z;
            if (frame.StableProfile)
            {
                settings.maxStabilizedFrameNum = 7;
                settings.planeDistanceSensitivity = .025f;
            }
            if (nrd::SetDenoiserSettings(*Instance, kIdentifier, &settings) != nrd::Result::SUCCESS)
            {
                return false;
            }
        }
        if (Signal == NrdSignal::Diffuse)
        {
            // 偶奇設定は固定する。NRD 内部ですでに frameIndex による交互切り替えを行う。
            // frameIndex は TraceDiffuse と同じ frame.Sequence であり、毎フレーム BLACK/WHITE を反転すると
            // NRD がチェッカーボードの空の半分を読んでしまう。
            nrd::ReblurSettings settings;
            ConfigureStableDiffuse(settings);
            if (frame.StableProfile)
            {
                settings.maxAccumulatedFrameNum = 63;
                settings.fastHistoryClampingSigmaScale = 2.5f;
            }
            if (nrd::SetDenoiserSettings(*Instance, kIdentifier, &settings) != nrd::Result::SUCCESS)
            {
                return false;
            }
        }
        if (Signal == NrdSignal::MegaLights)
        {
            // MegaLights の直接光（デモジュレート済み・フル解像度・全ピクセル）。チェッカーボードなし。
            // ヒット距離はシェーディング点からライト上の点までの距離（MegaLightsNrdCS.hlsl と同じ正規化）
            nrd::ReblurSettings settings;
            ApplyHitDistance(settings, kHitDistanceA, kHitDistanceB, kHitDistanceC);
            EnableAntiFirefly(settings);
            if (nrd::SetDenoiserSettings(*Instance, kIdentifier, &settings) != nrd::Result::SUCCESS)
            {
                return false;
            }
        }
        if (Signal == NrdSignal::SpecularCheckerboard)
        {
            // 反射の半分のピクセルだけをトレース（RayReflectionsCS main：(x >> 1, y) に詰めて格納）。
            // パリティは frameIndex で NRD が交互に切り替えるので、モードは BLACK に固定する（GI と同じ）
            nrd::ReblurSettings settings;
            settings.checkerboardMode = nrd::CheckerboardMode::BLACK;
            ConfigureStableSpecular(settings, frame.StableProfile);
            if (nrd::SetDenoiserSettings(*Instance, kIdentifier, &settings) != nrd::Result::SUCCESS)
            {
                return false;
            }
        }
        if (Signal == NrdSignal::Specular)
        {
            // 全ピクセル追跡時も、半解像度と同じ反射の安定化設定を使う。
            nrd::ReblurSettings settings;
            ConfigureStableSpecular(settings, frame.StableProfile);
            if (nrd::SetDenoiserSettings(*Instance, kIdentifier, &settings) != nrd::Result::SUCCESS)
            {
                return false;
            }
        }
        if (Signal == NrdSignal::AmbientOcclusion)
        {
            nrd::ReblurSettings settings;
            settings.hitDistanceParameters.A = frame.AmbientRadius;
            settings.hitDistanceParameters.B = 0.000001f;
            settings.hitDistanceParameters.C = 1.0f;
            if (nrd::SetDenoiserSettings(*Instance, kIdentifier, &settings) != nrd::Result::SUCCESS)
            {
                return false;
            }
        }
        const nrd::DispatchDesc* dispatches = nullptr;
        uint32_t count = 0;
        if (nrd::GetComputeDispatches(*Instance, &kIdentifier, 1, dispatches, count) != nrd::Result::SUCCESS ||
            count > kMaxDispatches)
        {
            return false;
        }

        // コマンド記録や状態変更の前に、ディスパッチ列全体を検証する。
        map<ID3D12Resource*, D3D12_RESOURCE_STATES> externalStates;
        for (const auto& texture : {inputs.Motion, inputs.NormalRoughness, inputs.ViewZ, inputs.NoisySignal})
        {
            externalStates[texture.Resource] = texture.State;
        }
        const auto resolve = [&](const nrd::ResourceDesc& resource) -> ID3D12Resource* {
            switch (resource.type)
            {
                case nrd::ResourceType::PERMANENT_POOL:
                    return resource.indexInPool < Permanent.size() ? Permanent[resource.indexInPool].Resource.Get()
                                                                   : nullptr;
                case nrd::ResourceType::TRANSIENT_POOL:
                    return resource.indexInPool < Transient.size() ? Transient[resource.indexInPool].Resource.Get()
                                                                   : nullptr;
                case nrd::ResourceType::IN_MV:
                    return inputs.Motion.Resource;
                case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
                    return inputs.NormalRoughness.Resource;
                case nrd::ResourceType::IN_VIEWZ:
                    return inputs.ViewZ.Resource;
                case nrd::ResourceType::IN_PENUMBRA:
                case nrd::ResourceType::IN_DIFF_HITDIST:
                case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
                case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
                    return inputs.NoisySignal.Resource;
                case nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY:
                case nrd::ResourceType::OUT_DIFF_HITDIST:
                case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
                case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
                    return Output.Resource.Get();
                default:
                    return nullptr;
            }
        };
        for (UINT dispatch = 0; dispatch < count; ++dispatch)
        {
            for (UINT index = 0; index < dispatches[dispatch].resourcesNum; ++index)
            {
                if (!resolve(dispatches[dispatch].resources[index]))
                {
                    return false;
                }
            }
        }

        BYTE* mapped = nullptr;
        const CD3DX12_RANGE noRead(0, 0);
        if (FAILED(Constants->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
        {
            return false;
        }
        const UINT frameSlot = frame.FrameIndex % kFrames;
        ID3D12DescriptorHeap* heaps[] = {Heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(Root.Get());
        const auto& description = *nrd::GetInstanceDesc(*Instance);
        for (UINT dispatchIndex = 0; dispatchIndex < count; ++dispatchIndex)
        {
            const auto& dispatch = dispatches[dispatchIndex];
            const auto& pipeline = description.pipelines[dispatch.pipelineIndex];
            const UINT descriptorBase = frameSlot * DescriptorsPerFrame + dispatchIndex * (SrvCount + UavCount);
            const auto cpu = [&](UINT offset) {
                return CD3DX12_CPU_DESCRIPTOR_HANDLE(Heap->GetCPUDescriptorHandleForHeapStart(),
                                                     descriptorBase + offset, Increment);
            };
            const auto gpu = [&](UINT offset) {
                return CD3DX12_GPU_DESCRIPTOR_HANDLE(Heap->GetGPUDescriptorHandleForHeapStart(),
                                                     descriptorBase + offset, Increment);
            };
            UINT resourceIndex = 0;
            for (UINT rangeIndex = 0; rangeIndex < pipeline.resourceRangesNum; ++rangeIndex)
            {
                const auto& range = pipeline.resourceRanges[rangeIndex];
                const bool writable = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
                for (UINT index = 0; index < range.descriptorsNum; ++index, ++resourceIndex)
                {
                    const auto& binding = dispatch.resources[resourceIndex];
                    auto* resource = resolve(binding);
                    D3D12_RESOURCE_STATES* state = nullptr;
                    if (binding.type == nrd::ResourceType::PERMANENT_POOL)
                    {
                        state = &Permanent[binding.indexInPool].State;
                    }
                    else if (binding.type == nrd::ResourceType::TRANSIENT_POOL)
                    {
                        state = &Transient[binding.indexInPool].State;
                    }
                    else if (resource == Output.Resource.Get())
                    {
                        state = &Output.State;
                    }
                    else
                    {
                        state = &externalStates.at(resource);
                    }
                    Transition(commands, resource, *state,
                               writable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    if (writable)
                    {
                        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
                        view.Format = resource->GetDesc().Format;
                        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                        Device->CreateUnorderedAccessView(resource, nullptr, &view, cpu(SrvCount + index));
                    }
                    else
                    {
                        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                        view.Format = resource->GetDesc().Format;
                        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        view.Texture2D.MipLevels = 1;
                        Device->CreateShaderResourceView(resource, &view, cpu(index));
                    }
                }
            }
            const UINT64 constantOffset = (UINT64(frameSlot) * kMaxDispatches + dispatchIndex) * ConstantStride;
            if (dispatch.constantBufferDataSize)
            {
                memcpy(mapped + constantOffset, dispatch.constantBufferData, dispatch.constantBufferDataSize);
            }
            commands->SetPipelineState(Pipelines[dispatch.pipelineIndex].Get());
            commands->SetComputeRootConstantBufferView(0, Constants->GetGPUVirtualAddress() + constantOffset);
            commands->SetComputeRootDescriptorTable(1, gpu(0));
            commands->SetComputeRootDescriptorTable(2, gpu(SrvCount));
            commands->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
        }
        Constants->Unmap(0, nullptr);
        for (const auto& input : {inputs.Motion, inputs.NormalRoughness, inputs.ViewZ, inputs.NoisySignal})
        {
            Transition(commands, input.Resource, externalStates.at(input.Resource), input.State);
        }
        Transition(commands, Output.Resource.Get(), Output.State, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Dispatches = count;
        RestartedHistory = common.accumulationMode != nrd::AccumulationMode::CONTINUE;
        FirstFrame = false;
        LastSequence = frame.Sequence;
        return true;
    }
};
#else
struct NrdDenoiser::Impl
{
};
#endif

NrdDenoiser::NrdDenoiser() = default;
NrdDenoiser::~NrdDenoiser() = default;

bool NrdDenoiser::Initialize(ID3D12Device* device, NrdSignal signal, UINT width, UINT height)
{
#if defined(RENDERINGPROJECT_USE_NRD)
    if (!device || !width || !height || width > UINT16_MAX || height > UINT16_MAX)
    {
        return false;
    }
    auto created = make_unique<Impl>();
    created->Device = device;
    if (!created->Create(signal, width, height))
    {
        return false;
    }
    m_Impl = move(created);
    return true;
#else
    return false;
#endif
}

bool NrdDenoiser::Execute(ID3D12GraphicsCommandList* commands, const NrdInputs& inputs, const NrdFrame& frame)
{
#if defined(RENDERINGPROJECT_USE_NRD)
    return m_Impl && m_Impl->Execute(commands, inputs, frame);
#else
    return false;
#endif
}

ID3D12Resource* NrdDenoiser::GetOutput() const
{
#if defined(RENDERINGPROJECT_USE_NRD)
    return m_Impl ? m_Impl->Output.Resource.Get() : nullptr;
#else
    return nullptr;
#endif
}

UINT NrdDenoiser::GetDispatchCount() const
{
#if defined(RENDERINGPROJECT_USE_NRD)
    return m_Impl ? m_Impl->Dispatches : 0;
#else
    return 0;
#endif
}

bool NrdDenoiser::DidRestartHistory() const
{
#if defined(RENDERINGPROJECT_USE_NRD)
    return m_Impl && m_Impl->RestartedHistory;
#else
    return false;
#endif
}

bool NrdDenoiser::IsCompiled()
{
#if defined(RENDERINGPROJECT_USE_NRD)
    return true;
#else
    return false;
#endif
}

const char* NrdDenoiser::GetVersion()
{
#if defined(RENDERINGPROJECT_USE_NRD)
    return "4.17.3";
#else
    return "SDK 未導入";
#endif
}
