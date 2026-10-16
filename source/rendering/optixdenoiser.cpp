#include "pch.h"
#include "optixdenoiser.h"
#include "optixinterop.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "rendererstate.h"

namespace
{
constexpr UINT kFrames = RendererState::g_kFRAME_COUNT;
constexpr UINT kDescriptors = 7;
ComPtr<ID3D12Device> g_Device;
ComPtr<ID3D12RootSignature> g_Root;
ComPtr<ID3D12PipelineState> g_Pack, g_Unpack;
UINT g_Increment = 0;
bool g_RuntimeFailed = false;

struct Constants
{
    XMFLOAT4X4 WorldToView{};
    XMUINT4 ExtentFlags{};
    XMFLOAT4 Parameters{};
};
static_assert(sizeof(Constants) <= 256);

void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
    commands->ResourceBarrier(1, &barrier);
}
}

struct OptixDenoiser::Impl
{
    OptixInterop Interop;
    array<ComPtr<ID3D12DescriptorHeap>, kFrames> Heaps;
    ComPtr<ID3D12Resource> ConstantBuffer;

    bool Ensure(UINT slot)
    {
        if (!ConstantBuffer)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(256 * kFrames);
            if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ConstantBuffer))))
            {
                return false;
            }
        }
        if (!Heaps[slot])
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = kDescriptors;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&Heaps[slot]))))
            {
                return false;
            }
        }
        return true;
    }
};

OptixDenoiser::OptixDenoiser() : m_Impl(make_unique<Impl>()) {}
OptixDenoiser::~OptixDenoiser() = default;

