#include "pch.h"
#include "memoryallocator.h"
#include "spatialupscaler.h"
#include "rendererstate.h"
#include "renderprofiler.h"

#include <cstddef>
#include <cmath>
#include <algorithm>

#define A_CPU 1
#include "../../External/FidelityFX-FSR/ffx_a.h"
#include "../../External/FidelityFX-FSR/ffx_fsr1.h"
#include "../../External/NVIDIAImageScaling/NIS_Config.h"

static ComPtr<ID3D12Device> g_Device;
static ComPtr<ID3D12DescriptorHeap> g_DescriptorHeap;
static UINT g_DescriptorIncrement = 0;
static ComPtr<ID3D12RootSignature> g_RootSignature;
static ComPtr<ID3D12PipelineState> g_FsrEasuPso;
static ComPtr<ID3D12PipelineState> g_FsrRcasPso;
static ComPtr<ID3D12PipelineState> g_NisPso;
static ComPtr<ID3D12Resource> g_FsrScratch;
static ComPtr<ID3D12Resource> g_NisScaleCoefficients;
static ComPtr<ID3D12Resource> g_NisUsmCoefficients;
static UINT g_OutputWidth = 0;
static UINT g_OutputHeight = 0;
DXGI_FORMAT g_OutputFormat = DXGI_FORMAT_UNKNOWN;

struct FsrConstants
{
    uint32_t Const0[4]{};
    uint32_t Const1[4]{};
    uint32_t Const2[4]{};
    uint32_t Const3[4]{};
    uint32_t DispatchInfo[4]{};
};

static_assert(sizeof(FsrConstants) == sizeof(uint32_t) * 20);

constexpr UINT kNisConfigDwordCount =
    static_cast<UINT>((offsetof(NISConfig, reserved1) + sizeof(NISConfig::reserved1)) / sizeof(uint32_t));
static_assert(kNisConfigDwordCount == 28);
static_assert(kNisConfigDwordCount <= 32);

D3D12_CPU_DESCRIPTOR_HANDLE SpatialUpscaler::CpuHandle(UINT index)
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_DescriptorHeap->GetCPUDescriptorHandleForHeapStart(), index,
                                         g_DescriptorIncrement);
}

D3D12_GPU_DESCRIPTOR_HANDLE SpatialUpscaler::GpuHandle(UINT index)
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(g_DescriptorHeap->GetGPUDescriptorHandleForHeapStart(), index,
                                         g_DescriptorIncrement);
}

bool SpatialUpscaler::CreateRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE ranges[4];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

    CD3DX12_ROOT_PARAMETER parameters[5];
    for (UINT i = 0; i < 4; ++i)
    {
        parameters[i].InitAsDescriptorTable(1, &ranges[i], D3D12_SHADER_VISIBILITY_ALL);
    }
    parameters[4].InitAsConstants(32, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CD3DX12_ROOT_SIGNATURE_DESC desc(_countof(parameters), parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr))
    {
        if (errors)
        {
            Debug::Log("Spatial upscaler root signature: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }
    return SUCCEEDED(g_Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                   IID_PPV_ARGS(&g_RootSignature)));
}

bool SpatialUpscaler::CreatePipeline(const wchar_t* path, ComPtr<ID3D12PipelineState>& output)
{
    ComPtr<ID3DBlob> shader;
    HRESULT hr = D3DReadFileToBlob(path, &shader);
    if (FAILED(hr))
    {
        Debug::Log("Spatial upscaler shader is missing (HRESULT 0x%08X).\n", hr);
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = g_RootSignature.Get();
    desc.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
    hr = g_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&output));
    return SUCCEEDED(hr);
}

bool SpatialUpscaler::CreateNisCoefficientBuffers()
{
    auto createBuffer = [](const float* coefficients, ComPtr<ID3D12Resource>& resource, UINT descriptorIndex) {
        const UINT elementCount = static_cast<UINT>(kPhaseCount * 2);
        const UINT byteSize = elementCount * sizeof(float) * 4;
        auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
        HRESULT hr =
            g_Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
        if (FAILED(hr))
        {
            return false;
        }

        void* mapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        if (FAILED(resource->Map(0, &readRange, &mapped)))
        {
            return false;
        }
        memcpy(mapped, coefficients, byteSize);
        resource->Unmap(0, nullptr);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = byteSize / sizeof(uint32_t);
        srv.Buffer.StructureByteStride = 0;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        g_Device->CreateShaderResourceView(resource.Get(), &srv, SpatialUpscaler::CpuHandle(descriptorIndex));
        return true;
    };

    return createBuffer(&coef_scale[0][0], g_NisScaleCoefficients, RendererState::g_kNIS_COEF_SCALE_SRV_INDEX) &&
           createBuffer(&coef_usm[0][0], g_NisUsmCoefficients, RendererState::g_kNIS_COEF_USM_SRV_INDEX);
}

