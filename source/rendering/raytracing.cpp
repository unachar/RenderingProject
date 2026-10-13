#include "pch.h"
#include "raytracing.h"
#include "raytracingscene.h"
#include "rendererstate.h"
#include "graphicslog.h"
#include "camera.h"
#include "world.h"
#include "nrddenoiser.h"
#include "rayreflections.h"
#include "rendertargets.h"
#include "graphicsdevice.h"
#include <map>
#include <tuple>

namespace
{
constexpr UINT kFrameCount = RendererState::g_kFRAME_COUNT;
constexpr UINT kSrvStart = RendererState::g_kHIZ_MIP_SRV_START_INDEX + 2 * RendererState::g_kHIZ_MAX_MIPS;
constexpr UINT kUavStart = kSrvStart + kFrameCount;
constexpr UINT kFallbackSrv = kUavStart + kFrameCount;
constexpr UINT kGuideUavStart = kSrvStart + 16; // after the existing DXR and DLSS descriptor ranges
constexpr UINT kNrdShadowSrvStart = kGuideUavStart + 5 * kFrameCount;
constexpr UINT kNrdAmbientSrvStart = kNrdShadowSrvStart + kFrameCount;
static_assert(kNrdAmbientSrvStart + kFrameCount <= RendererState::g_kENGINE_DESCRIPTOR_END);
static_assert(kFallbackSrv < RendererState::g_kENGINE_DESCRIPTOR_END, "DXR descriptor range exceeds engine heap");
constexpr UINT kConstantStride = 256;
constexpr UINT kMaximumInstances = 16384;

using GeometryKey = std::tuple<UINT64, UINT64, UINT, UINT, UINT>;

struct BottomLevel
{
    ComPtr<ID3D12Resource> Result;
    ComPtr<ID3D12Resource> Scratch;
    ComPtr<ID3D12Resource> Vertices;
    ComPtr<ID3D12Resource> Indices;
    UINT64 LastSeen = 0;
    UINT64 LastBuilt = 0;
};

struct FrameResources
{
    std::array<ComPtr<ID3D12Resource>, 4> Guides;
    ComPtr<ID3D12Resource> AmbientGuide;
    std::map<GeometryKey, BottomLevel> Geometry;
    ComPtr<ID3D12Resource> TopLevel;
    ComPtr<ID3D12Resource> Scratch;
    ComPtr<ID3D12Resource> Instances;
    ComPtr<ID3D12Resource> Result;
    UINT Width = 0;
    std::vector<RaytracingGeometry> SceneGeometry;
    // A scene SRV can already reference the previous TLAS earlier in this same
    // command list. Keep replaced resources until this frame's fence is reused.
    std::vector<ComPtr<ID3D12Resource>> Retired;
    UINT Height = 0;
};

struct Constants
{
    XMFLOAT4X4 InvViewProjection;
    XMFLOAT4 SunDirection;
    XMUINT4 ExtentSamples;
    XMFLOAT4 Distances;
    XMUINT4 Features;
    XMFLOAT4X4 WorldToView;
    XMFLOAT4 Jitter;
};
struct LightingConstants
{
    XMFLOAT4 Flags{};
    XMFLOAT4 Parameters{};
    std::array<XMUINT4, 160> Lights{};
};
constexpr UINT kLightingStride = (sizeof(LightingConstants) + 255u) & ~255u;
std::array<XMUINT4, 160> g_LightShadowFlags{};
ComPtr<ID3D12Resource> g_LightingConstants;
static_assert(sizeof(Constants) <= kConstantStride);

ComPtr<ID3D12Device5> g_Device;
ComPtr<ID3D12DescriptorHeap> g_Heap;
ComPtr<ID3D12RootSignature> g_RootSignature;
ComPtr<ID3D12PipelineState> g_Pipeline;
ComPtr<ID3D12PipelineState> g_ComposeShadow;
ComPtr<ID3D12PipelineState> g_ComposeAmbient;
std::unique_ptr<NrdDenoiser> g_NrdSun;
std::unique_ptr<NrdDenoiser> g_NrdAmbient;
UINT g_NrdWidth = 0, g_NrdHeight = 0, g_NrdDispatchCount = 0;
UINT g_AmbientDispatchCount = 0;
NrdFrame g_PreviousNrdFrame;
bool g_NrdHistoryValid = false;
bool g_ShadowHistoryValid = false;
bool g_AmbientHistoryValid = false;
EntityID g_NrdCamera = g_kINVALID_ENTITY;
ComPtr<ID3D12Resource> g_Constants;
std::array<FrameResources, kFrameCount> g_Frames;
Raytracing::Settings g_Settings;
UINT g_Increment = 0;
UINT g_FrameIndex = 0;
UINT g_InstanceCount = 0;
UINT64 g_Serial = 0;
bool g_Active = false;
bool g_ReflectionsActive = false;
XMFLOAT4 g_SunDirection{};
const char* g_Status = "未初期化";

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index)
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Heap->GetCPUDescriptorHandleForHeapStart(), index, g_Increment);
}

