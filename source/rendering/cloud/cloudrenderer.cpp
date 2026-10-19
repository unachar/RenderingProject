#include "pch.h"
#include "cloudrenderer.h"
#include "rendererstate.h"
#include "renderprofiler.h"
#include "graphicslog.h"
#include "proceduralio.h"
#include "proceduralstudio.h"
#include <future>
#include <DirectXPackedVector.h>
#include <cstddef>

namespace
{
    constexpr UINT kFrames = RendererState::g_kFRAME_COUNT;
    static_assert(kFrames * 2 == CloudRenderer::kRtvCount);
    constexpr UINT kDescriptorStart = RendererState::g_kCLOUD_DESCRIPTOR_START;
    constexpr UINT kBaseSrv = kDescriptorStart;
    constexpr UINT kBaseUav = kDescriptorStart + 1;
    constexpr UINT kDetailSrv = kDescriptorStart + 2;
    constexpr UINT kDetailUav = kDescriptorStart + 3;
    constexpr UINT kFrameSrv = kDescriptorStart + 4;
    constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    struct LayerConstants
    {
        XMFLOAT4 Layer;
        XMFLOAT4 Shape;
        XMFLOAT4 Wind;
    };

    static_assert(sizeof(LayerConstants) == 48);

    struct alignas(16) Constants
    {
        XMFLOAT4 Layer;
        XMFLOAT4 Shape;
        XMFLOAT4 Lighting;
        XMFLOAT4 Wind;
        XMUINT4 Quality;
        XMFLOAT4 Extent;
        XMFLOAT4 Trace;
        LayerConstants AdditionalLayers[CloudSettings::MaxLayers - 1];
    };

    static_assert(sizeof(Constants) == 256 && offsetof(Constants, Quality) == 64 && offsetof(Constants, Trace) == 96);
    static_assert(offsetof(Constants, AdditionalLayers) == 112);
    constexpr UINT kConstantStride = (sizeof(Constants) + 255) & ~255u;

    struct FrameResources
    {
        ComPtr<ID3D12Resource> Color;
        ComPtr<ID3D12Resource> Depth;
        UINT Width = 0;
        UINT Height = 0;
    };

