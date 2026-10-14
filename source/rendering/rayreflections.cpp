#include "pch.h"
#include "rayreflections.h"
#include "raytracing.h"
#include "rendererstate.h"
#include "rendertargets.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "lightingresources.h"
#include "world.h"
#include <map>

namespace
{
constexpr UINT kFrames = RendererState::g_kFRAME_COUNT;
constexpr UINT kMaximumMeshes = 16384;
constexpr UINT kVertexDescriptors = 8;
constexpr UINT kIndexDescriptors = kVertexDescriptors + kMaximumMeshes;
constexpr UINT kTextureDescriptors = kIndexDescriptors + kMaximumMeshes;
constexpr UINT kResultSrv = RendererState::g_kHIZ_MIP_SRV_START_INDEX + 2 * RendererState::g_kHIZ_MAX_MIPS + 40;
static_assert(kResultSrv + kFrames <= RendererState::g_kENGINE_DESCRIPTOR_END);
struct MeshConstants
{
    XMFLOAT4X4 World{};
    XMFLOAT4X4 NormalToWorld{};
    XMUINT4 Layout{};
    XMFLOAT4 Material{};
};
struct FrameConstants
{
    XMFLOAT4X4 InvViewProjection{};
    XMFLOAT4X4 View{};
    XMFLOAT4X4 ViewProjection{};
    XMFLOAT4 Camera{};
    XMUINT4 Extent{};
};
struct ReflectionLight
{
    XMFLOAT4 Direction{}, Color{}, PositionType{}, Extras{};
    XMUINT4 Flags{};
};
static_assert(sizeof(ReflectionLight) == 80);
static_assert(sizeof(MeshConstants) == 160 && sizeof(FrameConstants) <= 256);
struct FrameResources
{
    ComPtr<ID3D12DescriptorHeap> Heap;
    ComPtr<ID3D12Resource> Meshes, Lights, Constants, Noisy, Result;
    // Keep sampled resources alive until this frame slot's fence completes.
    vector<RaytracingGeometry> Geometry;
    UINT Width = 0, Height = 0;
};
ComPtr<ID3D12Device> g_Device;
ComPtr<ID3D12DescriptorHeap> g_EngineHeap;
ComPtr<ID3D12RootSignature> g_Root;
ComPtr<ID3D12PipelineState> g_Trace, g_Compose;
array<FrameResources, kFrames> g_Frames;
unique_ptr<NrdDenoiser> g_Denoiser;
UINT g_Increment = 0, g_Width = 0, g_Height = 0, g_Dispatches = 0;
UINT g_LastFrame = 0;
bool g_HistoryValid = false;

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* heap, UINT index)
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(), index, g_Increment);
}
D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* heap, UINT index)
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), index, g_Increment);
}
bool EnsureUpload(ComPtr<ID3D12Resource>& resource, UINT64 bytes)
{
    if (resource && resource->GetDesc().Width >= bytes) return true;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    return SUCCEEDED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())));
}
bool Upload(ID3D12Resource* resource, const void* data, size_t bytes)
{
    void* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(resource->Map(0, &noRead, &mapped))) return false;
    memcpy(mapped, data, bytes);
    resource->Unmap(0, nullptr);
    return true;
}
void TextureSrv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT minimumResidentMip = 0)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Format = resource ? resource->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (desc.Format == DXGI_FORMAT_R32_TYPELESS) desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = resource ? resource->GetDesc().MipLevels : 1;
    desc.Texture2D.ResourceMinLODClamp = static_cast<float>(minimumResidentMip);
    g_Device->CreateShaderResourceView(resource, &desc, handle);
}
void BufferSrv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = resource ? static_cast<UINT>(resource->GetDesc().Width / 4) : 1;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    g_Device->CreateShaderResourceView(resource, &desc, handle);
}
bool EnsureFrame(FrameResources& frame, UINT slot, UINT width, UINT height)
{
    if (!frame.Heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        desc.NumDescriptors = kTextureDescriptors + kMaximumMeshes;
        if (FAILED(g_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&frame.Heap)))) return false;
    }
    if (!EnsureUpload(frame.Constants, 256)) return false;
    if (frame.Result && frame.Width == width && frame.Height == height) return true;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
        width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (auto* resource : { addressof(frame.Noisy), addressof(frame.Result) })
    {
        if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())))) return false;
    }
    frame.Noisy->SetName(L"Ray Reflection NRD Input");
    frame.Result->SetName(L"Ray Reflection Radiance");
    frame.Width = width; frame.Height = height;
    TextureSrv(frame.Result.Get(), Cpu(g_EngineHeap.Get(), kResultSrv + slot));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = desc.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_Device->CreateUnorderedAccessView(frame.Noisy.Get(), nullptr, &uav, Cpu(frame.Heap.Get(), 4));
    g_Device->CreateUnorderedAccessView(frame.Result.Get(), nullptr, &uav, Cpu(frame.Heap.Get(), 5));
    return true;
}
void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (!resource || before == after) return;
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
    commands->ResourceBarrier(1, &barrier);
}
}