D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index)
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(g_Heap->GetGPUDescriptorHandleForHeapStart(), index, g_Increment);
}

bool EnsureBuffer(ComPtr<ID3D12Resource>& resource, UINT64 bytes,
    D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state, const wchar_t* name)
{
    bytes = (bytes + 255ull) & ~255ull;
    if (resource && resource->GetDesc().Width >= bytes)
    {
        return true;
    }
    const auto heap = CD3DX12_HEAP_PROPERTIES(heapType);
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (heapType == D3D12_HEAP_TYPE_DEFAULT)
    {
        flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
    ComPtr<ID3D12Resource> created;
    const HRESULT result = g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &description, state, nullptr, IID_PPV_ARGS(&created));
    if (FAILED(result))
    {
        Debug::Log("DXR: buffer allocation failed (0x%08X)\n", static_cast<unsigned int>(result));
        return false;
    }
    created->SetName(name);
    if (resource) g_Frames[g_FrameIndex].Retired.push_back(resource);
    resource = std::move(created);
    return true;
}

void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (resource && before != after && before != D3D12_RESOURCE_STATE_GENERIC_READ &&
        after != D3D12_RESOURCE_STATE_GENERIC_READ)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        commandList->ResourceBarrier(1, &barrier);
    }
}

bool EnsureOutput(FrameResources& frame, UINT width, UINT height)
{
    if (frame.Result && frame.Width == width && frame.Height == height)
    {
        return true;
    }
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto description = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16_FLOAT,
        width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> created;
    if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&created))))
    {
        return false;
    }
    created->SetName(L"Raytraced Sun Shadow and AO");
    frame.Result = std::move(created);
    frame.Width = width;
    frame.Height = height;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = description.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_Device->CreateShaderResourceView(frame.Result.Get(), &srv, Cpu(kSrvStart + g_FrameIndex));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = description.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_Device->CreateUnorderedAccessView(frame.Result.Get(), nullptr, &uav, Cpu(kUavStart + g_FrameIndex));
    return true;
}

bool EnsureDenoiser(FrameResources& frame)
{
    if (!NrdDenoiser::IsCompiled() || !g_ComposeShadow) return false;
    constexpr DXGI_FORMAT formats[] = { DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT };
    for (UINT index = 0; index < frame.Guides.size(); ++index)
    {
        auto& guide = frame.Guides[index];
        if (guide && guide->GetDesc().Width == frame.Width && guide->GetDesc().Height == frame.Height) continue;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description = CD3DX12_RESOURCE_DESC::Tex2D(formats[index], frame.Width, frame.Height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(guide.ReleaseAndGetAddressOf())))) return false;
        guide->SetName(L"NRD Primary Surface Guide");
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = formats[index];
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_Device->CreateUnorderedAccessView(guide.Get(), nullptr, &uav, Cpu(kGuideUavStart + g_FrameIndex * 5 + index));
    }
    if (!frame.AmbientGuide || frame.AmbientGuide->GetDesc().Width != frame.Width || frame.AmbientGuide->GetDesc().Height != frame.Height)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16_FLOAT, frame.Width, frame.Height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(frame.AmbientGuide.ReleaseAndGetAddressOf())))) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = desc.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_Device->CreateUnorderedAccessView(frame.AmbientGuide.Get(), nullptr, &uav, Cpu(kGuideUavStart + g_FrameIndex * 5 + 4));
    }
    if (!g_NrdSun || g_NrdWidth != frame.Width || g_NrdHeight != frame.Height)
    {
        // Histories are shared across queued frames; retire them only after the queue has completed.
        if (!GraphicsDevice::WaitForGpuIdle()) return false;
        auto created = std::make_unique<NrdDenoiser>();
        if (!created->Initialize(g_Device.Get(), NrdSignal::Shadow, frame.Width, frame.Height)) return false;
        auto ambient = std::make_unique<NrdDenoiser>();
        if (!ambient->Initialize(g_Device.Get(), NrdSignal::AmbientOcclusion, frame.Width, frame.Height)) return false;
        g_NrdSun = std::move(created);
        g_NrdAmbient = std::move(ambient);
        g_NrdWidth = frame.Width;
        g_NrdHeight = frame.Height;
        g_NrdHistoryValid = false;
    }
    return true;
}

