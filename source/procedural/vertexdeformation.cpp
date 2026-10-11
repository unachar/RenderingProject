#include "pch.h"
#include "vertexdeformation.h"
#include "vertexdeformationmath.h"
#include "componentmanager.h"
#include "graphicsdevice.h"
#include "modelmanager.h"
#include "world.h"
#include "renderprofiler.h"
#include <map>
#include <set>
#include <tuple>
#include <cmath>

namespace
{
using GeometryKey = std::tuple<EntityID, UINT, int>;
constexpr UINT kPrimitiveMesh = UINT_MAX;
constexpr UINT kFrameCount = 3;
constexpr UINT kConstantSize = 256;

struct MotionConstants
{
    XMFLOAT4 AmplitudeAndPhase{};
    XMFLOAT4 Parameters{}; // spatial frequency, noise time, method, seed bits
};
struct Constants
{
    MotionConstants Current;
    MotionConstants Previous;
    XMUINT4 Geometry{};
};
static_assert(sizeof(Constants) <= kConstantSize);

struct FrameGeometry
{
    ComPtr<ID3D12Resource> Base;
    ComPtr<ID3D12Resource> Current;
    ComPtr<ID3D12Resource> Previous;
    ComPtr<ID3D12Resource> ConstantBuffer;
    D3D12_VERTEX_BUFFER_VIEW View{};
    UINT64 Revision = 0;
};
struct History
{
    UINT64 BaseAddress = 0;
    MotionConstants Motion{};
    double Time = 0;
    bool Valid = false;
};

std::array<std::map<GeometryKey, FrameGeometry>, kFrameCount> g_Frames;
std::map<GeometryKey, History> g_History;
ComPtr<ID3D12RootSignature> g_Root;
ComPtr<ID3D12PipelineState> g_Pipeline;
UINT g_FrameIndex = 0;
UINT g_Dispatches = 0;
UINT64 g_Revision = 0;

bool Initialize()
{
    if (g_Pipeline) return true;
    ID3D12Device* device = GraphicsDevice::GetDevice();
    if (!device) return false;
    CD3DX12_ROOT_PARAMETER parameters[4];
    parameters[0].InitAsShaderResourceView(0);
    parameters[1].InitAsUnorderedAccessView(0);
    parameters[2].InitAsUnorderedAccessView(1);
    parameters[3].InitAsConstantBufferView(0);
    const CD3DX12_ROOT_SIGNATURE_DESC description(4, parameters);
    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_Root))))
    {
        return false;
    }
    ComPtr<ID3DBlob> shader;
    if (FAILED(D3DCompileFromFile(L"shader/hlsl/VertexDeformationCS.hlsl", nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_6_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors)))
    {
        if (errors) Debug::Log("Vertex deformation: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = g_Root.Get();
    pipeline.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
    return SUCCEEDED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g_Pipeline)));
}

bool Allocate(FrameGeometry& geometry, ID3D12Resource* base, const D3D12_VERTEX_BUFFER_VIEW& view)
{
    if (geometry.Base.Get() == base && geometry.View.SizeInBytes == view.SizeInBytes && geometry.Current) return true;
    FrameGeometry created;
    created.Base = base;
    created.View = view;
    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto vertexDescription = CD3DX12_RESOURCE_DESC::Buffer(view.SizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* device = GraphicsDevice::GetDevice();
    const std::array<ComPtr<ID3D12Resource>*, 2> outputs = { std::addressof(created.Current), std::addressof(created.Previous) };
    for (auto* output : outputs)
    {
        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vertexDescription,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr, IID_PPV_ARGS(output->ReleaseAndGetAddressOf())))) return false;
    }
    created.Current->SetName(L"Procedural Current Vertices");
    created.Previous->SetName(L"Procedural Previous Vertices");
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto constantDescription = CD3DX12_RESOURCE_DESC::Buffer(kConstantSize);
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &constantDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&created.ConstantBuffer)))) return false;
    geometry = std::move(created);
    return true;
}