bool RayReflections::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap, UINT increment)
{
    g_Device = device; g_EngineHeap = engineHeap; g_Increment = increment;
    CD3DX12_DESCRIPTOR_RANGE ranges[5];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 1);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);
    for (UINT i = 2; i < 5; ++i) ranges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumMeshes, 0, i - 1);
    CD3DX12_ROOT_PARAMETER parameters[11];
    parameters[0].InitAsShaderResourceView(25);
    parameters[1].InitAsShaderResourceView(0);
    parameters[2].InitAsConstantBufferView(6);
    parameters[3].InitAsConstantBufferView(1);
    parameters[4].InitAsConstantBufferView(5);
    for (UINT i = 0; i < 5; ++i) parameters[5 + i].InitAsDescriptorTable(1, &ranges[i]);
    parameters[10].InitAsShaderResourceView(5);
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    const CD3DX12_ROOT_SIGNATURE_DESC root(11, parameters, 1, &sampler);
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_Root)))) return false;
    const D3D_SHADER_MACRO defines[] = { { "USE_NRD", NrdDenoiser::IsCompiled() ? "1" : "0" }, {} };
    auto compile = [&](const char* entry, ComPtr<ID3D12PipelineState>& pipeline)
    {
        blob.Reset(); errors.Reset();
        if (FAILED(D3DCompileFromFile(L"shader/hlsl/RayReflectionsCS.hlsl", defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry, "cs_6_5", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
        {
            WriteGraphicsLog("DXR reflection shader failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : entry);
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g_Root.Get(); desc.CS = CD3DX12_SHADER_BYTECODE(blob.Get());
        return SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    };
    return compile("main", g_Trace) && (!NrdDenoiser::IsCompiled() || compile("Compose", g_Compose));
}

void RayReflections::Shutdown()
{
    g_Denoiser.reset(); g_Frames = {}; g_Root.Reset(); g_Trace.Reset(); g_Compose.Reset();
    g_Device.Reset(); g_EngineHeap.Reset(); g_Width = g_Height = g_Dispatches = 0; g_HistoryValid = false;
}

bool RayReflections::Execute(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene,
    const vector<RaytracingGeometry>& geometry, const NrdFrame& nrdFrame,
    const array<ComPtr<ID3D12Resource>, 4>& guides, bool denoise)
{
    g_Dispatches = 0;
    if (!commands || !scene || !g_Trace || geometry.empty()) return false;
    const UINT slot = nrdFrame.FrameIndex % kFrames;
    g_LastFrame = slot;
    auto* depth = RenderTargets::GetGBufferResource(GBufferType::DEPTH);
    auto* normal = RenderTargets::GetGBufferResource(GBufferType::NORMAL);
    auto* material = RenderTargets::GetGBufferResource(GBufferType::MATERIAL);
    if (!depth || !normal || !material) return false;
    const auto extent = depth->GetDesc();
    auto& frame = g_Frames[slot];
    if (!EnsureFrame(frame, slot, static_cast<UINT>(extent.Width), extent.Height)) return false;
    frame.Geometry = geometry;
    const UINT count = static_cast<UINT>((min)(geometry.size(), static_cast<size_t>(kMaximumMeshes)));
    vector<MeshConstants> meshes(count);
    map<ID3D12Resource*, D3D12_RESOURCE_STATES> sampledResources;
    sampledResources.emplace(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    sampledResources.emplace(normal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    sampledResources.emplace(material, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (UINT i = 0; i < count; ++i)
    {
        const auto& geometry = frame.Geometry[i];
        auto& mesh = meshes[i];
        const XMMATRIX world = XMLoadFloat4x4(&geometry.World);
        XMStoreFloat4x4(&mesh.World, XMMatrixTranspose(world));
        XMStoreFloat4x4(&mesh.NormalToWorld, XMMatrixInverse(nullptr, world));
        mesh.Layout = { geometry.VertexView.StrideInBytes,
            geometry.Indices ? (geometry.IndexView.Format == DXGI_FORMAT_R16_UINT ? 16u : 32u) : 0u,
            static_cast<UINT>(geometry.VertexView.BufferLocation - geometry.Vertices->GetGPUVirtualAddress()),
            geometry.Indices ? static_cast<UINT>(geometry.IndexView.BufferLocation - geometry.Indices->GetGPUVirtualAddress()) : 0u };
        mesh.Material = { geometry.Roughness, geometry.Metallic, static_cast<float>(geometry.Texture != nullptr), 0 };
        BufferSrv(geometry.Vertices.Get(), Cpu(frame.Heap.Get(), kVertexDescriptors + i));
        BufferSrv(geometry.Indices.Get(), Cpu(frame.Heap.Get(), kIndexDescriptors + i));
        if (geometry.Texture)
        {
            TextureSrv(geometry.Texture.Get(), Cpu(frame.Heap.Get(), kTextureDescriptors + i), geometry.TextureResidentMip);
            sampledResources.emplace(geometry.Texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else TextureSrv(nullptr, Cpu(frame.Heap.Get(), kTextureDescriptors + i));
        sampledResources.emplace(geometry.Vertices.Get(), geometry.VertexState);
        if (geometry.Indices) sampledResources.emplace(geometry.Indices.Get(), geometry.IndexState);
    }
    if (!EnsureUpload(frame.Meshes, meshes.size() * sizeof(MeshConstants)) ||
        !Upload(frame.Meshes.Get(), meshes.data(), meshes.size() * sizeof(MeshConstants))) return false;
    FrameConstants constants;
    vector<ReflectionLight> lights;
    lights.reserve(RendererState::g_kMAX_SHADER_LIGHTS);
    for (EntityID entity : World::GetView<LightComponent, TransformComponent>())
    {
        const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
        if (!light.IsActive || !light.AffectsOpaque || light.RenderMode != LightRenderMode::Physical || light.Intensity <= 0) continue;
        if (lights.size() >= RendererState::g_kMAX_SHADER_LIGHTS) break;
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        XMFLOAT3 direction;
        XMStoreFloat3(&direction, XMVector3Normalize(XMLoadFloat3(&light.Direction)));
        ReflectionLight entry;
        entry.Direction = { direction.x, direction.y, direction.z, light.Range };
        entry.Color = { light.Color.x, light.Color.y, light.Color.z, light.Intensity * 1.35f };
        entry.PositionType = { transform.Position.x, transform.Position.y, transform.Position.z, static_cast<float>(light.Type) };
        entry.Extras = { cosf(XMConvertToRadians(light.InnerAngle)), cosf(XMConvertToRadians(light.OuterAngle)),
            light.VolumeDensity, static_cast<float>(light.VolumeShape) };
        entry.Flags.x = static_cast<UINT>(light.CastShadow);
        lights.push_back(entry);
    }
    if (!EnsureUpload(frame.Lights, (max)(sizeof(ReflectionLight), lights.size() * sizeof(ReflectionLight))) ||
        (!lights.empty() && !Upload(frame.Lights.Get(), lights.data(), lights.size() * sizeof(ReflectionLight)))) return false;
    const XMMATRIX view = XMLoadFloat4x4(&nrdFrame.View);
    XMMATRIX projection = XMLoadFloat4x4(&nrdFrame.Projection);
    // Restore the jitter used by this frame's GBuffer.
    XMFLOAT4X4 jittered; XMStoreFloat4x4(&jittered, projection);
    jittered._31 = -nrdFrame.Jitter.x * 2 / frame.Width;
    jittered._32 = nrdFrame.Jitter.y * 2 / frame.Height;
    projection = XMLoadFloat4x4(&jittered);
    XMStoreFloat4x4(&constants.InvViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, view * projection)));
    XMStoreFloat4x4(&constants.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&constants.ViewProjection, XMMatrixTranspose(view * projection));
    XMStoreFloat4(&constants.Camera, XMMatrixInverse(nullptr, view).r[3]);
    constants.Camera.w = nrdFrame.Range;
    constants.Extent = { frame.Width, frame.Height, nrdFrame.Sequence, static_cast<UINT>(lights.size()) };
    if (!Upload(frame.Constants.Get(), &constants, sizeof(constants))) return false;
    for (auto [resource, state] : sampledResources)
        if ((state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0)
            Transition(commands, resource, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (UINT i = 0; i < 3; ++i)
    {
        const auto type = i == 0 ? GBufferType::DEPTH : i == 1 ? GBufferType::NORMAL : GBufferType::MATERIAL;
        TextureSrv(RenderTargets::GetGBufferResource(type), Cpu(frame.Heap.Get(), i));
    }
    TextureSrv(nullptr, Cpu(frame.Heap.Get(), 3));
    auto bind = [&](ID3D12PipelineState* pipeline)
    {
        ID3D12DescriptorHeap* heaps[] = { frame.Heap.Get() }; commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(g_Root.Get()); commands->SetPipelineState(pipeline);
        commands->SetComputeRootShaderResourceView(0, scene->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(1, frame.Meshes->GetGPUVirtualAddress());
        commands->SetComputeRootConstantBufferView(2, frame.Constants->GetGPUVirtualAddress());
        commands->SetComputeRootConstantBufferView(3, LightingResources::GetCurrentLightConstantBufferAddress());
        commands->SetComputeRootConstantBufferView(4, Raytracing::GetLightingConstantsAddress(slot));
        commands->SetComputeRootDescriptorTable(5, Gpu(frame.Heap.Get(), 0));
        commands->SetComputeRootDescriptorTable(6, Gpu(frame.Heap.Get(), 4));
        commands->SetComputeRootDescriptorTable(7, Gpu(frame.Heap.Get(), kVertexDescriptors));
        commands->SetComputeRootDescriptorTable(8, Gpu(frame.Heap.Get(), kIndexDescriptors));
        commands->SetComputeRootDescriptorTable(9, Gpu(frame.Heap.Get(), kTextureDescriptors));
        commands->SetComputeRootShaderResourceView(10, frame.Lights->GetGPUVirtualAddress());
    };
    Transition(commands, frame.Noisy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bind(g_Trace.Get()); commands->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
    Transition(commands, frame.Noisy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // NRD owns a separate heap and restores these four input states.
    if (denoise && NrdDenoiser::IsCompiled() && guides[0] && guides[1] && guides[2])
    {
        if (!g_Denoiser || g_Width != frame.Width || g_Height != frame.Height)
        {
            if (GraphicsDevice::WaitForGpuIdle())
            {
                auto created = make_unique<NrdDenoiser>();
                if (created->Initialize(g_Device.Get(), NrdSignal::Specular, frame.Width, frame.Height))
                { g_Denoiser = move(created); g_Width = frame.Width; g_Height = frame.Height; g_HistoryValid = false; }
            }
        }
        NrdFrame temporalFrame = nrdFrame;
        temporalFrame.ResetHistory = temporalFrame.ResetHistory || !g_HistoryValid;
        NrdInputs inputs{ {guides[0].Get()}, {guides[1].Get()}, {guides[2].Get()}, {frame.Noisy.Get()} };
        if (g_Denoiser && g_Width == frame.Width && g_Height == frame.Height && g_Denoiser->Execute(commands, inputs, temporalFrame))
        {
            TextureSrv(g_Denoiser->GetOutput(), Cpu(frame.Heap.Get(), 3));
            Transition(commands, g_Denoiser->GetOutput(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            bind(g_Compose.Get()); commands->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
            Transition(commands, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(commands, g_Denoiser->GetOutput(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g_Dispatches = g_Denoiser->GetDispatchCount(); g_HistoryValid = true;
        }
        else g_HistoryValid = false;
    }
    else g_HistoryValid = false;
    for (auto [resource, state] : sampledResources)
        if ((state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0)
            Transition(commands, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state);
    ID3D12DescriptorHeap* engine[] = { g_EngineHeap.Get() }; commands->SetDescriptorHeaps(1, engine);
    return true;
}
D3D12_GPU_DESCRIPTOR_HANDLE RayReflections::GetResultSrv(UINT frameIndex)
{
    return Gpu(g_EngineHeap.Get(), kResultSrv + frameIndex % kFrames);
}
UINT RayReflections::GetDenoiserDispatchCount() { return g_Dispatches; }
bool RayReflections::DidRestartHistory() { return g_Denoiser && g_Denoiser->DidRestartHistory(); }
ID3D12Resource* RayReflections::GetResultResource()
{
    return g_Frames[g_LastFrame].Result.Get();
}
ID3D12Resource* RayReflections::GetNoisyResource()
{
    return g_Frames[g_LastFrame].Noisy.Get();
}