    CloudSettings g_Settings;
    ComPtr<ID3D12Device> g_Device;
    ComPtr<ID3D12DescriptorHeap> g_Heap;
    ComPtr<ID3D12RootSignature> g_DrawRoot;
    ComPtr<ID3D12RootSignature> g_NoiseRoot;
    ComPtr<ID3D12PipelineState> g_RaymarchPso;
    ComPtr<ID3D12PipelineState> g_CompositeHdrPso;
    ComPtr<ID3D12PipelineState> g_CompositeLdrPso;
    ComPtr<ID3D12PipelineState> g_BasePso;
    ComPtr<ID3D12PipelineState> g_DetailPso;
    ComPtr<ID3D12Resource> g_BaseNoise;
    ComPtr<ID3D12Resource> g_DetailNoise;
    ComPtr<ID3D12Resource> g_Constants;
    array<FrameResources, kFrames> g_Frames;
    UINT8* g_Mapped = nullptr;
    UINT g_Increment = 0;
    UINT g_RtvIncrement = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE g_RtvStart{};
    bool g_NoiseDirty = true;
    bool g_Available = false;
    uint32_t g_GeneratedSeed = 0;
    UINT g_GenerationCount = 0;
    // 長時間動作してもフレーム単位の増分が失われないよう、オフセットを倍精度で保持する。
    double g_WindX = 0;
    double g_WindZ = 0;
    constexpr UINT kBaseExtent = 128;
    // ユーザー指定のベースノイズ（Procedural Studio / テクスチャ駆動）。CPU 側に保持し、
    // デバイスリセットや雲の有効化による組み込みノイズ再生成の後に、再アップロードできるようにする。
    vector<float> g_CustomNoise;
    string g_CustomLabel;
    bool g_CustomActive = false;
    bool g_CustomPending = false;
    array<ComPtr<ID3D12Resource>, kFrames> g_CustomUploads;

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Heap->GetCPUDescriptorHandleForHeapStart(), index, g_Increment);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(g_Heap->GetGPUDescriptorHandleForHeapStart(), index, g_Increment);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT frame, UINT target)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_RtvStart, frame * 2 + target, g_RtvIncrement);
    }

    void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        commands->ResourceBarrier(1, &barrier);
    }

    bool CreateRoot(const CD3DX12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>& root)
    {
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
        {
            if (errors)
            {
                WriteGraphicsLog("Cloud root signature: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            }
            return false;
        }
        return SUCCEEDED(
            g_Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    }

    bool LoadCompute(const wchar_t* path, ComPtr<ID3D12PipelineState>& pso)
    {
        ComPtr<ID3DBlob> shader;
        if (FAILED(D3DReadFileToBlob(path, &shader)))
        {
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g_NoiseRoot.Get();
        desc.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
        return SUCCEEDED(g_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    }

    bool LoadGraphics(const wchar_t* path, DXGI_FORMAT format, bool composite, ComPtr<ID3D12PipelineState>& pso)
    {
        ComPtr<ID3DBlob> vertex, pixel;
        if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/postProcessVS.cso", &vertex)) ||
            FAILED(D3DReadFileToBlob(path, &pixel)))
        {
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g_DrawRoot.Get();
        desc.VS = CD3DX12_SHADER_BYTECODE(vertex.Get());
        desc.PS = CD3DX12_SHADER_BYTECODE(pixel.Get());
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = composite ? 1 : 2;
        desc.RTVFormats[0] = format;
        if (!composite)
        {
            desc.RTVFormats[1] = DXGI_FORMAT_R32G32_FLOAT;
        }
        desc.SampleDesc.Count = 1;
        if (composite)
        {
            auto& blend = desc.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_SRC_ALPHA;
            blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
        }
        return SUCCEEDED(g_Device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    }

    bool CreateNoise(UINT extent, UINT srvIndex, UINT uavIndex, ComPtr<ID3D12Resource>& resource)
    {
        // 小さいノイズ形式を優先し、型付き R8 UAV 書き込み非対応時はフォールバックする。
        DXGI_FORMAT format = DXGI_FORMAT_R16_FLOAT;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R8_UNORM};
        if (SUCCEEDED(g_Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
            (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) &&
            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        {
            format = DXGI_FORMAT_R8_UNORM;
        }
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex3D(format, extent, extent, static_cast<UINT16>(extent), 1,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kRead, nullptr,
                                                     IID_PPV_ARGS(&resource))))
        {
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture3D.MipLevels = 1;
        g_Device->CreateShaderResourceView(resource.Get(), &srv, Cpu(srvIndex));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = desc.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uav.Texture3D.WSize = extent;
        g_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uav, Cpu(uavIndex));
        WriteGraphicsLog("Cloud noise: %ux%ux%u format=%s\n", extent, extent, extent,
                         format == DXGI_FORMAT_R8_UNORM ? "R8_UNORM" : "R16_FLOAT");
        return true;
    }

    bool CreateTarget(UINT width, UINT height, DXGI_FORMAT format, UINT srvIndex, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                      ComPtr<ID3D12Resource>& resource)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc =
            CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kRead, nullptr,
                                                     IID_PPV_ARGS(&resource))))
        {
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_Device->CreateShaderResourceView(resource.Get(), &srv, Cpu(srvIndex));
        g_Device->CreateRenderTargetView(resource.Get(), nullptr, rtv);
        return true;
    }

    bool EnsureFrame(UINT index, UINT width, UINT height)
    {
        auto& frame = g_Frames[index];
        if (frame.Color && frame.Depth && frame.Width == width && frame.Height == height)
        {
            return true;
        }
        // 描画側はコマンドリストの記録前に、このフレームのフェンス完了を待っている。
        frame = {};
        if (!CreateTarget(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, kFrameSrv + index * 2, Rtv(index, 0),
                          frame.Color) ||
            !CreateTarget(width, height, DXGI_FORMAT_R32G32_FLOAT, kFrameSrv + index * 2 + 1, Rtv(index, 1),
                          frame.Depth))
        {
            return false;
        }
        frame.Width = width;
        frame.Height = height;
        frame.Color->SetName(L"Cloud Scattering / Transmittance");
        frame.Depth->SetName(L"Cloud Scene / Weighted Depth");
        return true;
    }

    void DispatchNoise(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, ID3D12PipelineState* pso,
                       UINT uavIndex, UINT extent)
    {
        Transition(commands, resource, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->SetPipelineState(pso);
        commands->SetComputeRootSignature(g_NoiseRoot.Get());
        commands->SetComputeRootDescriptorTable(0, Gpu(uavIndex));
        const UINT constants[] = {g_Settings.Seed, extent};
        commands->SetComputeRoot32BitConstants(1, 2, constants, 0);
        commands->Dispatch((extent + 3) / 4, (extent + 3) / 4, (extent + 3) / 4);
        Transition(commands, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    }

    bool UploadCustomNoise(ID3D12GraphicsCommandList* commands, UINT frame)
    {
        if (!g_BaseNoise || g_CustomNoise.size() != size_t(kBaseExtent) * kBaseExtent * kBaseExtent)
        {
            return false;
        }
        const DXGI_FORMAT format = g_BaseNoise->GetDesc().Format;
        const UINT bytesPerTexel = format == DXGI_FORMAT_R8_UNORM ? 1u : 2u;
        vector<uint8_t> texels(g_CustomNoise.size() * bytesPerTexel);
        for (size_t i = 0; i < g_CustomNoise.size(); ++i)
        {
            const float value = clamp(g_CustomNoise[i], 0.0f, 1.0f);
            if (bytesPerTexel == 1)
            {
                texels[i] = static_cast<uint8_t>(value * 255.0f + 0.5f);
            }
            else
            {
                const auto half = PackedVector::XMConvertFloatToHalf(value);
                memcpy(&texels[i * 2], &half, 2);
            }
        }
        auto& upload = g_CustomUploads[frame % kFrames];
        const UINT64 bytes = GetRequiredIntermediateSize(g_BaseNoise.Get(), 0, 1);
        if (!upload || upload->GetDesc().Width < bytes)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(upload.ReleaseAndGetAddressOf()))))
            {
                return false;
            }
            upload->SetName(L"Cloud Custom Noise Upload");
        }
        D3D12_SUBRESOURCE_DATA data{};
        data.pData = texels.data();
        data.RowPitch = LONG_PTR(kBaseExtent) * bytesPerTexel;
        data.SlicePitch = data.RowPitch * kBaseExtent;
        Transition(commands, g_BaseNoise.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        const bool copied = UpdateSubresources(commands, g_BaseNoise.Get(), upload.Get(), 0, 0, 1, &data) != 0;
        Transition(commands, g_BaseNoise.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        return copied;
    }
} // 名前空間

void CloudRenderer::SetCustomBaseNoise(const vector<float>& density, int extent, const string& label)
{
    if (extent <= 0 || density.size() != size_t(extent) * extent * extent)
    {
        return;
    }
    ProcStudio::Volume volume;
    volume.Extent = extent;
    volume.Density = density;
    g_CustomNoise = ProceduralIO::Resample(volume, kBaseExtent).Density;
    g_CustomLabel = label;
    g_CustomActive = true;
    g_CustomPending = true;
}

void CloudRenderer::ClearCustomBaseNoise()
{
    g_CustomNoise.clear();
    g_CustomLabel.clear();
    g_CustomActive = false;
    g_CustomPending = false;
    g_NoiseDirty = true; // 組み込みの形状ノイズへ戻す
}

bool CloudRenderer::HasCustomBaseNoise()
{
    return g_CustomActive;
}

bool CloudRenderer::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT increment)
{
    Shutdown();
    if (!device || !heap || !increment)
    {
        return false;
    }
    g_Device = device;
    g_Heap = heap;
    g_Increment = increment;
    // 雲の初期化に失敗しても、有効な null ボリュームにより遅延描画パイプラインを利用可能に保つ。
    D3D12_SHADER_RESOURCE_VIEW_DESC nullVolume{};
    nullVolume.Format = DXGI_FORMAT_R8_UNORM;
    nullVolume.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    nullVolume.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullVolume.Texture3D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &nullVolume, Cpu(kBaseSrv));
    device->CreateShaderResourceView(nullptr, &nullVolume, Cpu(kDetailSrv));

    // 後続の雲パイプラインやノイズ確保に失敗しても、ゼロ埋めの CBV は有効なままにする。
    const auto upload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(kConstantStride * kFrames);
    if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_Constants))))
    {
        return false;
    }
    const D3D12_RANGE noReads{0, 0};
    if (FAILED(g_Constants->Map(0, &noReads, reinterpret_cast<void**>(&g_Mapped))))
    {
        return false;
    }
    memset(g_Mapped, 0, kConstantStride * kFrames);

    CD3DX12_DESCRIPTOR_RANGE ranges[6];
    CD3DX12_ROOT_PARAMETER params[9];
    for (UINT i = 0; i < 6; ++i)
    {
        ranges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 30 + i);
        params[i].InitAsDescriptorTable(1, &ranges[i], D3D12_SHADER_VISIBILITY_PIXEL);
    }
    params[6].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    params[7].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    params[8].InitAsConstantBufferView(9, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC sampler(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    CD3DX12_ROOT_SIGNATURE_DESC drawDesc(9, params, 1, &sampler,
                                         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    if (!CreateRoot(drawDesc, g_DrawRoot))
    {
        return false;
    }
    CD3DX12_DESCRIPTOR_RANGE noiseRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    CD3DX12_ROOT_PARAMETER noiseParams[2];
    noiseParams[0].InitAsDescriptorTable(1, &noiseRange);
    noiseParams[1].InitAsConstants(2, 0);
    CD3DX12_ROOT_SIGNATURE_DESC noiseDesc(2, noiseParams);
    if (!CreateRoot(noiseDesc, g_NoiseRoot))
    {
        return false;
    }
    if (!LoadCompute(L"shader/hlsl/build/GenerateCloudBaseNoiseCS.cso", g_BasePso) ||
        !LoadCompute(L"shader/hlsl/build/GenerateCloudDetailNoiseCS.cso", g_DetailPso) ||
        !LoadGraphics(L"shader/hlsl/build/CloudRaymarchPS.cso", DXGI_FORMAT_R16G16B16A16_FLOAT, false, g_RaymarchPso) ||
        !LoadGraphics(L"shader/hlsl/build/CloudCompositePS.cso", DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                      g_CompositeHdrPso) ||
        !LoadGraphics(L"shader/hlsl/build/CloudCompositePS.cso", DXGI_FORMAT_R8G8B8A8_UNORM, true, g_CompositeLdrPso) ||
        !CreateNoise(128, kBaseSrv, kBaseUav, g_BaseNoise) || !CreateNoise(32, kDetailSrv, kDetailUav, g_DetailNoise))
    {
        WriteGraphicsLog("Cloud: initialization failed; rendering disabled.\n");
        return false;
    }
    g_BaseNoise->SetName(L"Periodic Cloud Base Noise");
    g_DetailNoise->SetName(L"Periodic Cloud Detail Noise");
    g_Available = true;
    return true;
}

void CloudRenderer::Shutdown()
{
    g_Available = false;
    if (g_Constants && g_Mapped)
    {
        g_Constants->Unmap(0, nullptr);
    }
    g_Mapped = nullptr;
    g_Frames = {};
    g_CustomUploads = {};
    g_CustomPending = g_CustomActive; // 再初期化後に再アップロードする
    g_Constants.Reset();
    g_BaseNoise.Reset();
    g_DetailNoise.Reset();
    g_RaymarchPso.Reset();
    g_CompositeHdrPso.Reset();
    g_CompositeLdrPso.Reset();
    g_BasePso.Reset();
    g_DetailPso.Reset();
    g_DrawRoot.Reset();
    g_NoiseRoot.Reset();
    g_Device.Reset();
    g_Heap.Reset();
    g_RtvStart = {};
    g_NoiseDirty = true;
    g_GenerationCount = 0;
    g_WindX = g_WindZ = 0;
}

void CloudRenderer::Resize(D3D12_CPU_DESCRIPTOR_HANDLE start, UINT increment)
{
    g_Frames = {};
    g_RtvStart = start;
    g_RtvIncrement = increment;
}

CloudSettings& CloudRenderer::GetSettings()
{
    return g_Settings;
}

void CloudRenderer::GenerateNoise(uint32_t seed)
{
    g_Settings.Seed = seed;
    g_NoiseDirty = true;
}

bool CloudRenderer::IsAvailable()
{
    return g_Available;
}

UINT CloudRenderer::GetNoiseGenerationCount()
{
    return g_GenerationCount;
}

ID3D12Resource* CloudRenderer::GetCloudResource(UINT frame)
{
    return g_Frames[frame % kFrames].Color.Get();
}

ID3D12Resource* CloudRenderer::GetBaseNoiseResource()
{
    return g_BaseNoise.Get();
}

void CloudRenderer::PrepareLighting(ID3D12GraphicsCommandList* commands, UINT frameIndex, UINT width, UINT height,
                                    float deltaTime)
{
    if (!g_Available || !commands)
    {
        return;
    }
    g_Settings.Validate();
    const UINT index = frameIndex % kFrames;
    const UINT cloudWidth = max(1u, static_cast<UINT>(ceilf(width * g_Settings.ResolutionScale)));
    const UINT cloudHeight = max(1u, static_cast<UINT>(ceilf(height * g_Settings.ResolutionScale)));
    ID3D12DescriptorHeap* heaps[] = {g_Heap.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    if (g_Settings.Enabled && (g_NoiseDirty || g_GeneratedSeed != g_Settings.Seed))
    {
        RenderProfiler::ScopedEvent profile("Cloud Noise Generation", commands);
        DispatchNoise(commands, g_BaseNoise.Get(), g_BasePso.Get(), kBaseUav, 128);
        DispatchNoise(commands, g_DetailNoise.Get(), g_DetailPso.Get(), kDetailUav, 32);
        g_GeneratedSeed = g_Settings.Seed;
        g_NoiseDirty = false;
        ++g_GenerationCount;
        g_CustomPending = g_CustomActive; // 組み込みディスパッチが独自ボリュームを上書きした
    }
    if (g_CustomActive && g_CustomPending)
    {
        RenderProfiler::ScopedEvent profile("Cloud Custom Noise Upload", commands);
        if (UploadCustomNoise(commands, index))
        {
            g_CustomPending = false;
        }
    }
    const double length = sqrt(static_cast<double>(g_Settings.WindX) * g_Settings.WindX +
                               static_cast<double>(g_Settings.WindZ) * g_Settings.WindZ);
    const double delta = isfinite(deltaTime) ? clamp(deltaTime, 0.0f, 1.0f) : 0;
    if (length > 0.000001)
    {
        g_WindX += g_Settings.WindX / length * g_Settings.WindSpeed * delta;
        g_WindZ += g_Settings.WindZ / length * g_Settings.WindSpeed * delta;
    }
    Constants constants{};
    float firstCoverage = 0;
    if (g_Settings.LayerEnabled)
    {
        firstCoverage = g_Settings.Coverage;
    }
    constants.Layer = {g_Settings.BottomHeight, g_Settings.TopHeight, firstCoverage, g_Settings.Density};
    constants.Shape = {g_Settings.BaseScale, g_Settings.DetailScale, g_Settings.DetailStrength, g_Settings.Extinction};
    constants.Lighting = {g_Settings.PhaseG, g_Settings.AmbientStrength, g_Settings.PowderStrength,
                          g_Settings.JitterStrength};
    auto wrapped = [](double offset) {
        return static_cast<float>(offset - floor(offset));
    };
    constants.Wind = {wrapped(g_WindX * g_Settings.BaseScale), wrapped(g_WindZ * g_Settings.BaseScale),
                      wrapped(g_WindX * g_Settings.DetailScale * 1.15),
                      wrapped(g_WindZ * g_Settings.DetailScale * 1.15)};
    for (int index = 1; index < g_Settings.LayerCount; ++index)
    {
        const auto& layer = g_Settings.Layer(index);
        if (!layer.LayerEnabled)
        {
            continue;
        }
        auto& target = constants.AdditionalLayers[index - 1];
        target.Layer = {layer.BottomHeight, layer.TopHeight, layer.Coverage, layer.Density};
        target.Shape = {layer.BaseScale, layer.DetailScale, layer.DetailStrength, 0};
        target.Wind = {wrapped(g_WindX * layer.BaseScale), wrapped(g_WindZ * layer.BaseScale),
                       wrapped(g_WindX * layer.DetailScale * 1.15), wrapped(g_WindZ * layer.DetailScale * 1.15)};
    }
    constants.Quality = {static_cast<UINT>(g_Settings.ViewSteps), static_cast<UINT>(g_Settings.LightSteps),
                         static_cast<UINT>(g_Settings.Debug), static_cast<UINT>(g_Settings.LayerCount)};
    constants.Extent = {static_cast<float>(width), static_cast<float>(height), static_cast<float>(cloudWidth),
                        static_cast<float>(cloudHeight)};
    constants.Trace = {g_Settings.MaxTraceDistance, g_Settings.NoiseSlice, static_cast<float>(g_Settings.Enabled),
                       static_cast<float>(g_Settings.CastShadows)};
    memcpy(g_Mapped + index * kConstantStride, &constants, sizeof(constants));
}

void CloudRenderer::BindLighting(ID3D12GraphicsCommandList* commands, UINT frameIndex)
{
    if (!commands || !g_Heap)
    {
        return;
    }
    commands->SetGraphicsRootDescriptorTable(20, Gpu(kBaseSrv));
    commands->SetGraphicsRootDescriptorTable(21, Gpu(kDetailSrv));
    if (g_Constants)
    {
        commands->SetGraphicsRootConstantBufferView(22, g_Constants->GetGPUVirtualAddress() +
                                                            (frameIndex % kFrames) * kConstantStride);
    }
}

void CloudRenderer::BindForwardLighting(ID3D12GraphicsCommandList* commands, UINT frameIndex)
{
    if (!commands || !g_Heap || !g_Constants)
    {
        return;
    }
    commands->SetGraphicsRootDescriptorTable(24, Gpu(kBaseSrv));
    commands->SetGraphicsRootDescriptorTable(25, Gpu(kDetailSrv));
    commands->SetGraphicsRootConstantBufferView(27, g_Constants->GetGPUVirtualAddress() +
                                                        (frameIndex % kFrames) * kConstantStride);
}

void CloudRenderer::Render(ID3D12GraphicsCommandList* commands, UINT frameIndex, UINT width, UINT height,
                           D3D12_GPU_DESCRIPTOR_HANDLE depth, D3D12_GPU_DESCRIPTOR_HANDLE material,
                           D3D12_GPU_VIRTUAL_ADDRESS camera, D3D12_GPU_VIRTUAL_ADDRESS light,
                           D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv, DXGI_FORMAT sceneFormat)
{
    if (!g_Available || !g_Settings.Enabled || !commands || !width || !height || !camera || !light || !g_RtvStart.ptr)
    {
        return;
    }
    if (sceneFormat != DXGI_FORMAT_R16G16B16A16_FLOAT && sceneFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        return;
    }
    const UINT index = frameIndex % kFrames;
    const UINT cloudWidth = max(1u, static_cast<UINT>(ceilf(width * g_Settings.ResolutionScale)));
    const UINT cloudHeight = max(1u, static_cast<UINT>(ceilf(height * g_Settings.ResolutionScale)));
    if (!EnsureFrame(index, cloudWidth, cloudHeight))
    {
        return;
    }
    RenderProfiler::ScopedEvent total("Cloud Total", commands);
    auto& frame = g_Frames[index];
    commands->SetGraphicsRootSignature(g_DrawRoot.Get());
    commands->SetGraphicsRootDescriptorTable(0, depth);
    commands->SetGraphicsRootDescriptorTable(1, Gpu(kBaseSrv));
    commands->SetGraphicsRootDescriptorTable(2, Gpu(kDetailSrv));
    commands->SetGraphicsRootDescriptorTable(3, Gpu(kFrameSrv + index * 2));
    commands->SetGraphicsRootDescriptorTable(4, Gpu(kFrameSrv + index * 2 + 1));
    commands->SetGraphicsRootDescriptorTable(5, material);
    commands->SetGraphicsRootConstantBufferView(6, camera);
    commands->SetGraphicsRootConstantBufferView(7, light);
    commands->SetGraphicsRootConstantBufferView(8, g_Constants->GetGPUVirtualAddress() + index * kConstantStride);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    {
        RenderProfiler::ScopedEvent profile("Cloud Raymarch", commands);
        Transition(commands, frame.Color.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(commands, frame.Depth.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {Rtv(index, 0), Rtv(index, 1)};
        commands->OMSetRenderTargets(2, targets, FALSE, nullptr);
        const CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(cloudWidth), static_cast<float>(cloudHeight));
        const CD3DX12_RECT scissor(0, 0, cloudWidth, cloudHeight);
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->SetPipelineState(g_RaymarchPso.Get());
        commands->DrawInstanced(3, 1, 0, 0);
        Transition(commands, frame.Color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
        Transition(commands, frame.Depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    }
    {
        RenderProfiler::ScopedEvent profile("Cloud Composite", commands);
        commands->OMSetRenderTargets(1, &sceneRtv, FALSE, nullptr);
        const CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        const CD3DX12_RECT scissor(0, 0, width, height);
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->SetPipelineState(sceneFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? g_CompositeHdrPso.Get()
                                                                                 : g_CompositeLdrPso.Get());
        commands->DrawInstanced(3, 1, 0, 0);
    }
}

namespace
{
    // 雲ウィンドウ用のテクスチャ・フラクタル駆動ベースノイズ生成。
    // 生成はワーカースレッドで行い、結果は PrepareLighting でアップロードする。
    struct CustomNoiseJob
    {
        future<ProcStudio::Volume> Result;
        string Label;
        bool Running = false;
    };

    CustomNoiseJob g_NoiseJob;
    string g_NoiseStatus;

    void StartNoiseJob(function<ProcStudio::Volume()> work, string label)
    {
        g_NoiseJob.Result = async(launch::async, std::move(work));
        g_NoiseJob.Label = std::move(label);
        g_NoiseJob.Running = true;
        g_NoiseStatus = "生成中: " + g_NoiseJob.Label;
    }

    void DrawCustomNoiseUI()
    {
        static filesystem::path source;
        static int octaves = 3;
        static float variation = 1.0f;
        static int extent = 1; // 0 = 64, 1 = 128
        static ProcStudio::TextureParams fractal = [] {
            ProcStudio::TextureParams params;
            params.Kind = ProcStudio::TextureKind::FractalCloud;
            params.Noise.Frequency = 4;
            params.Coverage = 0.55f;
            params.Softness = 0.6f;
            return params;
        }();
        if (g_NoiseJob.Running && g_NoiseJob.Result.wait_for(chrono::seconds(0)) == future_status::ready)
        {
            g_NoiseJob.Running = false;
            const auto volume = g_NoiseJob.Result.get();
            if (volume.Extent > 0)
            {
                CloudRenderer::SetCustomBaseNoise(volume.Density, volume.Extent, g_NoiseJob.Label);
                g_NoiseStatus = "適用済み: " + g_NoiseJob.Label;
            }
            else
            {
                g_NoiseStatus = "生成に失敗しました: " + g_NoiseJob.Label;
            }
        }
        ImGui::TextWrapped("現在: %s", g_CustomActive ? g_CustomLabel.c_str() : "組み込みノイズ (Perlin-Worley)");
        if (!g_NoiseStatus.empty())
        {
            ImGui::TextDisabled("%s", g_NoiseStatus.c_str());
        }
        ImGui::Combo("生成解像度", &extent, "64^3 (高速)\000128^3\0");
        const int size = extent == 0 ? 64 : 128;
        ImGui::BeginDisabled(g_NoiseJob.Running);
        ImGui::SeparatorText("テクスチャから生成");
        ImGui::TextWrapped("2D 画像はオクターブ重ねで 3D の周期ノイズに変換します。3D DDS はそのまま使用します。");
        ImGui::SliderInt("オクターブ", &octaves, 1, 6);
        ImGui::SliderFloat("高さ方向の変化", &variation, 0, 4);
        if (ImGui::Button("テクスチャを指定して生成..."))
        {
            filesystem::path path;
            if (ProceduralIO::OpenFileDialog(
                    L"画像 / ボリューム (*.png;*.jpg;*.dds;*.tga;*.bmp;*.hdr;*.tif)\0*.png;*.jpg;*.jpeg;*.dds;*.tga;*.bmp;*.hdr;*.tif;*.tiff\0すべてのファイル\0*.*\0",
                    path))
            {
                source = path;
                const int o = octaves;
                const float v = variation;
                const uint32_t seed = g_Settings.Seed;
                StartNoiseJob(
                    [path, o, v, size, seed] {
                        string error;
                        ProcStudio::Volume volume;
                        if (ProceduralIO::LoadVolume(path, volume, error))
                        {
                            return volume;
                        }
                        ProcStudio::Image image;
                        if (!ProceduralIO::LoadImageFile(path, image, error))
                        {
                            return ProcStudio::Volume{};
                        }
                        return ProcStudio::VolumeFromImage(image, size, o, v, seed);
                    },
                    ProceduralIO::ToUtf8(path.filename()));
            }
        }
        if (!source.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button("同じテクスチャで再生成"))
            {
                const auto path = source;
                const int o = octaves;
                const float v = variation;
                const uint32_t seed = g_Settings.Seed;
                StartNoiseJob(
                    [path, o, v, size, seed] {
                        string error;
                        ProcStudio::Volume volume;
                        if (ProceduralIO::LoadVolume(path, volume, error))
                        {
                            return volume;
                        }
                        ProcStudio::Image image;
                        return ProceduralIO::LoadImageFile(path, image, error)
                                   ? ProcStudio::VolumeFromImage(image, size, o, v, seed)
                                   : ProcStudio::Volume{};
                    },
                    ProceduralIO::ToUtf8(path.filename()));
            }
        }
        ImGui::SeparatorText("フラクタルノイズから生成");
        int basis = static_cast<int>(fractal.Noise.BasisType);
        if (ImGui::Combo("基底", &basis, "Perlin\0Value\0Worley\0Cell Edge\0"))
        {
            fractal.Noise.BasisType = static_cast<ProcStudio::Basis>(basis);
        }
        ImGui::InputScalar("Seed##fractal", ImGuiDataType_U32, &fractal.Noise.Seed);
        ImGui::SliderFloat("周波数", &fractal.Noise.Frequency, 1, 16, "%.0f");
        ImGui::SliderInt("オクターブ##fractal", &fractal.Noise.Octaves, 1, 8);
        ImGui::SliderFloat("Gain", &fractal.Noise.Gain, 0.2f, 0.8f);
        ImGui::SliderFloat("被覆率", &fractal.Coverage, 0, 1);
        ImGui::SliderFloat("柔らかさ", &fractal.Softness, 0.05f, 1);
        if (ImGui::Button("フラクタル雲ノイズを生成して適用"))
        {
            const auto params = fractal;
            StartNoiseJob(
                [params, size] {
                    return ProcStudio::GenerateVolume(params, size);
                },
                "フラクタル雲ノイズ");
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!g_CustomActive);
        if (ImGui::Button("組み込みノイズに戻す"))
        {
            CloudRenderer::ClearCustomBaseNoise();
            g_NoiseStatus.clear();
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("プロシージャルスタジオの「フラクタル雲」からも適用できます。");
    }
} // 名前空間

void CloudRenderer::DrawDebugUI()
{
    ImGui::Checkbox("Enable clouds", &g_Settings.Enabled);
    ImGui::Checkbox("Cloud shadows", &g_Settings.CastShadows);
    if (!g_Available)
    {
        ImGui::TextUnformatted("Cloud pipelines unavailable");
    }
    if (ImGui::TreeNodeEx("Layers", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int removeIndex = -1;
        for (int index = 0; index < g_Settings.LayerCount; ++index)
        {
            auto& layer = g_Settings.Layer(index);
            ImGui::PushID(index);
            if (ImGui::TreeNodeEx("Layer", ImGuiTreeNodeFlags_DefaultOpen, "Layer %d", index + 1))
            {
                ImGui::Checkbox("Enabled", &layer.LayerEnabled);
                ImGui::DragFloat("Bottom Height", &layer.BottomHeight, 0.5f);
                ImGui::DragFloat("Top Height", &layer.TopHeight, 0.5f);
                ImGui::SliderFloat("Coverage", &layer.Coverage, 0, 1);
                ImGui::SliderFloat("Density", &layer.Density, 0, 10);
                ImGui::DragFloat("Base Scale", &layer.BaseScale, 0.0001f, 0.00001f, 1, "%.5f");
                ImGui::DragFloat("Detail Scale", &layer.DetailScale, 0.0001f, 0.00001f, 4, "%.5f");
                ImGui::SliderFloat("Detail Strength", &layer.DetailStrength, 0, 1);
                ImGui::BeginDisabled(g_Settings.LayerCount <= 1);
                if (ImGui::Button("Remove layer"))
                {
                    removeIndex = index;
                }
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeIndex >= 0)
        {
            g_Settings.RemoveLayer(removeIndex);
        }
        ImGui::BeginDisabled(g_Settings.LayerCount >= CloudSettings::MaxLayers);
        if (ImGui::Button("Add layer"))
        {
            g_Settings.AddLayer();
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("Height sets thickness; density sets transparency. Wind and lighting are shared.");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Wind"))
    {
        ImGui::SliderFloat("Direction X", &g_Settings.WindX, -1, 1);
        ImGui::SliderFloat("Direction Z", &g_Settings.WindZ, -1, 1);
        ImGui::DragFloat("Speed", &g_Settings.WindSpeed, 0.1f, 0, 1000);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Lighting"))
    {
        ImGui::DragFloat("Extinction", &g_Settings.Extinction, 0.001f, 0, 10, "%.4f");
        ImGui::SliderFloat("Phase G", &g_Settings.PhaseG, -0.9f, 0.9f);
        ImGui::SliderFloat("Ambient Strength", &g_Settings.AmbientStrength, 0, 10);
        ImGui::SliderFloat("Powder Strength", &g_Settings.PowderStrength, 0, 2);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Quality"))
    {
        if (ImGui::Button("Low"))
        {
            g_Settings.ApplyQuality(CloudQuality::Low);
        }
        ImGui::SameLine();
        if (ImGui::Button("Medium"))
        {
            g_Settings.ApplyQuality(CloudQuality::Medium);
        }
        ImGui::SameLine();
        if (ImGui::Button("High"))
        {
            g_Settings.ApplyQuality(CloudQuality::High);
        }
        ImGui::SliderInt("View Steps", &g_Settings.ViewSteps, 1, 192);
        ImGui::SliderInt("Light Steps", &g_Settings.LightSteps, 1, 16);
        ImGui::DragFloat("Max Trace Distance", &g_Settings.MaxTraceDistance, 10, 0.1f, 100000);
        ImGui::SliderFloat("Resolution Scale", &g_Settings.ResolutionScale, 0.25f, 1);
        ImGui::SliderFloat("Jitter Strength", &g_Settings.JitterStrength, 0, 1);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Noise"))
    {
        ImGui::InputScalar("Seed", ImGuiDataType_U32, &g_Settings.Seed);
        if (ImGui::Button("Regenerate"))
        {
            GenerateNoise(g_Settings.Seed);
        }
        ImGui::SliderFloat("Noise Slice", &g_Settings.NoiseSlice, 0, 1);
        ImGui::Text("Generations: %u", g_GenerationCount);
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("ノイズテクスチャ (形状ノイズの差し替え)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawCustomNoiseUI();
        ImGui::TreePop();
    }
    int debug = static_cast<int>(g_Settings.Debug);
    if (ImGui::Combo(
            "Debug View", &debug,
            "Normal\0Density\0Transmittance\0Lighting\0Cloud Depth\0Step Count\0Base Noise Slice\0Detail Noise Slice\0"))
    {
        g_Settings.Debug = static_cast<CloudDebug>(debug);
    }
    int preset = -1;
    if (ImGui::Combo("Layer 1 preset", &preset, "Clear\0Fair\0Cloudy\0Overcast\0Storm\0"))
    {
        g_Settings.ApplyPreset(static_cast<CloudPreset>(preset));
    }
    const auto& timing = RenderProfiler::GetLatest();
    if (!timing.GpuTimingValid)
    {
        ImGui::TextUnformatted("GPU timing pending");
    }
    else
    {
        for (const auto& pass : timing.Passes)
        {
            if (pass.Name == "Cloud Raymarch" || pass.Name == "Cloud Composite" || pass.Name == "Cloud Total")
            {
                ImGui::Text("%s: %.3f ms", pass.Name.c_str(), pass.GpuMs);
            }
        }
    }
    g_Settings.Validate();
}
