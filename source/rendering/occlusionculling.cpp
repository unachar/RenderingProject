#include "pch.h"
#include "occlusionculling.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "graphicsdevice.h"
#include "renderersettings.h"
#include "rendererstate.h"
#include "rendergraph.h"
#include "renderprofiler.h"
#include "virtualgeometry/virtualgeometrytraversal.h"

static ComPtr<ID3D12Device> g_Device;
static ComPtr<ID3D12DescriptorHeap> g_Heap;
static UINT g_Increment = 0;
static ComPtr<ID3D12RootSignature> g_RootSignature;
static ComPtr<ID3D12PipelineState> g_ReducePso, g_CopyPso, g_DebugPso;
static ComPtr<ID3D12Resource> g_DebugTexture;
static UINT g_DebugMip = 0;
static bool g_DebugEnabled = false, g_DebugReady = false;
static ComPtr<ID3D12Resource> g_OccluderDepth;
static ComPtr<ID3D12DescriptorHeap> g_OccluderDsv;
static ComPtr<ID3D12Resource> g_HiZ[2];
static UINT g_Width = 0;
static UINT g_Height = 0;
static UINT g_MipCount = 0;
static UINT g_PreviousIndex = 0;
static UINT g_CurrentIndex = 1;
static UINT g_Phase = 0;
static bool g_PreviousValid = false;
static bool g_CurrentValid = false;

static D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index)
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Heap->GetCPUDescriptorHandleForHeapStart(), index, g_Increment);
}
static D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index)
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(g_Heap->GetGPUDescriptorHandleForHeapStart(), index, g_Increment);
}
static bool LoadPipeline(const wchar_t *path, ComPtr<ID3D12PipelineState> &output)
{
    ComPtr<ID3DBlob> shader;
    if (FAILED(D3DReadFileToBlob(path, &shader)))
    {
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = g_RootSignature.Get();
    desc.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
    return SUCCEEDED(g_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&output)));
}

bool OcclusionCulling::Initialize(ID3D12Device *device, ID3D12DescriptorHeap *heap, UINT descriptorIncrement)
{
    Shutdown();
    if (!device || !heap || descriptorIncrement == 0)
    {
        return false;
    }
    g_Device = device;
    g_Heap = heap;
    g_Increment = descriptorIncrement;
    CD3DX12_DESCRIPTOR_RANGE ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    CD3DX12_ROOT_PARAMETER parameters[3];
    parameters[0].InitAsDescriptorTable(1, &ranges[0]);
    parameters[1].InitAsDescriptorTable(1, &ranges[1]);
    parameters[2].InitAsConstants(4, 0);
    CD3DX12_ROOT_SIGNATURE_DESC desc(_countof(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        if (errors)
        {
            Debug::Log("Hi-Z root signature: %s\n", static_cast<const char *>(errors->GetBufferPointer()));
        }
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&g_RootSignature))))
    {
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Format = DXGI_FORMAT_R32_FLOAT;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Texture2D.MipLevels = 1;
    for (UINT i = 0; i < 2; ++i)
    {
        device->CreateShaderResourceView(nullptr, &nullSrv, Cpu(RendererState::g_kHIZ_SRV_START_INDEX + i));
    }
    return LoadPipeline(L"shader/hlsl/build/HiZReduceCS.cso", g_ReducePso) &&
           LoadPipeline(L"shader/hlsl/build/HiZCopyCS.cso", g_CopyPso) &&
           LoadPipeline(L"shader/hlsl/build/HiZDebugCS.cso", g_DebugPso);
}

void OcclusionCulling::Shutdown()
{
    g_HiZ[0].Reset();
    g_HiZ[1].Reset();
    g_ReducePso.Reset();
    g_CopyPso.Reset();
    g_OccluderDepth.Reset();
    g_OccluderDsv.Reset();
    g_DebugPso.Reset();
    g_DebugTexture.Reset();
    g_DebugReady = g_DebugEnabled = false;
    g_RootSignature.Reset();
    g_Heap.Reset();
    g_Device.Reset();
    g_Increment = g_Width = g_Height = g_MipCount = g_Phase = 0;
    g_PreviousIndex = 0;
    g_CurrentIndex = 1;
    g_PreviousValid = g_CurrentValid = false;
}