bool BuildBottomLevel(ID3D12GraphicsCommandList4* commandList, const RaytracingGeometry& geometry, BottomLevel& level)
{
    level.LastSeen = g_Serial;
    if (level.Result && (!geometry.Deforming || level.LastBuilt == g_Serial))
    {
        return true;
    }
    D3D12_RAYTRACING_GEOMETRY_DESC description{};
    description.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    description.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    description.Triangles.VertexBuffer.StartAddress = geometry.VertexView.BufferLocation;
    description.Triangles.VertexBuffer.StrideInBytes = geometry.VertexView.StrideInBytes;
    description.Triangles.VertexCount = geometry.VertexView.SizeInBytes / geometry.VertexView.StrideInBytes;
    description.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    description.Triangles.IndexBuffer = geometry.IndexView.BufferLocation;
    description.Triangles.IndexCount = geometry.IndexCount;
    description.Triangles.IndexFormat = geometry.IndexView.Format;
    if (!geometry.Indices)
    {
        description.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &description;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (geometry.Deforming)
    {
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
    g_Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
    const bool update = geometry.Deforming && level.Result;
    if (sizes.ResultDataMaxSizeInBytes == 0 ||
        !EnsureBuffer(level.Result, sizes.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"Scene BLAS") ||
        !EnsureBuffer(level.Scratch, (std::max)(sizes.ScratchDataSizeInBytes, sizes.UpdateScratchDataSizeInBytes),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"BLAS Scratch"))
    {
        return false;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = level.Result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = level.Scratch->GetGPUVirtualAddress();
    if (update)
    {
        build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        build.SourceAccelerationStructureData = build.DestAccelerationStructureData;
    }
    Transition(commandList, geometry.Vertices.Get(), geometry.VertexState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commandList, geometry.Indices.Get(), geometry.IndexState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Upload heaps remain GENERIC_READ: it already includes NON_PIXEL_SHADER_RESOURCE.
    commandList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(level.Result.Get());
    commandList->ResourceBarrier(1, &barrier);
    Transition(commandList, geometry.Vertices.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, geometry.VertexState);
    Transition(commandList, geometry.Indices.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, geometry.IndexState);
    level.Vertices = geometry.Vertices;
    level.Indices = geometry.Indices;
    level.LastBuilt = g_Serial;
    return true;
}

bool BuildScene(ID3D12GraphicsCommandList4* commandList, FrameResources& frame)
{
    const auto geometry = GatherRaytracingGeometry();
    frame.SceneGeometry.clear();
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    instances.reserve((std::min)(geometry.size(), static_cast<size_t>(kMaximumInstances)));
    for (const auto& entry : geometry)
    {
        if (instances.size() >= kMaximumInstances || entry.VertexView.StrideInBytes < sizeof(XMFLOAT3))
        {
            continue;
        }
        const GeometryKey key(entry.VertexView.BufferLocation, entry.IndexView.BufferLocation,
            entry.VertexView.SizeInBytes, entry.IndexCount, entry.VertexView.StrideInBytes);
        BottomLevel& bottom = frame.Geometry[key];
        if (!BuildBottomLevel(commandList, entry, bottom))
        {
            return false;
        }
        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        XMFLOAT4X4 transform;
        XMStoreFloat4x4(&transform, XMMatrixTranspose(XMLoadFloat4x4(&entry.World)));
        memcpy(instance.Transform, &transform, sizeof(instance.Transform));
        instance.InstanceID = static_cast<UINT>(instances.size());
        instance.InstanceMask = 0xff;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure = bottom.Result->GetGPUVirtualAddress();
        instances.push_back(instance);
        frame.SceneGeometry.push_back(entry);
    }
    for (auto iterator = frame.Geometry.begin(); iterator != frame.Geometry.end();)
    {
        if (iterator->second.LastSeen != g_Serial)
        {
            const auto& bottom = iterator->second;
            if (bottom.Result) frame.Retired.push_back(bottom.Result);
            if (bottom.Scratch) frame.Retired.push_back(bottom.Scratch);
            if (bottom.Vertices) frame.Retired.push_back(bottom.Vertices);
            if (bottom.Indices) frame.Retired.push_back(bottom.Indices);
            iterator = frame.Geometry.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    g_InstanceCount = static_cast<UINT>(instances.size());
    if (instances.empty())
    {
        return false;
    }
    if (!EnsureBuffer(frame.Instances, instances.size() * sizeof(instances.front()), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, L"TLAS Instance Descriptions"))
    {
        return false;
    }
    void* mapped = nullptr;
    const CD3DX12_RANGE noRead(0, 0);
    if (FAILED(frame.Instances->Map(0, &noRead, &mapped)))
    {
        return false;
    }
    memcpy(mapped, instances.data(), instances.size() * sizeof(instances.front()));
    frame.Instances->Unmap(0, nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = g_InstanceCount;
    inputs.InstanceDescs = frame.Instances->GetGPUVirtualAddress();
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
    g_Device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
    if (!EnsureBuffer(frame.TopLevel, sizes.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"Scene TLAS") ||
        !EnsureBuffer(frame.Scratch, sizes.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"TLAS Scratch"))
    {
        return false;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = frame.TopLevel->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = frame.Scratch->GetGPUVirtualAddress();
    commandList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(frame.TopLevel.Get());
    commandList->ResourceBarrier(1, &barrier);
    return true;
}

void FindSunDirection()
{
    g_SunDirection = {};
    for (EntityID entity : World::GetView<SunComponent, LightComponent>())
    {
        const auto& light = ComponentManager::GetComponentUnchecked<LightComponent>(entity);
        if (light.Type == LightType::Directional && light.RenderMode == LightRenderMode::Physical &&
            light.IsActive && light.CastShadow && light.AffectsOpaque && light.Intensity > 0.0f)
        {
            XMFLOAT3 direction;
            XMStoreFloat3(&direction, XMVector3Normalize(XMLoadFloat3(&light.Direction)));
            g_SunDirection = { direction.x, direction.y, direction.z, 1.0f };
            return;
        }
    }
}
}

bool Raytracing::Initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT increment)
{
    Shutdown();
    g_Heap = heap;
    g_Increment = increment;
    if (!device || !heap || increment == 0)
    {
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC fallback{};
    fallback.Format = DXGI_FORMAT_R16G16_FLOAT;
    fallback.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    fallback.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    fallback.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &fallback, Cpu(kFallbackSrv));
	char disabled[8]{};
	if (GetEnvironmentVariableA("DX12_DISABLE_DXR", disabled, sizeof(disabled)) > 0 && atoi(disabled) != 0)
	{
		g_Status = "検証設定で DXR を無効にしています";
		WriteGraphicsLog("DXR: disabled by validation override; raster fallback\n");
		return false;
	}
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ D3D_SHADER_MODEL_6_5 };
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) ||
        options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) ||
        shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_5 ||
        FAILED(device->QueryInterface(IID_PPV_ARGS(&g_Device))))
    {
        g_Status = "DXR Tier 1.1 / Shader Model 6.5 非対応";
        WriteGraphicsLog("DXR: unavailable; keeping raster rendering\n");
        return false;
    }
    CD3DX12_DESCRIPTOR_RANGE ranges[8];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 1);
    ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
    ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);
    ranges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);
    ranges[7].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);
    CD3DX12_ROOT_PARAMETER parameters[10];
    parameters[0].InitAsShaderResourceView(0);
    for (UINT index = 0; index < 3; ++index)
    {
        parameters[index + 1].InitAsDescriptorTable(1, &ranges[index]);
    }
    parameters[4].InitAsConstantBufferView(0);
    for (UINT index = 3; index < 8; ++index) parameters[index + 2].InitAsDescriptorTable(1, &ranges[index]);
    const CD3DX12_ROOT_SIGNATURE_DESC rootDescription(10, parameters);
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&g_RootSignature))))
    {
        g_Status = "DXR root signature 作成失敗";
        return false;
    }
    ComPtr<ID3DBlob> shader;
    if (FAILED(D3DReadFileToBlob(NrdDenoiser::IsCompiled() ?
        L"shader/hlsl/build/RaytracingNrdCS.cso" : L"shader/hlsl/build/RaytracingCS.cso", &shader)))
    {
        g_Status = "RaytracingCS.cso がありません";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = g_RootSignature.Get();
    pipeline.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g_Pipeline))) ||
        !EnsureBuffer(g_Constants, kConstantStride * kFrameCount, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, L"Raytracing Frame Constants") ||
        !EnsureBuffer(g_LightingConstants, kLightingStride * kFrameCount, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, L"Scene Ray Lighting Constants"))
    {
        g_Pipeline.Reset();
        g_Status = "DXR pipeline 作成失敗";
        return false;
    }
    if (NrdDenoiser::IsCompiled())
    {
        shader.Reset();
        if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/NrdComposeShadowCS.cso", &shader))) return false;
        pipeline.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
        if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g_ComposeShadow)))) return false;
        shader.Reset();
        if (FAILED(D3DReadFileToBlob(L"shader/hlsl/build/NrdComposeAmbientCS.cso", &shader))) return false;
        pipeline.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
        if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g_ComposeAmbient)))) return false;
    }
    g_Status = "DXR Tier 1.1 対応";
    void* lightingData = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (SUCCEEDED(g_LightingConstants->Map(0, &noRead, &lightingData)))
    {
        memset(lightingData, 0, kLightingStride * kFrameCount);
        g_LightingConstants->Unmap(0, nullptr);
    }
    if (!RayReflections::Initialize(device, heap, increment))
        WriteGraphicsLog("DXR: reflection pipeline initialization failed\n");
    WriteGraphicsLog("DXR: Tier 1.1 inline raytracing ready\n");
    return true;
}