bool SpatialUpscaler::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* descriptorHeap, UINT descriptorIncrement)
{
    Shutdown();
    if (!device || !descriptorHeap || descriptorIncrement == 0)
    {
        return false;
    }
    g_Device = device;
    g_DescriptorHeap = descriptorHeap;
    g_DescriptorIncrement = descriptorIncrement;
    if (!CreateRootSignature())
    {
        return false;
    }

    const bool fsrReady = CreatePipeline(L"shader/hlsl/build/FsrEasuCS.cso", g_FsrEasuPso) &&
                          CreatePipeline(L"shader/hlsl/build/FsrRcasCS.cso", g_FsrRcasPso);
    const bool nisReady =
        CreatePipeline(L"shader/hlsl/build/NisScalerCS.cso", g_NisPso) && CreateNisCoefficientBuffers();
    if (!fsrReady)
    {
        Debug::Log("FidelityFX FSR 1 is unavailable; bilateral fallback remains active.\n");
    }
    if (!nisReady)
    {
        Debug::Log("NVIDIA Image Scaling is unavailable; bilateral fallback remains active.\n");
    }
    return fsrReady || nisReady;
}

bool SpatialUpscaler::Resize(UINT outputWidth, UINT outputHeight, DXGI_FORMAT format)
{
    if (!g_Device || outputWidth == 0 || outputHeight == 0)
    {
        return false;
    }
    if (g_FsrScratch && g_OutputWidth == outputWidth && g_OutputHeight == outputHeight && g_OutputFormat == format)
    {
        return true;
    }
    g_FsrScratch.Reset();
    g_OutputWidth = outputWidth;
    g_OutputHeight = outputHeight;
    g_OutputFormat = format;

    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, outputWidth, outputHeight, 1, 1, 1, 0,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // Through D3D12MA when the flag is on; committed fallback otherwise.
    HRESULT hr = E_FAIL;
    if (ID3D12Resource* allocated = MemoryAllocator::CreateResourceAutoLifetime(
            desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr))
    {
        g_FsrScratch.Attach(allocated);
        hr = S_OK;
    }
    else
    {
        auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        hr = g_Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                               IID_PPV_ARGS(&g_FsrScratch));
    }
    if (FAILED(hr))
    {
        return false;
    }
    g_FsrScratch->SetName(L"FSR EASU Output");

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    g_Device->CreateShaderResourceView(g_FsrScratch.Get(), &srv, CpuHandle(RendererState::g_kFSR_SCRATCH_SRV_INDEX));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_Device->CreateUnorderedAccessView(g_FsrScratch.Get(), nullptr, &uav,
                                        CpuHandle(RendererState::g_kFSR_SCRATCH_UAV_INDEX));
    return true;
}

void SpatialUpscaler::Shutdown()
{
    g_FsrScratch.Reset();
    g_NisScaleCoefficients.Reset();
    g_NisUsmCoefficients.Reset();
    g_FsrEasuPso.Reset();
    g_FsrRcasPso.Reset();
    g_NisPso.Reset();
    g_RootSignature.Reset();
    g_DescriptorHeap.Reset();
    g_Device.Reset();
    g_DescriptorIncrement = 0;
    g_OutputWidth = 0;
    g_OutputHeight = 0;
    g_OutputFormat = DXGI_FORMAT_UNKNOWN;
}

bool SpatialUpscaler::IsAvailable(UpscaleMode mode)
{
    switch (mode)
    {
        case UpscaleMode::Fsr1:
            return g_FsrEasuPso && g_FsrRcasPso && g_FsrScratch;
        case UpscaleMode::Nis:
            return g_NisPso && g_NisScaleCoefficients && g_NisUsmCoefficients;
        default:
            return true;
    }
}

bool SpatialUpscaler::IsScaleSupported(UpscaleMode mode, UINT inputWidth, UINT inputHeight, UINT outputWidth,
                                       UINT outputHeight)
{
    if (inputWidth == 0 || inputHeight == 0 || outputWidth == 0 || outputHeight == 0)
    {
        return false;
    }
    if (mode != UpscaleMode::Nis)
    {
        return true;
    }

    // NIS 1 段は最大 2 倍。それ以上（内部解像度 50% 未満）は 2 倍以下の段を重ねる（最大 3 段 = 8 倍 → 20% まで対応）
    return inputWidth <= outputWidth && inputHeight <= outputHeight &&
           NisPassCount(inputWidth, inputHeight, outputWidth, outputHeight) <= 3;
}