bool OcclusionCulling::Resize(UINT width, UINT height)
{
    if (!g_Device || !g_Heap || width == 0 || height == 0)
    {
        return false;
    }
    if (width == g_Width && height == g_Height && g_HiZ[0] && g_HiZ[1])
    {
        return true;
    }
    if (!GraphicsDevice::WaitForGpuIdle())
    {
        return false;
    }
    g_Width = width;
    g_Height = height;
    g_MipCount = min<UINT>(1u + static_cast<UINT>(floor(log2(static_cast<double>(max(width, height))))),
                           RendererState::g_kHIZ_MAX_MIPS);
    g_PreviousValid = g_CurrentValid = false;
    g_PreviousIndex = 0;
    g_CurrentIndex = 1;
    auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto resourceDesc =
        CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, width, height, 1, static_cast<UINT16>(g_MipCount), 1, 0,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (UINT texture = 0; texture < 2; ++texture)
    {
        g_HiZ[texture].Reset();
        if (FAILED(g_Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, ReadState(),
                                                     nullptr, IID_PPV_ARGS(&g_HiZ[texture]))))
        {
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = g_MipCount;
        g_Device->CreateShaderResourceView(g_HiZ[texture].Get(), &srv,
                                           Cpu(RendererState::g_kHIZ_SRV_START_INDEX + texture));
        for (UINT mip = 0; mip < g_MipCount; ++mip)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC mipSrv = srv;
            mipSrv.Texture2D.MostDetailedMip = mip;
            mipSrv.Texture2D.MipLevels = 1;
            g_Device->CreateShaderResourceView(
                g_HiZ[texture].Get(), &mipSrv,
                Cpu(RendererState::g_kHIZ_MIP_SRV_START_INDEX + texture * RendererState::g_kHIZ_MAX_MIPS + mip));
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D.MipSlice = mip;
            g_Device->CreateUnorderedAccessView(
                g_HiZ[texture].Get(), nullptr, &uav,
                Cpu(RendererState::g_kHIZ_UAV_START_INDEX + texture * RendererState::g_kHIZ_MAX_MIPS + mip));
        }
    }
    auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, width, height, 1, 1, 1, 0,
                                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1;
    g_OccluderDepth.Reset();
    if (FAILED(g_Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &clear,
                                                 IID_PPV_ARGS(&g_OccluderDepth))))
    {
        return false;
    }
    if (!g_OccluderDsv)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = 1;
        if (FAILED(g_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_OccluderDsv))))
        {
            return false;
        }
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g_Device->CreateDepthStencilView(g_OccluderDepth.Get(), &dsv, g_OccluderDsv->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    g_Device->CreateShaderResourceView(g_OccluderDepth.Get(), &srv, Cpu(RendererState::g_kHIZ_SOURCE_SRV_INDEX));
    if (!g_DebugTexture)
    {
        const auto debugDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 320, 180, 1, 1, 1, 0,
                                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(g_Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &debugDesc,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&g_DebugTexture))))
        {
            return false;
        }
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_Device->CreateShaderResourceView(g_DebugTexture.Get(), &srv, Cpu(RendererState::g_kHIZ_DEBUG_SRV_INDEX));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = srv.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_Device->CreateUnorderedAccessView(g_DebugTexture.Get(), nullptr, &uav,
                                            Cpu(RendererState::g_kHIZ_DEBUG_UAV_INDEX));
    }
    return true;
}

bool OcclusionCulling::IsAvailable()
{
    return g_RootSignature && g_ReducePso && g_HiZ[0] && g_HiZ[1];
}

void OcclusionCulling::BeginPhaseOne()
{
    g_Phase = static_cast<UINT>((!GPUDrivenRenderer::IsActive() || VirtualGeometryTraversal::TwoPassActive()) && RendererSettings::GetTwoPhaseOcclusionEnabled() &&
                                IsAvailable());
    g_CurrentValid = false;
}