MotionConstants PackMotion(const ProceduralMotionComponent& motion)
{
    MotionConstants result;
    const double cycles = motion.Time * motion.Speed;
    const float phase = std::isfinite(cycles) ? static_cast<float>(std::remainder(cycles, 1.0) * XM_2PI) : 0;
    const float noiseTime = std::isfinite(cycles) ? static_cast<float>(std::remainder(cycles, 8192.0)) : 0;
    const auto amplitude = [](float value) { return std::isfinite(value) ? std::clamp(value, -100.0f, 100.0f) : 0.0f; };
    result.AmplitudeAndPhase = { amplitude(motion.Amplitude.x), amplitude(motion.Amplitude.y), amplitude(motion.Amplitude.z), phase };
    float seedBits;
    memcpy(&seedBits, &motion.Seed, sizeof(seedBits));
    result.Parameters = { std::isfinite(motion.SpatialFrequency) ? std::clamp(motion.SpatialFrequency, 0.001f, 4.0f) : 0.3f, noiseTime,
        static_cast<float>(motion.Method), seedBits };
    return result;
}

void DispatchGeometry(ID3D12GraphicsCommandList* commandList, const GeometryKey& key,
    ID3D12Resource* base, const D3D12_VERTEX_BUFFER_VIEW& view, const ProceduralMotionComponent& motion)
{
    if (!base || view.StrideInBytes != sizeof(Vertex) || view.SizeInBytes == 0) return;
    FrameGeometry& geometry = g_Frames[g_FrameIndex][key];
    if (!Allocate(geometry, base, view)) return;
    History& history = g_History[key];
    Constants constants;
    constants.Current = PackMotion(motion);
    const bool reset = !history.Valid || history.BaseAddress != view.BufferLocation || motion.Time < history.Time;
    constants.Previous = reset ? constants.Current : history.Motion;
    constants.Geometry = { view.SizeInBytes / view.StrideInBytes, view.StrideInBytes, 0, 0 };
    void* mapped = nullptr;
    const CD3DX12_RANGE noRead(0, 0);
    if (FAILED(geometry.ConstantBuffer->Map(0, &noRead, &mapped))) return;
    memcpy(mapped, &constants, sizeof(constants));
    geometry.ConstantBuffer->Unmap(0, nullptr);

    D3D12_HEAP_PROPERTIES heapProperties{};
    base->GetHeapProperties(&heapProperties, nullptr);
    const bool uploadBase = heapProperties.Type == D3D12_HEAP_TYPE_UPLOAD;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    if (!uploadBase)
    {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(base,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }
    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(geometry.Current.Get(),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(geometry.Previous.Get(),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    commandList->SetComputeRootSignature(g_Root.Get());
    commandList->SetPipelineState(g_Pipeline.Get());
    commandList->SetComputeRootShaderResourceView(0, view.BufferLocation);
    commandList->SetComputeRootUnorderedAccessView(1, geometry.Current->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(2, geometry.Previous->GetGPUVirtualAddress());
    commandList->SetComputeRootConstantBufferView(3, geometry.ConstantBuffer->GetGPUVirtualAddress());
    const UINT groups = (constants.Geometry.x + 63u) / 64u;
    commandList->Dispatch((std::min)(groups, 65535u), (groups + 65534u) / 65535u, 1);
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    history = { view.BufferLocation, constants.Current, motion.Time, true };
    geometry.Revision = g_Revision;
    ++g_Dispatches;
}

bool Resolve(const GeometryKey& key, VertexDeformationView& output)
{
    if (!VertexDeformation::IsRequested(std::get<0>(key))) return false;
    const auto found = g_Frames[g_FrameIndex].find(key);
    if (found == g_Frames[g_FrameIndex].end() || found->second.Revision != g_Revision) return false;
    const auto& geometry = found->second;
    output.CurrentResource = geometry.Current.Get();
    output.PreviousResource = geometry.Previous.Get();
    output.CurrentView = geometry.View;
    output.PreviousView = geometry.View;
    output.CurrentView.BufferLocation = geometry.Current->GetGPUVirtualAddress();
    output.PreviousView.BufferLocation = geometry.Previous->GetGPUVirtualAddress();
    output.GeometryRevision = geometry.Revision;
    return true;
}
}

bool VertexDeformation::IsRequested(EntityID entity)
{
    if (!Registry::IsAlive(entity) || !ComponentManager::HasComponent<ProceduralMotionComponent>(entity) ||
        ComponentManager::HasComponent<AnimationModelComponent>(entity)) return false;
    const auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(entity);
    return motion.Enabled && motion.Target == Procedural::MotionTarget::MeshVertices;
}

void VertexDeformation::PrepareFrame(ID3D12GraphicsCommandList* commandList, UINT frameIndex)
{
    g_FrameIndex = frameIndex % kFrameCount;
    g_Dispatches = 0;
    ++g_Revision;
    if (!commandList || !Initialize()) return;
    RenderProfiler::ScopedEvent profile("Procedural Vertices", commandList);
    std::set<GeometryKey> live;
    for (EntityID entity : World::GetView<ProceduralMotionComponent>())
    {
        if (!IsRequested(entity)) continue;
        const auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(entity);
        if (ComponentManager::HasComponent<StaticModelComponent>(entity))
        {
            const auto& modelComponent = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
            const auto* model = ModelManager::GetStaticModel(modelComponent.ModelId);
            if (!model) continue;
            for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
            {
                const auto& mesh = model->GetMeshData(meshIndex);
                GeometryKey key(entity, meshIndex, -1);
                live.insert(key);
                DispatchGeometry(commandList, key, mesh.VertexBuffer.Get(), mesh.VertexBufferView, motion);
                for (int outlineMode = 0; outlineMode < kToonOutlineModeCount; ++outlineMode)
                {
                    if (!mesh.TeoVertexBuffers[outlineMode]) continue;
                    key = { entity, meshIndex, outlineMode };
                    live.insert(key);
                    DispatchGeometry(commandList, key, mesh.TeoVertexBuffers[outlineMode].Get(), mesh.TeoVertexBufferViews[outlineMode], motion);
                }
            }
        }
        else if (ComponentManager::HasComponent<MeshComponent>(entity))
        {
            const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
            const GeometryKey key(entity, kPrimitiveMesh, -1);
            live.insert(key);
            DispatchGeometry(commandList, key, mesh.VertexBuffer.Get(), mesh.VertexBufferView, motion);
        }
    }
    // Only this frame slot has completed its fence. Other slots retain their old buffers until safe.
    for (auto it = g_Frames[g_FrameIndex].begin(); it != g_Frames[g_FrameIndex].end();)
    {
        if (!live.count(it->first)) it = g_Frames[g_FrameIndex].erase(it);
        else ++it;
    }
    for (auto it = g_History.begin(); it != g_History.end();)
    {
        if (!live.count(it->first)) it = g_History.erase(it);
        else ++it;
    }
}

bool VertexDeformation::TryResolveMesh(EntityID entity, VertexDeformationView& output)
{
    return Resolve({ entity, kPrimitiveMesh, -1 }, output);
}
bool VertexDeformation::TryResolveStaticMesh(EntityID entity, UINT meshIndex, VertexDeformationView& output, int outlineMode)
{
    return Resolve({ entity, meshIndex, outlineMode }, output);
}
bool VertexDeformation::TryGetLocalBounds(EntityID entity, XMFLOAT3& center, XMFLOAT3& extents)
{
    if (!IsRequested(entity)) return false;
    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
    {
        const auto* model = ModelManager::GetStaticModel(ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId);
        if (!model) return false;
        center = model->GetAabbCenter();
        extents = model->GetAabbExtents();
    }
    else if (ComponentManager::HasComponent<MeshComponent>(entity))
    {
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (!mesh.HasLocalBounds) return false;
        center = mesh.LocalBoundsCenter;
        extents = mesh.LocalBoundsExtents;
    }
    else return false;
    const auto& motion = ComponentManager::GetComponentUnchecked<ProceduralMotionComponent>(entity);
    const auto padding = Procedural::GetDeformationBoundsPadding({ motion.Method,
        { motion.Amplitude.x, motion.Amplitude.y, motion.Amplitude.z }, motion.Speed, motion.SpatialFrequency, motion.Seed });
    extents.x += padding.X;
    extents.y += padding.Y;
    extents.z += padding.Z;
    return true;
}
void VertexDeformation::ResetHistory(EntityID entity)
{
    for (auto& [key, history] : g_History)
    {
        if (std::get<0>(key) == entity) history.Valid = false;
    }
}
void VertexDeformation::Shutdown()
{
    g_Frames = {};
    g_History.clear();
    g_Pipeline.Reset();
    g_Root.Reset();
    g_Dispatches = 0;
    g_Revision = 0;
}
UINT VertexDeformation::GetDispatchCount() { return g_Dispatches; }
