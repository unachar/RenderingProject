#include "pch.h"
#include "gpuvisibility.h"
#include "camera.h"
#include "componentmanager.h"
#include "frameconstants.h"
#include "gpuscene/gpuscene.h"
#include "gpuscene/gpuscenebuffer.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "occlusionculling.h"
#include "renderprofiler.h"
#include "shaderpipelines.h"
#include "meshshader/meshshaderrenderer.h"
#include "virtualgeometry/virtualgeometrytraversal.h"

namespace GPUVisibility
{
namespace
{
struct Constants
{
    XMFLOAT4X4 View{}, Projection{}, PreviousViewProjection{};
    uint32_t Slots = 0, Meshes = 0, Groups = 0, Levels = 0;
    uint32_t Occlusion = 0, LOD = 0, Reset = 0, Force = UINT32_MAX;
    float DepthBias = 0, LODBias = 0, Width = 1, Height = 1;
    uint32_t Mips = 1, Debug = 0, NativeHierarchy = 0, Padding = 0;
};
static_assert(sizeof(Constants) == 256);
struct FrameData
{
    GpuSceneBuffer Result, Counters;
    ComPtr<ID3D12Resource> Readback, RecordReadback;
    bool Pending = false;
    uint32_t RecordCount = 0;
};
struct State
{
    Settings Options;
    Constants Parameters;
    vector<Visibility::GPULODGroup> Groups;
    vector<Visibility::GPULODLevel> Levels;
    vector<uint32_t> NewGroups, NewLevels;
    GpuSceneBuffer GroupBuffer, LevelBuffer, History;
    array<FrameData, GpuSceneBuffer::FrameCount> Frames;
    ComPtr<ID3D12RootSignature> Root;
    ComPtr<ID3D12PipelineState> ResetPSO, OcclusionPSO, LODPSO;
    XMFLOAT4X4 PreviousVP{}, PreviousView{}, PreviousProjection{};
    uint32_t PreviousCamera = UINT32_MAX, FrameIndex = 0;
    bool HaveCamera = false, Cut = true, Reset = true, UsableHistory = false;
    bool PreviousOcclusion = false, PreviousLOD = true, CameraFrozen = false;
    int PreviousForce = -1;
    float PreviousBias = 0;
    const char *Reason = "First frame";
    Visibility::Statistics Latest;
    bool HasSample=false;
    vector<Visibility::VisibleInstanceData> DebugRecords;
};
State &Data()
{
    static State state;
    return state;
}
FrameData &Frame()
{
    return Data().Frames.at(Data().FrameIndex);
}
RenderGraph::ResourceHandle Import(RenderGraph &graph, const char *name, GpuSceneBuffer &buffer)
{
    RenderGraph::ImportedResource resource{};
    resource.Name = name;
    resource.Resource = buffer.Resource();
    resource.InitialState = buffer.State;
    resource.FinalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    resource.HasFinalState = true;
    buffer.State = resource.FinalState;
    return graph.ImportResource(resource);
}
void Initialize()
{
    auto &state = Data();
    if (state.Root)
    {
        return;
    }
    CD3DX12_DESCRIPTOR_RANGE hierarchy(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);
    CD3DX12_DESCRIPTOR_RANGE constants(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    CD3DX12_ROOT_PARAMETER parameters[10];
    for (uint32_t i = 0; i < 5; ++i)
    {
        parameters[i].InitAsShaderResourceView(i);
    }
    parameters[5].InitAsDescriptorTable(1, &hierarchy);
    for (uint32_t i = 0; i < 3; ++i)
    {
        parameters[6 + i].InitAsUnorderedAccessView(i);
    }
    parameters[9].InitAsDescriptorTable(1, &constants);
    CD3DX12_ROOT_SIGNATURE_DESC description(_countof(parameters), parameters);
    ComPtr<ID3DBlob> blob, errors;
    auto *device = GraphicsDevice::GetDevice();
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(
            device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&state.Root))))
    {
        throw runtime_error("GPU visibility root signature failed");
    }
    auto pipeline = [&](const char *path, ComPtr<ID3D12PipelineState> &output) {
        ComPtr<ID3DBlob> shader;
        ShaderDescription input{};
        input.csoPath = path;
        input.ppBlob = shader.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(input))
        {
            throw runtime_error("GPU visibility shader missing");
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = state.Root.Get();
        desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&output))))
        {
            throw runtime_error("GPU visibility compute pipeline failed");
        }
    };
    pipeline("shader/hlsl/build/GPUVisibilityResetCS.cso", state.ResetPSO);
    pipeline("shader/hlsl/build/GPUOcclusionCS.cso", state.OcclusionPSO);
    pipeline("shader/hlsl/build/GPULODSelectionCS.cso", state.LODPSO);
}
void AllocateReadback(ComPtr<ID3D12Resource> &resource, uint64_t bytes)
{
    if (resource)
    {
        return;
    }
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    if (FAILED(GraphicsDevice::GetDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource))))
    {
        throw runtime_error("GPU visibility debug readback allocation failed");
    }
}
void ConsumeReadback(FrameData &frame)
{
    if (!frame.Pending)
    {
        return;
    }
    // Prepare runs after this frame slot's fence; no wait or per-frame allocation.
    void *mapped = nullptr;
    D3D12_RANGE range{0, sizeof(Visibility::Statistics)};
    if (SUCCEEDED(frame.Readback->Map(0, &range, &mapped)))
    {
        memcpy(&Data().Latest, mapped, sizeof(Visibility::Statistics));
        Data().HasSample=true;
        const D3D12_RANGE noWrite{0, 0};
        frame.Readback->Unmap(0, &noWrite);
    }
    Data().DebugRecords.clear();
    if (frame.RecordCount)
    {
        range.End = size_t(frame.RecordCount) * sizeof(Visibility::VisibleInstanceData);
        if (SUCCEEDED(frame.RecordReadback->Map(0, &range, &mapped)))
        {
            auto *records = static_cast<Visibility::VisibleInstanceData *>(mapped);
            Data().DebugRecords.assign(records, records + frame.RecordCount);
            const D3D12_RANGE noWrite{0, 0};
            frame.RecordReadback->Unmap(0, &noWrite);
        }
    }
    frame.Pending = false;
}
void Bind(ID3D12GraphicsCommandList *list, ID3D12Resource *geometry)
{
    auto &data = Data();
    ID3D12DescriptorHeap *heap = FrameConstants::GetCbvHeap();
    list->SetDescriptorHeaps(1, &heap);
    list->SetComputeRootSignature(data.Root.Get());
    list->SetComputeRootShaderResourceView(0, GpuScene::InstanceBuffer()->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, GpuScene::VisibilityBuffer()->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(2, geometry->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, data.GroupBuffer.Resource()->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(4, data.LevelBuffer.Resource()->GetGPUVirtualAddress());
    // The engine creates a null R32 SRV when Hi-Z isn't available. Never bind an uninitialized descriptor.
    list->SetComputeRootDescriptorTable(5, OcclusionCulling::GetPreviousSrv());
    list->SetComputeRootUnorderedAccessView(6, Frame().Result.Resource()->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(7, data.History.Resource()->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(8, Frame().Counters.Resource()->GetGPUVirtualAddress());
    const auto cbv = FrameConstants::AllocateTransientConstantBuffer(&data.Parameters, sizeof(data.Parameters));
    if (!cbv.ptr)
    {
        throw runtime_error("GPU visibility transient constants exhausted");
    }
    list->SetComputeRootDescriptorTable(9, cbv);
}
} // namespace
Settings &GetSettings()
{
    return Data().Options;
}
DirectX::XMFLOAT4X4 CullingViewProjection()
{
    XMFLOAT4X4 result;
    const auto& p=Data().Parameters;
    XMStoreFloat4x4(&result,XMLoadFloat4x4(&p.View)*XMLoadFloat4x4(&p.Projection));
    return result;
}
vector<uint32_t> LODMeshes(uint32_t id)
{
    const auto& state=Data();
    vector<uint32_t> result;
    if (id<state.Groups.size())
    {
        const auto& group=state.Groups[id];
        for (uint32_t i=0;i<group.Count;++i) result.push_back(state.Levels[group.Offset+i].MeshIndex);
    }
    return result;
}
uint32_t RegisterLODGroup(const array<uint32_t, Visibility::MaxLODs> &meshes,
                          const Visibility::LODMetadata &metadata)
{
    if (!Visibility::ValidLOD(metadata))
    {
        return Visibility::InvalidID;
    }
    auto &state = Data();
    array<Visibility::GPULODLevel, Visibility::MaxLODs> levels{};
    for (uint32_t i = 0; i < metadata.Count; ++i)
    {
        levels[i] = {meshes[i], metadata.Thresholds[i], metadata.Hysteresis, 0};
    }
    for (uint32_t id = 0; id < state.Groups.size(); ++id)
    {
        const auto &group = state.Groups[id];
        if (group.Count == metadata.Count &&
            memcmp(state.Levels.data() + group.Offset, levels.data(), metadata.Count * sizeof(levels[0])) == 0)
        {
            return id;
        }
    }
    const uint32_t id = static_cast<uint32_t>(state.Groups.size());
    state.Groups.push_back({static_cast<uint32_t>(state.Levels.size()), metadata.Count, {0, 0}});
    state.NewGroups.push_back(id);
    for (uint32_t i = 0; i < metadata.Count; ++i)
    {
        state.NewLevels.push_back(static_cast<uint32_t>(state.Levels.size()));
        state.Levels.push_back(levels[i]);
    }
    return id;
}
void Invalidate(const char *reason)
{
    Data().Reset = true;
    Data().UsableHistory = false;
    Data().Reason = reason;
    OcclusionCulling::InvalidateHistory();
}
void PrepareCamera()
{
    auto &state = Data();
    if (state.Options.FreezeCamera && state.CameraFrozen)
    {
        // Frozen view is not compatible with Hi-Z captured from the live camera.
        state.UsableHistory=false;
        return;
    }
    if (state.CameraFrozen) Invalidate("Culling camera released");
    state.CameraFrozen=state.Options.FreezeCamera;
    if (OcclusionCulling::GetWidth() != GraphicsDevice::GetSceneWidth() ||
        OcclusionCulling::GetHeight() != GraphicsDevice::GetSceneHeight())
    {
        Invalidate("Render resolution changed");
        OcclusionCulling::Resize(GraphicsDevice::GetSceneWidth(), GraphicsDevice::GetSceneHeight());
    }
    if (state.Options.Occlusion != state.PreviousOcclusion || state.Options.LOD != state.PreviousLOD ||
        state.Options.ForceLOD != state.PreviousForce || state.Options.LODBias != state.PreviousBias)
    {
        OcclusionCulling::InvalidateHistory();
        state.Reason = "Visibility/LOD settings changed";
    }
    state.PreviousOcclusion = state.Options.Occlusion;
    state.PreviousLOD = state.Options.LOD;
    state.PreviousForce = state.Options.ForceLOD;
    state.PreviousBias = state.Options.LODBias;
    XMMATRIX view, projection;
    const auto camera = Camera::GetCameraEntity();
    if (camera == UINT32_MAX || !ComponentManager::HasComponent<CameraComponent>(camera))
    {
        Invalidate("No camera");
        state.Cut = true;
        return;
    }
    Camera::GetCameraMatrices(camera, view, projection);
    XMStoreFloat4x4(&state.Parameters.View, view);
    XMStoreFloat4x4(&state.Parameters.Projection, projection);
    const bool viewChanged = memcmp(&state.PreviousView, &state.Parameters.View, sizeof(XMFLOAT4X4)) != 0;
    const bool projectionChanged =
        memcmp(&state.PreviousProjection, &state.Parameters.Projection, sizeof(XMFLOAT4X4)) != 0;
    const XMMATRIX currentWorld = XMMatrixInverse(nullptr, view);
    const XMMATRIX previousWorld = XMMatrixInverse(nullptr, XMLoadFloat4x4(&state.PreviousView));
    const float translation =
        state.HaveCamera ? XMVectorGetX(XMVector3Length(currentWorld.r[3] - previousWorld.r[3])) : 0;
    const float direction = state.HaveCamera ? XMVectorGetX(XMVector3Dot(currentWorld.r[2], previousWorld.r[2])) : 1;
    state.Cut = !state.HaveCamera || state.PreviousCamera != camera || projectionChanged || translation > 10 ||
                direction < 0.70710678f;
    if (state.Cut)
    {
        Invalidate("Camera/view/projection changed");
    }
    else if (viewChanged)
    {
        OcclusionCulling::InvalidateHistory();
        state.Reason = "Camera moved (LOD history retained)";
    }
    state.Parameters.PreviousViewProjection = state.PreviousVP;
    state.UsableHistory = !state.Options.FreezeCamera && !state.Reset && OcclusionCulling::HasPrevious() && !state.Cut && !viewChanged;
    state.PreviousView = state.Parameters.View;
    state.PreviousProjection = state.Parameters.Projection;
    XMStoreFloat4x4(&state.PreviousVP, view * projection);
    state.PreviousCamera = camera;
    state.HaveCamera = true;
}
RenderGraph::ResourceHandle Prepare(RenderGraph &graph, RenderGraph::ResourceHandle instances,
                                    RenderGraph::ResourceHandle frustum, RenderGraph::ResourceHandle geometry,
                                    ID3D12Resource *geometryResource, uint32_t slots, uint32_t capacity,
                                    uint32_t meshCount)
{
    Initialize();
    auto &state = Data();
    state.FrameIndex = GraphicsDevice::GetFrameIndex();
    auto &frame = Frame();
    ConsumeReadback(frame);
    auto prepare = [&](GpuSceneBuffer &buffer, uint32_t count, uint32_t stride, const wchar_t *name, bool uav) {
        buffer.BeginFrame(state.FrameIndex);
        return buffer.Ensure(GraphicsDevice::GetDevice(), count, stride, state.FrameIndex, name, uav);
    };
    prepare(frame.Result, capacity, 16, L"GPU Visibility Results", true);
    prepare(frame.Counters, 16, 4, L"GPU Visibility Statistics", true);
    const bool newHistory = prepare(state.History, capacity, 16, L"GPU LOD History", true);
    prepare(state.GroupBuffer, static_cast<uint32_t>(state.Groups.size()), 16, L"GPU LOD Groups", false);
    prepare(state.LevelBuffer, static_cast<uint32_t>(state.Levels.size()), 16, L"GPU LOD Levels", false);
    const auto groupBytes = state.GroupBuffer.Stage(GraphicsDevice::GetDevice(), state.FrameIndex, state.Groups.data(),
                                                    static_cast<uint32_t>(state.Groups.size()), state.NewGroups);
    const auto levelBytes = state.LevelBuffer.Stage(GraphicsDevice::GetDevice(), state.FrameIndex, state.Levels.data(),
                                                    static_cast<uint32_t>(state.Levels.size()), state.NewLevels);
    state.NewGroups.clear();
    state.NewLevels.clear();
    const auto result = Import(graph, "Visibility + Selected LOD", frame.Result);
    const auto counters = Import(graph, "Visibility Statistics", frame.Counters);
    const auto history = Import(graph, "LOD History", state.History);
    const auto groups = Import(graph, "LOD Groups", state.GroupBuffer);
    const auto levels = Import(graph, "LOD Levels", state.LevelBuffer);
    if (groupBytes || levelBytes)
    {
        graph.AddPass(
            "LOD Metadata Upload",
            [=](RenderGraph::PassBuilder &builder) {
                if (groupBytes)
                {
                    builder.Write(groups, D3D12_RESOURCE_STATE_COPY_DEST);
                }
                if (levelBytes)
                {
                    builder.Write(levels, D3D12_RESOURCE_STATE_COPY_DEST);
                }
            },
            [](ID3D12GraphicsCommandList *list) {
                auto &data = Data();
                data.GroupBuffer.Copy(list, data.FrameIndex);
                data.LevelBuffer.Copy(list, data.FrameIndex);
            });
    }
    state.Parameters.Slots = slots;
    state.Parameters.Meshes = meshCount;
    state.Parameters.Groups = static_cast<uint32_t>(state.Groups.size());
    state.Parameters.Levels = static_cast<uint32_t>(state.Levels.size());
    state.Parameters.Occlusion = state.Options.Occlusion && state.UsableHistory;
    state.Parameters.LOD = state.Options.LOD;
    state.Parameters.NativeHierarchy = MeshShaderRenderer::NativeReady() &&
        VirtualGeometryTraversal::GetSettings().Enabled;
    state.Parameters.Reset = state.Reset || newHistory;
    state.Parameters.Force = static_cast<uint32_t>(state.Options.ForceLOD);
    state.Parameters.DepthBias = state.Options.DepthBias;
    state.Parameters.LODBias = state.Options.LODBias;
    state.Parameters.Width = float((max)(OcclusionCulling::GetWidth(), 1u));
    state.Parameters.Height = float((max)(OcclusionCulling::GetHeight(), 1u));
    state.Parameters.Mips = (max)(OcclusionCulling::GetMipCount(), 1u);
    graph.AddPass(
        "Visibility Counter Reset",
        [=](RenderGraph::PassBuilder &builder) { builder.Write(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); },
        [](ID3D12GraphicsCommandList *list) {
            list->SetComputeRootSignature(Data().Root.Get());
            list->SetPipelineState(Data().ResetPSO.Get());
            list->SetComputeRootUnorderedAccessView(8, Frame().Counters.Resource()->GetGPUVirtualAddress());
            list->Dispatch(1, 1, 1);
        });
    RenderGraph::ResourceHandle hierarchy;
    if (state.Parameters.Occlusion)
    {
        RenderGraph::ImportedResource resource{};
        resource.Name = "Previous Hi-Z";
        resource.Resource = OcclusionCulling::GetPreviousResource();
        resource.InitialState = resource.FinalState = OcclusionCulling::ReadState();
        resource.HasFinalState = true;
        hierarchy = graph.ImportResource(resource);
    }
    graph.AddPass(
        "GPU Occlusion Culling",
        [=](RenderGraph::PassBuilder &builder) {
            builder.Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(frustum, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(geometry, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Write(result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .ReadWrite(history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .ReadWrite(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (hierarchy.IsValid())
            {
                builder.Read(hierarchy, OcclusionCulling::ReadState());
            }
        },
        [geometryResource](ID3D12GraphicsCommandList *list) {
            RenderProfiler::ScopedEvent profile("Occlusion Culling", list);
            Bind(list, geometryResource);
            list->SetPipelineState(Data().OcclusionPSO.Get());
            if (Data().Parameters.Slots)
            {
                list->Dispatch((Data().Parameters.Slots + 63) / 64, 1, 1);
            }
        });
    graph.AddPass(
        "GPU LOD Selection",
        [=](RenderGraph::PassBuilder &builder) {
            builder.Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(geometry, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(groups, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(levels, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .ReadWrite(result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .ReadWrite(history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .ReadWrite(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        [geometryResource](ID3D12GraphicsCommandList *list) {
            RenderProfiler::ScopedEvent profile("LOD Selection", list);
            Bind(list, geometryResource);
            list->SetPipelineState(Data().LODPSO.Get());
            if (Data().Parameters.Slots)
            {
                list->Dispatch((Data().Parameters.Slots + 63) / 64, 1, 1);
            }
        });
    if (state.Options.DebugReadback || state.Options.Bounds || state.Options.ScreenRectangles)
    {
        AllocateReadback(frame.Readback, sizeof(Visibility::Statistics));
        const uint32_t records = state.Options.Bounds || state.Options.ScreenRectangles ? (min)(slots, 4096u) : 0;
        if (records)
        {
            AllocateReadback(frame.RecordReadback, 4096 * 16);
        }
        graph.AddPass(
            "Visibility Debug Readback",
            [=](RenderGraph::PassBuilder &builder) {
                builder.Read(counters, D3D12_RESOURCE_STATE_COPY_SOURCE);
                if (records)
                {
                    builder.Read(result, D3D12_RESOURCE_STATE_COPY_SOURCE);
                }
            },
            [records](ID3D12GraphicsCommandList *list) {
                auto &buffers = Frame();
                list->CopyBufferRegion(buffers.Readback.Get(), 0, buffers.Counters.Resource(), 0,
                                       sizeof(Visibility::Statistics));
                if (records)
                {
                    list->CopyBufferRegion(buffers.RecordReadback.Get(), 0, buffers.Result.Resource(), 0,
                                           uint64_t(records) * 16);
                }
                buffers.Pending = true;
                buffers.RecordCount = records;
            });
    }
    state.Reset = false;
    return result;
}
ID3D12Resource *ResultBuffer()
{
    return Frame().Result.Resource();
}
ID3D12Resource *StatisticsBuffer()
{
    return Frame().Counters.Resource();
}
const Visibility::Statistics &LatestStatistics()
{
    return Data().Latest;
}
bool HasStatisticsSample() { return Data().HasSample; }
bool HistoryValid()
{
    return Data().UsableHistory;
}
bool CameraCut()
{
    return Data().Cut;
}
void DrawDebug()
{
    auto &state = Data();
    auto &options = state.Options;
    ImGui::Checkbox("Enable Frustum Culling", &options.Frustum);
    ImGui::Checkbox("Enable Hi-Z Occlusion", &options.Occlusion);
    ImGui::Checkbox("Enable GPU LOD", &options.LOD);
    ImGui::DragFloat("Occlusion Depth Bias", &options.DepthBias, 0.0001f, 0, 0.1f, "%.5f");
    ImGui::SliderFloat("LOD Bias", &options.LODBias, -4, 4);
    ImGui::SliderInt("Force LOD (-1 = Auto)", &options.ForceLOD, -1, 7);
    ImGui::Combo("Debug shading", &options.DebugMode, "Off\0Visibility\0LOD\0");
    ImGui::Checkbox("Bounds overlay (debug readback)", &options.Bounds);
    ImGui::Checkbox("Screen rectangles (debug readback)", &options.ScreenRectangles);
    ImGui::Checkbox("Culling statistics (delayed debug readback)", &options.DebugReadback);
    ImGui::Text("Hi-Z: %ux%u / %u mips / history %s", OcclusionCulling::GetWidth(), OcclusionCulling::GetHeight(),
                OcclusionCulling::GetMipCount(), state.UsableHistory ? "valid" : "disabled");
    if (!state.UsableHistory)
    {
        ImGui::Text("History reset: %s", state.Reason);
    }
    if (OcclusionCulling::HasPrevious())
    {
        const int maximum = int(OcclusionCulling::GetMipCount()) - 1;
        options.HiZMip = clamp(options.HiZMip, 0, maximum);
        ImGui::SliderInt("Hi-Z Mip", &options.HiZMip, 0, maximum);
        OcclusionCulling::SetDebugPreview(uint32_t(options.HiZMip), true);
        ImGui::Image((ImTextureID)OcclusionCulling::GetDebugMipSrv(uint32_t(options.HiZMip)).ptr, ImVec2(320, 180));
    }
    if (options.DebugReadback)
    {
        const auto &stats = state.Latest;
        ImGui::Text("Total %u / Frustum culled %u / Frustum visible %u", stats.Total, stats.FrustumCulled,
                    stats.FrustumVisible);
        ImGui::Text("Occlusion tested %u / Culled %u / Final visible %u", stats.Tested, stats.Occluded,
                    stats.FinalVisible);
        ImGui::Text("Triangles %u / Index vertex invocations %u", stats.Triangles, stats.IndexVertices);
        for (uint32_t i = 0; i < Visibility::MaxLODs; ++i)
        {
            ImGui::Text("LOD%u: %u", i, stats.LODCounts[i]);
        }
    }
    for (const auto &pass : RenderProfiler::GetLatest().Passes)
    {
        if (pass.Name == "Hi-Z Build" || pass.Name == "GPU Scene Culling" || pass.Name == "Occlusion Culling" ||
            pass.Name == "LOD Selection" || pass.Name == "Generate Indirect Commands" ||
            pass.Name == "Opaque ExecuteIndirect")
        {
            ImGui::Text("%s: %.4f ms GPU", pass.Name.c_str(), pass.GpuMs);
        }
    }
    ImGui::TextDisabled("Bounds preview shows up to 4096 instance slots; cyan visible, red frustum, orange occluded.");
    if (options.Bounds || options.ScreenRectangles)
    {
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 extent(320, 180);
        draw->AddRect(origin, ImVec2(origin.x + extent.x, origin.y + extent.y), IM_COL32(100, 100, 100, 255));
        XMMATRIX vp = XMLoadFloat4x4(&state.Parameters.View) * XMLoadFloat4x4(&state.Parameters.Projection);
        for (const auto &record : state.DebugRecords)
        {
            if (!record.State)
            {
                continue;
            }
            const auto *instance = GpuScene::GetInstance(record.InstanceID);
            if (!instance)
            {
                continue;
            }
            ImVec2 points[8];
            bool valid = true;
            float minX = 1, minY = 1, maxX = 0, maxY = 0;
            for (uint32_t i = 0; i < 8; ++i)
            {
                const auto &c = instance->BoundsCenter;
                const auto &e = instance->BoundsExtents;
                XMFLOAT4 clip;
                XMStoreFloat4(&clip,
                              XMVector4Transform(XMVectorSet(c.x + ((i & 1) ? e.x : -e.x), c.y + ((i & 2) ? e.y : -e.y),
                                                             c.z + ((i & 4) ? e.z : -e.z), 1),
                                                 vp));
                if (clip.w <= 0 || clip.z <= 0)
                {
                    valid = false;
                    break;
                }
                const float x = clip.x / clip.w * 0.5f + 0.5f, y = 0.5f - clip.y / clip.w * 0.5f;
                minX = (min)(minX, x);
                minY = (min)(minY, y);
                maxX = (max)(maxX, x);
                maxY = (max)(maxY, y);
                points[i] = ImVec2(origin.x + x * extent.x, origin.y + y * extent.y);
            }
            if (!valid || minX < 0 || minY < 0 || maxX > 1 || maxY > 1)
            {
                continue;
            }
            const ImU32 color = record.State == 1   ? IM_COL32(40, 230, 230, 170)
                                : record.State == 2 ? IM_COL32(255, 40, 40, 170)
                                                    : IM_COL32(255, 160, 40, 170);
            if (options.ScreenRectangles)
            {
                draw->AddRect(ImVec2(origin.x + minX * extent.x, origin.y + minY * extent.y),
                              ImVec2(origin.x + maxX * extent.x, origin.y + maxY * extent.y), color);
            }
            if (options.Bounds)
            {
                for (uint32_t i = 0; i < 8; ++i)
                {
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        if ((i & (1u << axis)) == 0)
                        {
                            draw->AddLine(points[i], points[i | (1u << axis)], color);
                        }
                    }
                }
            }
        }
        ImGui::Dummy(extent);
    }
}
void Shutdown()
{
    Data() = {};
}
} // namespace GPUVisibility