UINT SpatialUpscaler::NisPassCount(UINT inputWidth, UINT inputHeight, UINT outputWidth, UINT outputHeight)
{
    if (inputWidth == 0 || inputHeight == 0)
    {
        return 0;
    }
    const double ratio =
        (std::max)(static_cast<double>(outputWidth) / inputWidth, static_cast<double>(outputHeight) / inputHeight);
    if (ratio <= 2.0)
    {
        return 1;
    }
    return static_cast<UINT>(ceil(log2(ratio) - 1.0e-6));
}

bool SpatialUpscaler::Execute(ID3D12GraphicsCommandList* commandList, D3D12_GPU_DESCRIPTOR_HANDLE inputSrv,
                              ID3D12Resource* output, D3D12_GPU_DESCRIPTOR_HANDLE outputUav, UINT inputWidth,
                              UINT inputHeight, UINT outputWidth, UINT outputHeight, UpscaleMode mode,
                              ID3D12Resource* input, D3D12_GPU_DESCRIPTOR_HANDLE outputSrv)
{
    // 入力テクスチャの実サイズ（ビューポートより大きいことがある。正しい UV 計算に必要）
    UINT inputTextureWidth = inputWidth, inputTextureHeight = inputHeight;
    if (input)
    {
        const auto inputDesc = input->GetDesc();
        inputTextureWidth = (std::max)(static_cast<UINT>(inputDesc.Width), inputWidth);
        inputTextureHeight = (std::max)(inputDesc.Height, inputHeight);
    }
    const auto outputDesc = output ? output->GetDesc() : D3D12_RESOURCE_DESC{};
    const UINT outputTextureWidth = (std::max)(static_cast<UINT>(outputDesc.Width), outputWidth);
    const UINT outputTextureHeight = (std::max)(outputDesc.Height, outputHeight);
    if (!commandList || !output || !g_RootSignature || !IsAvailable(mode) ||
        !IsScaleSupported(mode, inputWidth, inputHeight, outputWidth, outputHeight))
    {
        return false;
    }
    ID3D12DescriptorHeap* heaps[] = {g_DescriptorHeap.Get()};
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootSignature(g_RootSignature.Get());

    if (mode == UpscaleMode::Fsr1)
    {
        FsrConstants constants{};
        FsrEasuCon(constants.Const0, constants.Const1, constants.Const2, constants.Const3,
                   static_cast<float>(inputWidth), static_cast<float>(inputHeight),
                   static_cast<float>(inputTextureWidth), static_cast<float>(inputTextureHeight),
                   static_cast<float>(outputWidth), static_cast<float>(outputHeight));
        constants.DispatchInfo[0] = outputWidth;
        constants.DispatchInfo[1] = outputHeight;

        D3D12_RESOURCE_BARRIER scratchToUav = CD3DX12_RESOURCE_BARRIER::Transition(
            g_FsrScratch.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &scratchToUav);
        commandList->SetPipelineState(g_FsrEasuPso.Get());
        commandList->SetComputeRootDescriptorTable(0, inputSrv);
        commandList->SetComputeRootDescriptorTable(1, GpuHandle(RendererState::g_kFSR_SCRATCH_UAV_INDEX));
        commandList->SetComputeRoot32BitConstants(4, 20, &constants, 0);
        commandList->Dispatch((outputWidth + 15u) / 16u, (outputHeight + 15u) / 16u, 1);

        D3D12_RESOURCE_BARRIER scratchToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            g_FsrScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &scratchToSrv);

        FsrConstants rcasConstants{};
        FsrRcasCon(rcasConstants.Const0, RendererSettings::GetFsrSharpness());
        rcasConstants.DispatchInfo[0] = outputWidth;
        rcasConstants.DispatchInfo[1] = outputHeight;
        commandList->SetPipelineState(g_FsrRcasPso.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(RendererState::g_kFSR_SCRATCH_SRV_INDEX));
        commandList->SetComputeRootDescriptorTable(1, outputUav);
        commandList->SetComputeRoot32BitConstants(4, 20, &rcasConstants, 0);
        commandList->Dispatch((outputWidth + 15u) / 16u, (outputHeight + 15u) / 16u, 1);
        return true;
    }

    if (mode == UpscaleMode::Nis)
    {
        // NIS は 1 段最大 2 倍。50% 未満は段を重ねる（各段の倍率を等しく = r^(1/n)。偏った 2 倍 +
        // 端数よりにじみが少ない）
        //   ・シャープネスは最後の段だけ（途中で掛けるとハロー・リンギングが段ごとに増幅される）
        //   ・低解像度ほど元の情報が少ないので、最後の段のシャープネスを少し強める（上限 1）
        //   ・中間は FSR のスクラッチ（出力サイズ）と出力そのものを交互に使う（ビューポートで範囲を指定）
        const UINT passes = NisPassCount(inputWidth, inputHeight, outputWidth, outputHeight);
        if (passes == 0 || passes > 3 || (passes == 3 && outputSrv.ptr == 0))
        {
            return false;
        }
        const float userSharpness = RendererSettings::GetNisSharpness();
        const double ratioX = static_cast<double>(outputWidth) / inputWidth;
        const double ratioY = static_cast<double>(outputHeight) / inputHeight;
        const float scaleBoost = static_cast<float>((std::min)(ratioX, ratioY));
        const float finalSharpness =
            (std::min)(1.0f, userSharpness * (passes > 1 ? 1.0f + 0.1f * (scaleBoost - 2.0f) : 1.0f));

        commandList->SetPipelineState(g_NisPso.Get());
        commandList->SetComputeRootDescriptorTable(2, GpuHandle(RendererState::g_kNIS_COEF_SCALE_SRV_INDEX));
        commandList->SetComputeRootDescriptorTable(3, GpuHandle(RendererState::g_kNIS_COEF_USM_SRV_INDEX));

        struct Surface
        {
            D3D12_GPU_DESCRIPTOR_HANDLE Srv, Uav;
            UINT TextureWidth, TextureHeight;
            ID3D12Resource* Resource;
        };

        const Surface source{inputSrv, {}, inputTextureWidth, inputTextureHeight, input};
        const Surface scratch{GpuHandle(RendererState::g_kFSR_SCRATCH_SRV_INDEX),
                              GpuHandle(RendererState::g_kFSR_SCRATCH_UAV_INDEX), g_OutputWidth, g_OutputHeight,
                              g_FsrScratch.Get()};
        const Surface target{outputSrv, outputUav, outputTextureWidth, outputTextureHeight, output};
        if (passes > 1 && (!g_FsrScratch || g_OutputWidth < outputWidth || g_OutputHeight < outputHeight))
        {
            return false;
        }
        // 最後が出力になるように逆算：3 段 = 出力 → スクラッチ → 出力、2 段 = スクラッチ → 出力
        const Surface* chain[4] = {&source, nullptr, nullptr, nullptr};
        if (passes == 2)
        {
            chain[1] = &scratch;
            chain[2] = &target;
        }
        else if (passes == 3)
        {
            chain[1] = &target;
            chain[2] = &scratch;
            chain[3] = &target;
        }
        else
        {
            chain[1] = &target;
        }

        D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES scratchState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto setState = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES target) {
            D3D12_RESOURCE_STATES* current = resource == output                             ? &outputState
                                             : (resource && resource == g_FsrScratch.Get()) ? &scratchState
                                                                                            : nullptr;
            if (!current)
            {
                return; // 入力は呼び出し側が管理
            }
            if (*current == target)
            {
                if (target == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                {
                    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(resource);
                    commandList->ResourceBarrier(1, &barrier);
                }
                return;
            }
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, *current, target);
            commandList->ResourceBarrier(1, &barrier);
            *current = target;
        };
        UINT currentWidth = inputWidth, currentHeight = inputHeight;
        for (UINT pass = 0; pass < passes; ++pass)
        {
            const bool last = pass + 1 == passes;
            const double t = static_cast<double>(pass + 1) / passes;
            const UINT nextWidth =
                last ? outputWidth : (std::min)(outputWidth, static_cast<UINT>(ceil(inputWidth * pow(ratioX, t))));
            const UINT nextHeight =
                last ? outputHeight : (std::min)(outputHeight, static_cast<UINT>(ceil(inputHeight * pow(ratioY, t))));
            const Surface& from = *chain[pass];
            const Surface& to = *chain[pass + 1];
            NISConfig config{};
            if (!NVScalerUpdateConfig(config, last ? finalSharpness : 0.0f, 0, 0, currentWidth, currentHeight,
                                      from.TextureWidth, from.TextureHeight, 0, 0, nextWidth, nextHeight,
                                      to.TextureWidth, to.TextureHeight, NISHDRMode::None))
            {
                return false;
            }
            // 状態：入力 = NON_PIXEL（呼び出し側）、出力 = UAV（呼び出し側）、スクラッチ = NON_PIXEL（FSR
            // と共通の前提）
            //   読む側を NON_PIXEL、書く側を UAV にそろえる
            setState(from.Resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            setState(to.Resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->SetComputeRootDescriptorTable(0, from.Srv);
            commandList->SetComputeRootDescriptorTable(1, to.Uav);
            commandList->SetComputeRoot32BitConstants(4, kNisConfigDwordCount, &config, 0);
            commandList->Dispatch((nextWidth + 31u) / 32u, (nextHeight + 23u) / 24u, 1);
            currentWidth = nextWidth;
            currentHeight = nextHeight;
        }
        // 呼び出し時の状態に戻す（出力 = UAV、スクラッチ = NON_PIXEL）
        setState(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        setState(g_FsrScratch.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }
    return false;
}