void Raytracing::Shutdown()
{
    RayReflections::Shutdown();
    g_NrdSun.reset();
    g_NrdWidth = g_NrdHeight = g_NrdDispatchCount = 0;
    g_NrdHistoryValid = false;
    g_NrdCamera = g_kINVALID_ENTITY;
    g_ComposeShadow.Reset();
    g_ComposeAmbient.Reset();
    g_NrdAmbient.reset();
    g_Frames = {};
    g_Constants.Reset();
    g_LightingConstants.Reset();
    g_LightShadowFlags = {};
    g_Pipeline.Reset();
    g_RootSignature.Reset();
    g_Device.Reset();
    g_Heap.Reset();
    g_Active = false;
    g_ReflectionsActive = false;
    g_ShadowHistoryValid = false;
    g_AmbientHistoryValid = false;
    g_InstanceCount = 0;
    g_Serial = 0;
}

void Raytracing::Execute(ID3D12GraphicsCommandList* commandList, ID3D12Resource* depth, ID3D12Resource* normal,
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrv, D3D12_GPU_DESCRIPTOR_HANDLE normalSrv, UINT frameIndex)
{
    g_Active = false;
    g_ReflectionsActive = false;
    const bool shadowHistoryValid = g_ShadowHistoryValid;
    const bool ambientHistoryValid = g_AmbientHistoryValid;
    g_ShadowHistoryValid = false;
    g_AmbientHistoryValid = false;
    g_NrdDispatchCount = 0;
    g_AmbientDispatchCount = 0;
    g_InstanceCount = 0;
    g_FrameIndex = frameIndex % kFrameCount;
    g_Frames[g_FrameIndex].Retired.clear();
    auto updateLighting = [&](bool sceneReady)
    {
        if (!g_LightingConstants) return;
        LightingConstants lighting{};
        lighting.Flags = { static_cast<float>(UsesRaytracedShadows()),
            static_cast<float>(UsesRaytracedShadows()), static_cast<float>(sceneReady), 0.0f };
        lighting.Parameters = { std::clamp(g_Settings.ShadowDistance, 1.0f, 2000.0f),
            std::clamp(g_Settings.NormalBias, 0.001f, 0.1f), 0.0f, 0.0f };
        lighting.Lights = g_LightShadowFlags;
        void* mapped = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        if (SUCCEEDED(g_LightingConstants->Map(0, &noRead, &mapped)))
        {
            memcpy(static_cast<BYTE*>(mapped) + g_FrameIndex * kLightingStride, &lighting, sizeof(lighting));
            g_LightingConstants->Unmap(0, nullptr);
        }
    };
    updateLighting(false);
    if (!IsAvailable() || !g_Settings.Enabled || (!UsesRaytracedShadows() && !UsesRaytracedAmbientOcclusion() && !g_Settings.Reflections) ||
        !commandList || !depth || !normal)
    {
        g_NrdHistoryValid = false;
        return;
    }
    RenderProfiler::ScopedEvent profile("DXR Shadows / AO", commandList);
    ++g_Serial;
    FrameResources& frame = g_Frames[g_FrameIndex];
    const auto extent = depth->GetDesc();
    ComPtr<ID3D12GraphicsCommandList4> rayCommands;
    if (FAILED(commandList->QueryInterface(IID_PPV_ARGS(&rayCommands))) ||
        !EnsureOutput(frame, static_cast<UINT>(extent.Width), extent.Height) || !BuildScene(rayCommands.Get(), frame))
    {
        return;
    }
    FindSunDirection();
    const EntityID cameraEntity = Camera::GetCameraEntity();
    if (cameraEntity != g_NrdCamera) g_NrdHistoryValid = false;
    g_NrdCamera = cameraEntity;
    // The NRD-enabled trace shader always produces its four guides, even when denoising is switched off.
    const bool guidesReady = !NrdDenoiser::IsCompiled() || EnsureDenoiser(frame);
    if (!guidesReady) return;
    XMMATRIX view;
    XMMATRIX projection;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    Constants constants{};
    XMStoreFloat4x4(&constants.InvViewProjection, XMMatrixTranspose(XMMatrixInverse(nullptr, view * projection)));
    constants.SunDirection = g_SunDirection;
    constants.ExtentSamples = { frame.Width, frame.Height, static_cast<UINT>(std::clamp(g_Settings.Samples, 1, 8)), static_cast<UINT>(g_Serial) };
    constants.Distances = { std::clamp(g_Settings.ShadowDistance, 1.0f, 2000.0f),
        std::clamp(g_Settings.AmbientRadius, 0.05f, 10.0f), std::clamp(g_Settings.NormalBias, 0.001f, 0.1f),
        std::clamp(g_Settings.SunAngularRadius, 0.0f, 0.05f) };
    constants.Features = { static_cast<UINT>(UsesRaytracedShadows()), static_cast<UINT>(UsesRaytracedAmbientOcclusion()), 0, 0 };
    XMStoreFloat4x4(&constants.WorldToView, XMMatrixTranspose(view));
    NrdFrame nrdFrame;
    XMStoreFloat4x4(&nrdFrame.View, view);
    XMStoreFloat4x4(&nrdFrame.Projection, projection);
    nrdFrame.Jitter = { -nrdFrame.Projection._31 * frame.Width * 0.5f, nrdFrame.Projection._32 * frame.Height * 0.5f };
    nrdFrame.Projection._31 = nrdFrame.Projection._32 = 0;
    nrdFrame.PreviousView = g_NrdHistoryValid ? g_PreviousNrdFrame.View : nrdFrame.View;
    nrdFrame.PreviousProjection = g_NrdHistoryValid ? g_PreviousNrdFrame.Projection : nrdFrame.Projection;
    nrdFrame.PreviousJitter = g_NrdHistoryValid ? g_PreviousNrdFrame.Jitter : nrdFrame.Jitter;
    nrdFrame.LightDirection = { g_SunDirection.x, g_SunDirection.y, g_SunDirection.z };
    nrdFrame.FrameIndex = g_FrameIndex;
    nrdFrame.Sequence = static_cast<UINT>(g_Serial);
    nrdFrame.Range = 2000.0f;
    nrdFrame.AmbientRadius = constants.Distances.y;
    if (ComponentManager::HasComponent<CameraComponent>(cameraEntity))
        nrdFrame.Range = (std::max)(ComponentManager::GetComponentUnchecked<CameraComponent>(cameraEntity).FarClip, 1.0f);
    nrdFrame.ResetHistory = !g_NrdHistoryValid;
    constants.Jitter = { nrdFrame.Jitter.x, nrdFrame.Jitter.y, nrdFrame.PreviousJitter.x, nrdFrame.PreviousJitter.y };
    memcpy(&constants.Features.w, &nrdFrame.Range, sizeof(float));
    void* mapped = nullptr;
    const CD3DX12_RANGE noRead(0, 0);
    if (FAILED(g_Constants->Map(0, &noRead, &mapped)))
    {
        return;
    }
    memcpy(static_cast<BYTE*>(mapped) + g_FrameIndex * kConstantStride, &constants, sizeof(constants));
    g_Constants->Unmap(0, nullptr);
    Transition(commandList, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commandList, normal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* velocity = RenderTargets::GetGBufferResource(GBufferType::VELOCITY);
    ID3D12Resource* material = RenderTargets::GetGBufferResource(GBufferType::MATERIAL);
    if (NrdDenoiser::IsCompiled())
    {
        for (auto& guide : frame.Guides) Transition(commandList, guide.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(commandList, frame.AmbientGuide.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(commandList, velocity, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, material, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    ID3D12DescriptorHeap* heaps[] = { g_Heap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_RootSignature.Get());
    commandList->SetPipelineState(g_Pipeline.Get());
    commandList->SetComputeRootShaderResourceView(0, frame.TopLevel->GetGPUVirtualAddress());
    commandList->SetComputeRootDescriptorTable(1, depthSrv);
    commandList->SetComputeRootDescriptorTable(2, normalSrv);
    commandList->SetComputeRootDescriptorTable(3, Gpu(kUavStart + g_FrameIndex));
    commandList->SetComputeRootConstantBufferView(4, g_Constants->GetGPUVirtualAddress() + g_FrameIndex * kConstantStride);
    if (NrdDenoiser::IsCompiled())
    {
        commandList->SetComputeRootDescriptorTable(5, Gpu(kGuideUavStart + g_FrameIndex * 5));
        commandList->SetComputeRootDescriptorTable(6, RenderTargets::GetGBufferSrvHandle(GBufferType::VELOCITY));
        commandList->SetComputeRootDescriptorTable(7, RenderTargets::GetGBufferSrvHandle(GBufferType::MATERIAL));
    }
    commandList->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
    Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (NrdDenoiser::IsCompiled())
    {
        for (auto& guide : frame.Guides) Transition(commandList, guide.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commandList, frame.AmbientGuide.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commandList, velocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commandList, material, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const bool denoise = g_Settings.Denoise && UsesRaytracedShadows() && g_SunDirection.w > 0.5f;
        NrdFrame sunFrame = nrdFrame;
        sunFrame.ResetHistory |= !shadowHistoryValid;
        const auto& oldDirection = g_PreviousNrdFrame.LightDirection;
        const float directionSimilarity = oldDirection.x * nrdFrame.LightDirection.x +
            oldDirection.y * nrdFrame.LightDirection.y + oldDirection.z * nrdFrame.LightDirection.z;
        if (directionSimilarity < 0.99f) sunFrame.ResetHistory = true;
        if (denoise && g_NrdSun->Execute(commandList,
            { { frame.Guides[0].Get() }, { frame.Guides[1].Get() }, { frame.Guides[2].Get() }, { frame.Guides[3].Get() } }, sunFrame))
        {
            g_NrdDispatchCount = g_NrdSun->GetDispatchCount();
            g_ShadowHistoryValid = true;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_Device->CreateShaderResourceView(g_NrdSun->GetOutput(), &srv, Cpu(kNrdShadowSrvStart + g_FrameIndex));
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetComputeRootSignature(g_RootSignature.Get());
            commandList->SetPipelineState(g_ComposeShadow.Get());
            commandList->SetComputeRootDescriptorTable(1, depthSrv);
            commandList->SetComputeRootDescriptorTable(3, Gpu(kUavStart + g_FrameIndex));
            commandList->SetComputeRootConstantBufferView(4, g_Constants->GetGPUVirtualAddress() + g_FrameIndex * kConstantStride);
            commandList->SetComputeRootDescriptorTable(8, Gpu(kNrdShadowSrvStart + g_FrameIndex));
            Transition(commandList, g_NrdSun->GetOutput(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
            Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(commandList, g_NrdSun->GetOutput(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (g_Serial == 1) WriteGraphicsLog("NRD: SIGMA GPU dispatches=%u\n", g_NrdDispatchCount);
        }
        NrdFrame ambientFrame = nrdFrame;
        ambientFrame.ResetHistory |= !ambientHistoryValid;
        if (g_Settings.Denoise && UsesRaytracedAmbientOcclusion() && g_NrdAmbient->Execute(commandList,
            { { frame.Guides[0].Get() }, { frame.Guides[1].Get() }, { frame.Guides[2].Get() }, { frame.AmbientGuide.Get() } }, ambientFrame))
        {
            g_AmbientDispatchCount = g_NrdAmbient->GetDispatchCount();
            g_AmbientHistoryValid = true;
            g_NrdDispatchCount += g_AmbientDispatchCount;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R16_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
            g_Device->CreateShaderResourceView(g_NrdAmbient->GetOutput(), &srv, Cpu(kNrdAmbientSrvStart + g_FrameIndex));
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetComputeRootSignature(g_RootSignature.Get()); commandList->SetPipelineState(g_ComposeAmbient.Get());
            commandList->SetComputeRootDescriptorTable(1, depthSrv);
            commandList->SetComputeRootDescriptorTable(3, Gpu(kUavStart + g_FrameIndex));
            commandList->SetComputeRootConstantBufferView(4, g_Constants->GetGPUVirtualAddress() + g_FrameIndex * kConstantStride);
            commandList->SetComputeRootDescriptorTable(9, Gpu(kNrdAmbientSrvStart + g_FrameIndex));
            Transition(commandList, g_NrdAmbient->GetOutput(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->Dispatch((frame.Width + 7) / 8, (frame.Height + 7) / 8, 1);
            Transition(commandList, frame.Result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(commandList, g_NrdAmbient->GetOutput(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        commandList->SetDescriptorHeaps(1, heaps);
    }
    Transition(commandList, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(commandList, normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_Active = true;
    updateLighting(true);
    if (g_Settings.Reflections)
        g_ReflectionsActive = RayReflections::Execute(commandList, frame.TopLevel.Get(), frame.SceneGeometry,
            nrdFrame, frame.Guides, g_Settings.Denoise);
    if (g_ReflectionsActive) g_NrdDispatchCount += RayReflections::GetDenoiserDispatchCount();
    // Camera and motion history belongs to the scene, independently of enabled denoiser signals.
    g_PreviousNrdFrame = nrdFrame;
    g_NrdHistoryValid = true;
	if (g_Serial == 1)
	{
		WriteGraphicsLog("DXR: first dispatch, instances=%u extent=%ux%u\n", g_InstanceCount, frame.Width, frame.Height);
	}
}

bool Raytracing::IsAvailable() { return g_Pipeline != nullptr; }
bool Raytracing::UsesRaytracedShadows()
{
    return IsAvailable() && g_Settings.Enabled && g_Settings.RenderMode == Mode::Full && g_Settings.Shadows;
}
bool Raytracing::UsesRaytracedAmbientOcclusion()
{
    return IsAvailable() && g_Settings.Enabled && g_Settings.RenderMode == Mode::Full && g_Settings.AmbientOcclusion;
}
bool Raytracing::IsActive() { return g_Active; }
const char* Raytracing::GetStatus() { return g_Status; }
Raytracing::Settings& Raytracing::GetSettings() { return g_Settings; }
D3D12_GPU_DESCRIPTOR_HANDLE Raytracing::GetResultSrv() { return Gpu(g_Active ? kSrvStart + g_FrameIndex : kFallbackSrv); }
XMFLOAT4 Raytracing::GetSunDirection() { return g_SunDirection; }
UINT Raytracing::GetInstanceCount() { return g_InstanceCount; }
UINT Raytracing::GetDenoiserDispatchCount() { return g_NrdDispatchCount; }
UINT Raytracing::GetAmbientDenoiserDispatchCount() { return g_AmbientDispatchCount; }
bool Raytracing::AreReflectionsActive() { return g_ReflectionsActive; }
D3D12_GPU_DESCRIPTOR_HANDLE Raytracing::GetReflectionSrv()
{
    return g_ReflectionsActive ? RayReflections::GetResultSrv(g_FrameIndex) : GetResultSrv();
}
D3D12_GPU_VIRTUAL_ADDRESS Raytracing::GetLightingConstantsAddress(UINT frameIndex)
{
    return g_LightingConstants ? g_LightingConstants->GetGPUVirtualAddress() + (frameIndex % kFrameCount) * kLightingStride : 0;
}
void Raytracing::BeginLightList() { g_LightShadowFlags = {}; }
void Raytracing::SetLightShadowFlag(UINT index, bool castShadow)
{
    if (index < g_LightShadowFlags.size()) g_LightShadowFlags[index].x = static_cast<UINT>(castShadow);
}
void Raytracing::BindLighting(ID3D12GraphicsCommandList* commands, UINT sceneIndex, UINT constantsIndex)
{
    if (!commands || !g_LightingConstants) return;
    const UINT slot = GraphicsDevice::GetFrameIndex() % kFrameCount;
    commands->SetGraphicsRootShaderResourceView(sceneIndex,
        g_Frames[slot].TopLevel ? g_Frames[slot].TopLevel->GetGPUVirtualAddress() : 0);
    commands->SetGraphicsRootConstantBufferView(constantsIndex, g_LightingConstants->GetGPUVirtualAddress() + slot * kLightingStride);
}
ID3D12Resource* Raytracing::GetResultResource()
{
	if (!g_Active)
	{
		return nullptr;
	}
	return g_Frames[g_FrameIndex].Result.Get();
}