bool OptixDenoiser::Initialize(ID3D12Device* device)
{
    g_RuntimeFailed = false;
    if (!OptixInterop::Initialize(device))
    {
        return false;
    }
    g_Device = device;
    g_Increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_DESCRIPTOR_RANGE ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);
    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsDescriptorTable(1, &ranges[0]);
    params[2].InitAsDescriptorTable(1, &ranges[1]);
    const CD3DX12_ROOT_SIGNATURE_DESC root(3, params);
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_Root))))
    {
        return false;
    }
    const auto compile = [&](const char* entry, ComPtr<ID3D12PipelineState>& pipeline)
    {
        blob.Reset();
        errors.Reset();
        if (FAILED(D3DCompileFromFile(L"shader/hlsl/OptixDenoiserCS.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry, "cs_6_5", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
        {
            WriteGraphicsLog("OptiX packing shader failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : entry);
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g_Root.Get();
        desc.CS = CD3DX12_SHADER_BYTECODE(blob.Get());
        return SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    };
    return compile("Pack", g_Pack) && compile("Unpack", g_Unpack);
}

void OptixDenoiser::Shutdown()
{
    g_Pack.Reset();
    g_Unpack.Reset();
    g_Root.Reset();
    g_Device.Reset();
    OptixInterop::Shutdown();
}
bool OptixDenoiser::IsAvailable()
{
    return !g_RuntimeFailed && OptixInterop::IsAvailable() && g_Pack && g_Unpack;
}
const char* OptixDenoiser::GetStatus()
{
    if (g_RuntimeFailed)
    {
        return "OptiX stopped after a runtime failure; NRD fallback is active (see graphics log)";
    }
    if (OptixInterop::IsAvailable() && (!g_Pack || !g_Unpack))
    {
        return "OptiX packing pipeline unavailable; NRD fallback is active";
    }
    return OptixInterop::GetStatus();
}

bool OptixDenoiser::Execute(ID3D12GraphicsCommandList* commands, ID3D12Resource* noisy,
    ID3D12Resource* depth, ID3D12Resource* normal, ID3D12Resource* result,
    const NrdFrame& frame, bool nrdPacked, bool preserveHitDistance, float intensity)
{
    if (!IsAvailable() || !commands || !noisy || !depth || !normal || !result)
    {
        return false;
    }
    const UINT slot = frame.FrameIndex % kFrames;
    const auto extent = noisy->GetDesc();
    const UINT width = static_cast<UINT>(extent.Width);
    const UINT height = extent.Height;
    if (!m_Impl->Ensure(slot) || !m_Impl->Interop.Ensure(width, height, slot))
    {
        g_RuntimeFailed = true;
        WriteGraphicsLog("OptiX resources unavailable; falling back to NRD: %s\n", OptixInterop::GetStatus());
        return false;
    }
    auto* shared = m_Impl->Interop.GetSharedBuffer(slot);
    auto* heap = m_Impl->Heaps[slot].Get();
    const auto cpu = [&](UINT index) { return CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(), index, g_Increment); };
    const auto gpu = [&](UINT index) { return CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), index, g_Increment); };
    array<ID3D12Resource*, 3> textures{noisy, depth, normal};
    for (UINT index = 0; index < textures.size(); ++index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = textures[index]->GetDesc().Format;
        if (srv.Format == DXGI_FORMAT_R32_TYPELESS)
        {
            srv.Format = DXGI_FORMAT_R32_FLOAT;
        }
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_Device->CreateShaderResourceView(textures[index], &srv, cpu(index));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC bufferSrv{};
    bufferSrv.Format = DXGI_FORMAT_UNKNOWN;
    bufferSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    bufferSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bufferSrv.Buffer.NumElements = width * height * 4;
    bufferSrv.Buffer.StructureByteStride = 16;
    g_Device->CreateShaderResourceView(shared, &bufferSrv, cpu(3));
    D3D12_UNORDERED_ACCESS_VIEW_DESC bufferUav{};
    bufferUav.Format = DXGI_FORMAT_UNKNOWN;
    bufferUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    bufferUav.Buffer.NumElements = width * height * 4;
    bufferUav.Buffer.StructureByteStride = 16;
    g_Device->CreateUnorderedAccessView(shared, nullptr, &bufferUav, cpu(4));
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav{};
    outputUav.Format = result->GetDesc().Format;
    outputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_Device->CreateUnorderedAccessView(result, nullptr, &outputUav, cpu(5));

    Constants constants;
    XMStoreFloat4x4(&constants.WorldToView, XMMatrixTranspose(XMLoadFloat4x4(&frame.View)));
    constants.ExtentFlags = {width, height, static_cast<UINT>(nrdPacked), static_cast<UINT>(preserveHitDistance)};
    constants.Parameters.x = intensity;
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(m_Impl->ConstantBuffer->Map(0, &noRead, &mapped)))
    {
        return false;
    }
    memcpy(static_cast<uint8_t*>(mapped) + 256 * slot, &constants, sizeof(constants));
    m_Impl->ConstantBuffer->Unmap(0, nullptr);
    const auto bind = [&](ID3D12PipelineState* pipeline)
    {
        commands->SetDescriptorHeaps(1, &heap);
        commands->SetComputeRootSignature(g_Root.Get());
        commands->SetPipelineState(pipeline);
        commands->SetComputeRootConstantBufferView(0, m_Impl->ConstantBuffer->GetGPUVirtualAddress() + 256 * slot);
        commands->SetComputeRootDescriptorTable(1, gpu(0));
        commands->SetComputeRootDescriptorTable(2, gpu(4));
    };
    Transition(commands, noisy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commands, shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bind(g_Pack.Get());
    commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    Transition(commands, shared, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    GraphicsDevice::SubmitForExternalCompute();
    const bool denoised = m_Impl->Interop.Enqueue(GraphicsDevice::GetCommandQueue(), slot);
    if (!denoised)
    {
        g_RuntimeFailed = true;
        WriteGraphicsLog("OptiX runtime failure; falling back to NRD: %s\n", OptixInterop::GetStatus());
    }
    if (denoised)
    {
        Transition(commands, shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commands, result, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bind(g_Unpack.Get());
        commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        Transition(commands, result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commands, shared, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    Transition(commands, noisy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return denoised;
}