void OcclusionCulling::BuildCurrent(ID3D12GraphicsCommandList *commands, ID3D12Resource *depth)
{
    if (!commands || !depth || g_Phase != 1u || !IsAvailable())
    {
        return;
    }
    // Legacy caller places depth in COPY_SOURCE. The graph restores that state.
    const auto desc = depth->GetDesc();
    const auto srv = Gpu(desc.Width == GraphicsDevice::GetWidth() && desc.Height == GraphicsDevice::GetHeight()
                             ? RendererState::g_kDEPTH_SRV_INDEX
                             : RendererState::g_kLOW_RES_DEPTH_SRV_INDEX);
    BuildPyramid(commands, depth, srv, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

void OcclusionCulling::BuildTemporal(ID3D12GraphicsCommandList *commands, ID3D12Resource *depth,
                                     D3D12_GPU_DESCRIPTOR_HANDLE srv)
{
    BuildPyramid(commands, depth, srv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void OcclusionCulling::BuildPyramid(ID3D12GraphicsCommandList *commands, ID3D12Resource *depth,
                                    D3D12_GPU_DESCRIPTOR_HANDLE srv, D3D12_RESOURCE_STATES depthState)
{
    if (!commands || !depth || !IsAvailable())
    {
        return;
    }
    RenderProfiler::ScopedEvent profile("Hi-Z Build", commands);
    RenderGraph graph;
    RenderGraph::ImportedResource input{};
    input.Name = "Hi-Z Source Depth";
    input.Resource = depth;
    input.InitialState = input.FinalState = depthState;
    input.HasFinalState = true;
    const auto source = graph.ImportResource(input);
    vector<RenderGraph::ResourceHandle> mips;
    for (UINT mip = 0; mip < g_MipCount; ++mip)
    {
        input.Name = "Hi-Z Mip";
        input.Resource = g_HiZ[g_CurrentIndex].Get();
        input.InitialState = input.FinalState = ReadState();
        input.Subresource = mip;
        mips.push_back(graph.ImportResource(input));
    }
    for (UINT mip = 0; mip < g_MipCount; ++mip)
    {
        const auto read = mip == 0 ? source : mips[mip - 1];
        const auto write = mips[mip];
        graph.AddPass(
            "Hi-Z Mip Generation",
            [=](RenderGraph::PassBuilder &builder) {
                builder.Read(read, mip == 0 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : ReadState())
                    .Write(write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            },
            [=](ID3D12GraphicsCommandList *list) {
                ID3D12DescriptorHeap *heap = g_Heap.Get();
                list->SetDescriptorHeaps(1, &heap);
                list->SetComputeRootSignature(g_RootSignature.Get());
                list->SetPipelineState(mip == 0 ? g_CopyPso.Get() : g_ReducePso.Get());
                list->SetComputeRootDescriptorTable(0, mip == 0 ? srv
                                                                : Gpu(RendererState::g_kHIZ_MIP_SRV_START_INDEX +
                                                                      g_CurrentIndex * RendererState::g_kHIZ_MAX_MIPS +
                                                                      mip - 1));
                list->SetComputeRootDescriptorTable(1, Gpu(RendererState::g_kHIZ_UAV_START_INDEX +
                                                           g_CurrentIndex * RendererState::g_kHIZ_MAX_MIPS + mip));
                const UINT width = max(g_Width >> mip, 1u), height = max(g_Height >> mip, 1u);
                const UINT constants[4] = {width, height, 0, 0};
                list->SetComputeRoot32BitConstants(2, 4, constants, 0);
                list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            });
    }
    if (g_DebugEnabled && g_DebugTexture)
    {
        input.Name = "Hi-Z Grayscale Preview";
        input.Resource = g_DebugTexture.Get();
        input.Subresource = 0;
        input.InitialState = input.FinalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        const auto preview = graph.ImportResource(input);
        const UINT mip = min(g_DebugMip, g_MipCount - 1);
        graph.AddPass(
            "Hi-Z Debug Preview",
            [=](RenderGraph::PassBuilder &builder) {
                builder.Read(mips[mip], ReadState()).Write(preview, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            },
            [=](ID3D12GraphicsCommandList *list) {
                list->SetComputeRootSignature(g_RootSignature.Get());
                list->SetPipelineState(g_DebugPso.Get());
                list->SetComputeRootDescriptorTable(0, Gpu(RendererState::g_kHIZ_MIP_SRV_START_INDEX +
                                                           g_CurrentIndex * RendererState::g_kHIZ_MAX_MIPS + mip));
                list->SetComputeRootDescriptorTable(1, Gpu(RendererState::g_kHIZ_DEBUG_UAV_INDEX));
                const UINT constants[4] = {320, 180, 0, 0};
                list->SetComputeRoot32BitConstants(2, 4, constants, 0);
                list->Dispatch(40, 23, 1);
                g_DebugReady = true;
            });
    }
    if (!graph.Compile() || !graph.Execute(commands))
    {
        throw runtime_error(graph.GetLastError());
    }
    g_CurrentValid = true;
}

void OcclusionCulling::InvalidateHistory()
{
    g_PreviousValid = false;
}
ID3D12Resource *OcclusionCulling::GetPreviousResource()
{
    return g_HiZ[g_PreviousIndex].Get();
}
ID3D12Resource *OcclusionCulling::GetCurrentResource()
{
    return g_HiZ[g_CurrentIndex].Get();
}
ID3D12Resource *OcclusionCulling::GetOccluderDepth()
{
    return g_OccluderDepth.Get();
}
D3D12_CPU_DESCRIPTOR_HANDLE OcclusionCulling::GetOccluderDsv()
{
    return g_OccluderDsv->GetCPUDescriptorHandleForHeapStart();
}
D3D12_GPU_DESCRIPTOR_HANDLE OcclusionCulling::GetOccluderSrv()
{
    return Gpu(RendererState::g_kHIZ_SOURCE_SRV_INDEX);
}
D3D12_GPU_DESCRIPTOR_HANDLE OcclusionCulling::GetDebugMipSrv(UINT mip)
{
    if (g_DebugReady)
    {
        return Gpu(RendererState::g_kHIZ_DEBUG_SRV_INDEX);
    }
    return Gpu(RendererState::g_kHIZ_MIP_SRV_START_INDEX + g_PreviousIndex * RendererState::g_kHIZ_MAX_MIPS +
               min(mip, g_MipCount - 1));
}
void OcclusionCulling::SetDebugPreview(UINT mip, bool enabled)
{
    g_DebugMip = mip;
    g_DebugEnabled = enabled;
}

void OcclusionCulling::BeginPhaseTwo()
{
    if (g_Phase == 1u && g_CurrentValid)
    {
        g_Phase = 2u;
    }
}

void OcclusionCulling::EndFrame()
{
    if (g_CurrentValid)
    {
        g_PreviousIndex = g_CurrentIndex;
        g_CurrentIndex = 1u - g_CurrentIndex;
        g_PreviousValid = true;
    }
    g_CurrentValid = false;
    g_Phase = 0;
}

UINT OcclusionCulling::GetPhase()
{
    return g_Phase;
}
D3D12_GPU_DESCRIPTOR_HANDLE OcclusionCulling::GetPreviousSrv()
{
    return Gpu(RendererState::g_kHIZ_SRV_START_INDEX + g_PreviousIndex);
}
D3D12_GPU_DESCRIPTOR_HANDLE OcclusionCulling::GetCurrentSrv()
{
    return Gpu(RendererState::g_kHIZ_SRV_START_INDEX + g_CurrentIndex);
}
bool OcclusionCulling::HasPrevious()
{
    return g_PreviousValid;
}
bool OcclusionCulling::HasCurrent()
{
    return g_CurrentValid;
}
UINT OcclusionCulling::GetWidth()
{
    return g_Width;
}
UINT OcclusionCulling::GetHeight()
{
    return g_Height;
}
UINT OcclusionCulling::GetMipCount()
{
    return g_MipCount;
}
