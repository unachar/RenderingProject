#include "pch.h"
#include "virtualgeometrytraversal.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "gpuscene/gpuscene.h"
#include "frameconstants.h"
#include "camera.h"
#include "visibility/gpuvisibility.h"
#include "occlusionculling.h"
#include "renderprofiler.h"
#include "rendercapture.h"
#include "shaderpipelines.h"
#include "virtualgeometrystreaming.h"
#include "renderersettings.h"
#include "world.h"
#include <numeric>
#include <cmath>

namespace VirtualGeometryTraversal
{
namespace
{
struct Constants
{
    XMFLOAT4X4 VP{}, PreviousVP{};
    XMFLOAT4 Camera{};
    uint32_t Capacity = 0, ObjectCapacity = 0, Assets = 0, Materials = 0;
    uint32_t Frustum = 1, Cone = 1, Occlusion = 0, Enabled = 0;
    float Bias = 0.001f, Width = 1, Height = 1;
    uint32_t Mips = 1;
    float ProjectionPixels = 1, TargetPixels = 1;
    int32_t ForceLevel = -1;
    uint32_t StateCapacity = 0;
    uint32_t PageCount = 0, RequestCapacity = 0, StreamingPadding[2]{};
    uint32_t HistorySlots = 0, HistoryCapacity = 0, ResetHistory = 0, TwoPass = 0;
    float HysteresisFraction = 0.1f;
    uint32_t HysteresisEnabled = 0, TemporalPhase = 0, OrthographicProjection = 0;
};

static_assert(sizeof(Constants) == 256);
static_assert(offsetof(Constants, OrthographicProjection) == 252);

struct HistoryHeader
{
    uint64_t Generation = 0;
    uint32_t Asset = UINT32_MAX, GroupBase = 0, GroupCount = 0, Epoch = 0, Padding[2]{};
};

static_assert(sizeof(HistoryHeader) == 32);

struct Frame
{
    GpuSceneBuffer States, Visible, Counters, Arguments, Success, Queue;
    GpuSceneBuffer HistoryHeaders, First, Deferred, Second, Combined, TemporalCounters, TemporalArguments;
    ComPtr<ID3D12Resource> TemporalReadback;
    uint64_t SampleFence = 0, SampleSerial = 0;
    bool SampleTwoPass = false;
};

struct State
{
    Settings Options;
    Constants Parameters;
    FrameInputs Inputs;
    std::array<Frame, GpuSceneBuffer::FrameCount> Frames;
    uint32_t FrameIndex = 0;
    bool Active = false;
    bool HistoryDirty = true, PreviousHysteresis = false, PreviousEnabled = false;
    uint32_t HistoryEpoch = 1, PreviousScratch = 0;
    uint64_t Serial = 0, LatestSample = 0;
    GpuSceneBuffer History;
    std::vector<HistoryHeader> HistoryLayout;
    TemporalStatistics TemporalStats;
    ComPtr<ID3D12RootSignature> Root;
    ComPtr<ID3D12PipelineState> ResetPSO, TraversePSO, ArgumentsPSO;
    ComPtr<ID3D12PipelineState> PartitionPSO, RetestPSO, TemporalArgumentsPSO;
    std::array<uint32_t, 64> Debug{};
};

State& Data()
{
    static State state;
    return state;
}

Frame& Current()
{
    return Data().Frames.at(Data().FrameIndex);
}

void Check(HRESULT result)
{
    if (FAILED(result))
    {
        throw std::runtime_error("Native VirtualGeometry compute initialization failed");
    }
}

void Initialize()
{
    auto& state = Data();
    if (state.ArgumentsPSO)
    {
        return;
    }
    CD3DX12_ROOT_PARAMETER parameters[29];
    for (uint32_t index = 0; index < 9; ++index)
    {
        parameters[index].InitAsShaderResourceView(index);
    }
    for (uint32_t index = 0; index < 6; ++index)
    {
        parameters[9 + index].InitAsUnorderedAccessView(index);
    }
    CD3DX12_DESCRIPTOR_RANGE hiz, cbv;
    hiz.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 9);
    cbv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    parameters[15].InitAsDescriptorTable(1, &hiz);
    parameters[16].InitAsDescriptorTable(1, &cbv);
    parameters[17].InitAsShaderResourceView(10);
    for (uint32_t index = 0; index < 3; ++index)
    {
        parameters[18 + index].InitAsUnorderedAccessView(6 + index);
    }
    parameters[21].InitAsUnorderedAccessView(9);
    parameters[22].InitAsShaderResourceView(11);
    for (uint32_t index = 0; index < 6; ++index)
    {
        parameters[23 + index].InitAsUnorderedAccessView(10 + index);
    }
    CD3DX12_ROOT_SIGNATURE_DESC description(29, parameters);
    ComPtr<ID3DBlob> blob, error;
    Check(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
    Check(GraphicsDevice::GetDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&state.Root)));
    auto pipeline = [&](const char* path, ComPtr<ID3D12PipelineState>& target) {
        ComPtr<ID3DBlob> shader;
        ShaderDescription input{};
        input.csoPath = path;
        input.ppBlob = shader.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(input))
        {
            throw std::runtime_error(path);
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = state.Root.Get();
        desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        Check(GraphicsDevice::GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&target)));
    };
    pipeline("shader/hlsl/build/VirtualGeometryResetCS.cso", state.ResetPSO);
    pipeline("shader/hlsl/build/VirtualGeometryTraversalCS.cso", state.TraversePSO);
    pipeline("shader/hlsl/build/VirtualGeometryArgumentsCS.cso", state.ArgumentsPSO);
    pipeline("shader/hlsl/build/VirtualGeometryPartitionCS.cso", state.PartitionPSO);
    pipeline("shader/hlsl/build/VirtualGeometryRetestCS.cso", state.RetestPSO);
    pipeline("shader/hlsl/build/VirtualGeometryTemporalArgumentsCS.cso", state.TemporalArgumentsPSO);
}

RenderGraph::ResourceHandle Import(RenderGraph& graph, const char* name, GpuSceneBuffer& buffer,
                                   D3D12_RESOURCE_STATES final)
{
    auto existing = graph.FindImportedResource(buffer.Resource());
    if (existing.IsValid())
    {
        return existing;
    }
    RenderGraph::ImportedResource resource{};
    resource.Name = name;
    resource.Resource = buffer.Resource();
    resource.InitialState = buffer.State;
    resource.HasFinalState = true;
    resource.FinalState = final;
    buffer.State = final;
    return graph.ImportResource(resource);
}

void Bind(ID3D12GraphicsCommandList* list, uint32_t temporalPhase = 0)
{
    auto& state = Data();
    auto& frame = Current();
    ID3D12DescriptorHeap* heap = FrameConstants::GetCbvHeap();
    list->SetDescriptorHeaps(1, &heap);
    list->SetComputeRootSignature(state.Root.Get());
    ID3D12Resource* sources[] = {GpuScene::InstanceBuffer(),
                                 state.Inputs.ObjectResource,
                                 VirtualGeometryResources::AssetBuffer().Resource(),
                                 VirtualGeometryResources::ClusterBuffer().Resource(),
                                 VirtualGeometryResources::GroupBuffer().Resource(),
                                 VirtualGeometryResources::HierarchyIndexBuffer().Resource(),
                                 VirtualGeometryResources::RootBuffer().Resource(),
                                 state.Inputs.CountResource,
                                 GpuScene::MaterialBuffer()};
    for (uint32_t index = 0; index < 9; ++index)
    {
        list->SetComputeRootShaderResourceView(index, sources[index]->GetGPUVirtualAddress());
    }
    GpuSceneBuffer* outputs[] = {&frame.States,    &frame.Visible, &frame.Counters,
                                 &frame.Arguments, &frame.Success, &frame.Queue};
    for (uint32_t index = 0; index < 6; ++index)
    {
        list->SetComputeRootUnorderedAccessView(9 + index, outputs[index]->Resource()->GetGPUVirtualAddress());
    }
    list->SetComputeRootDescriptorTable(15, temporalPhase == 2 ? OcclusionCulling::GetCurrentSrv()
                                                               : OcclusionCulling::GetPreviousSrv());
    auto constants = state.Parameters;
    constants.TemporalPhase = temporalPhase;
    if (temporalPhase == 0 && constants.TwoPass)
    {
        constants.Occlusion = 0;
    }
    if (temporalPhase == 2)
    {
        constants.Occlusion = OcclusionCulling::HasCurrent();
    }
    list->SetComputeRootDescriptorTable(16,
                                        FrameConstants::AllocateTransientConstantBuffer(&constants, sizeof(constants)));
    list->SetComputeRootShaderResourceView(
        17, VirtualGeometryStreaming::ResidencyBuffer().Resource()->GetGPUVirtualAddress());
    GpuSceneBuffer* feedback[] = {&VirtualGeometryStreaming::RequestBitsBuffer(),
                                  &VirtualGeometryStreaming::RequestBuffer(), &VirtualGeometryStreaming::UsageBuffer()};
    for (uint32_t index = 0; index < 3; ++index)
    {
        list->SetComputeRootUnorderedAccessView(18 + index, feedback[index]->Resource()->GetGPUVirtualAddress());
    }
    list->SetComputeRootUnorderedAccessView(21, state.History.Resource()->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(22, frame.HistoryHeaders.Resource()->GetGPUVirtualAddress());
    GpuSceneBuffer* temporal[] = {&frame.First,    &frame.Deferred,         &frame.Second,
                                  &frame.Combined, &frame.TemporalCounters, &frame.TemporalArguments};
    for (uint32_t index = 0; index < 6; ++index)
    {
        list->SetComputeRootUnorderedAccessView(23 + index, temporal[index]->Resource()->GetGPUVirtualAddress());
    }
}

void PollTemporalStatistics()
{
    auto& state = Data();
    uint64_t completed = GraphicsDevice::GetCompletedFence();
    for (auto& frame : state.Frames)
    {
        if (!frame.SampleFence || frame.SampleFence > completed)
        {
            continue;
        }
        void* mapped = nullptr;
        D3D12_RANGE range{0, 1024};
        Check(frame.TemporalReadback->Map(0, &range, &mapped));
        if (frame.SampleSerial > state.LatestSample)
        {
            const auto* values = static_cast<const uint32_t*>(mapped);
            state.TemporalStats.Candidates = values[0];
            state.TemporalStats.FirstVisible = frame.SampleTwoPass ? values[1] : values[193];
            state.TemporalStats.LodTransitions = values[4];
            state.TemporalStats.SecondVisible = values[65];
            state.TemporalStats.CapacityFallbacks = values[195];
            state.TemporalStats.HasSample = true;
            state.TemporalStats.SampleSerial = frame.SampleSerial;
            std::copy_n(values + 192, 64, state.Debug.begin());
            if (state.Options.AutoGrowBuffers && state.TemporalStats.CapacityFallbacks != 0)
            {
                // Applied before BeginFrame/Ensure, using only fence-completed feedback.
                state.Options.MaxSelectedClusters =
                    static_cast<uint32_t>(std::min<uint64_t>(4000000, uint64_t(state.Options.MaxSelectedClusters) * 2));
                state.Options.MaxGroupStates =
                    static_cast<uint32_t>(std::min<uint64_t>(4000000, uint64_t(state.Options.MaxGroupStates) * 2));
            }
            state.LatestSample = frame.SampleSerial;
        }
        D3D12_RANGE noWrite{0, 0};
        frame.TemporalReadback->Unmap(0, &noWrite);
        frame.SampleFence = 0;
    }
}

uint64_t PrepareHistory(RenderGraph& graph)
{
    auto& state = Data();
    auto& frame = Current();
    const auto& assets = VirtualGeometryResources::AssetTable();
    std::vector<HistoryHeader> headers(std::max(1u, GpuScene::Statistics().InstanceSlots));
    uint32_t used = 0;
    bool changed = state.HistoryDirty || GPUVisibility::CameraCut() || state.PreviousEnabled != state.Options.Enabled ||
                   state.PreviousHysteresis != state.Options.HysteresisEnabled ||
                   state.PreviousScratch != state.Options.MaxGroupStates;
    for (uint32_t id = 0; id < headers.size(); ++id)
    {
        const auto* instance = GpuScene::GetInstance(id);
        if (!instance || instance->VirtualGeometryAssetIndex >= assets.size())
        {
            continue;
        }
        auto& header = headers[id];
        header.Generation = GpuScene::InstanceHistoryGeneration(id);
        header.Asset = instance->VirtualGeometryAssetIndex;
        uint32_t groups = assets[header.Asset].GroupCount;
        if (groups <= state.Options.MaxGroupStates - used)
        {
            header.GroupBase = used;
            header.GroupCount = groups;
            used += groups;
        }
    }
    if (headers.size() != state.HistoryLayout.size())
    {
        changed = true;
    }
    for (size_t id = 0; id < headers.size() && id < state.HistoryLayout.size(); ++id)
    {
        // Compare identity/layout only, excluding the epoch assigned below.
        if (std::memcmp(&headers[id], &state.HistoryLayout[id], offsetof(HistoryHeader, Epoch)) != 0)
        {
            changed = true;
        }
    }
    state.History.BeginFrame(state.FrameIndex);
    changed |= state.History.Ensure(GraphicsDevice::GetDevice(), std::max(1u, used), 4, state.FrameIndex,
                                    L"Native LOD history", true);
    if (changed)
    {
        state.HistoryEpoch = state.HistoryEpoch == 0x7fffffffu ? 1 : state.HistoryEpoch + 1;
    }
    for (auto& header : headers)
    {
        header.Epoch = state.HistoryEpoch;
    }
    state.HistoryLayout = headers;
    state.HistoryDirty = false;
    state.PreviousEnabled = state.Options.Enabled;
    state.PreviousHysteresis = state.Options.HysteresisEnabled;
    state.PreviousScratch = state.Options.MaxGroupStates;
    state.Parameters.HistorySlots = uint32_t(headers.size());
    state.Parameters.HistoryCapacity = used;
    state.Parameters.ResetHistory = changed;
    frame.HistoryHeaders.BeginFrame(state.FrameIndex);
    frame.HistoryHeaders.Ensure(GraphicsDevice::GetDevice(), uint32_t(headers.size()) * 2, 16, state.FrameIndex,
                                L"Native LOD identities");
    std::vector<uint32_t> changes(headers.size() * 2);
    std::iota(changes.begin(), changes.end(), 0u);
    uint64_t bytes = frame.HistoryHeaders.Stage(GraphicsDevice::GetDevice(), state.FrameIndex, headers.data(),
                                                uint32_t(changes.size()), changes);
    auto headerHandle =
        Import(graph, "Native LOD identities", frame.HistoryHeaders, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (bytes)
    {
        graph.AddPass(
            "Native LOD identity upload",
            [=](RenderGraph::PassBuilder& b) {
                b.Write(headerHandle, D3D12_RESOURCE_STATE_COPY_DEST);
            },
            [](ID3D12GraphicsCommandList* list) {
                Current().HistoryHeaders.Copy(list, Data().FrameIndex);
            });
    }
    return bytes;
}
} // namespace

Settings& GetSettings()
{
    return Data().Options;
}

bool Active()
{
    return Data().Active;
}

bool TwoPassActive()
{
    return Data().Active && Data().Parameters.TwoPass != 0;
}

void InvalidateHistory()
{
    Data().HistoryDirty = true;
}

const TemporalStatistics& GetTemporalStatistics()
{
    return Data().TemporalStats;
}

GpuSceneBuffer& VisibleBuffer(RasterPhase phase)
{
    if (!TwoPassActive())
    {
        return Current().Visible;
    }
    if (phase == RasterPhase::First)
    {
        return Current().First;
    }
    if (phase == RasterPhase::Second)
    {
        return Current().Second;
    }
    return Current().Combined;
}

GpuSceneBuffer& CounterBuffer(RasterPhase phase)
{
    return TwoPassActive() ? Current().TemporalCounters : Current().Counters;
}

GpuSceneBuffer& ArgumentBuffer(RasterPhase phase)
{
    return TwoPassActive() ? Current().TemporalArguments : Current().Arguments;
}

uint64_t CounterOffset(RasterPhase phase)
{
    return TwoPassActive() ? uint64_t(phase) * 256 : 0;
}

uint64_t ArgumentOffset(RasterPhase phase)
{
    return TwoPassActive() ? uint64_t(phase) * 12 : 0;
}

GpuSceneBuffer& VisibleBuffer()
{
    return Current().Visible;
}

GpuSceneBuffer& CounterBuffer()
{
    return Current().Counters;
}

GpuSceneBuffer& ArgumentBuffer()
{
    return Current().Arguments;
}

GpuSceneBuffer& SuccessBuffer()
{
    return Current().Success;
}

const std::array<uint32_t, 64>& DebugCounters()
{
    return Data().Debug;
}

void Prepare(RenderGraph& graph, const FrameInputs& inputs)
{
    auto& state = Data();
    PollTemporalStatistics();
    ++state.Serial;
    state.Active = false;
    state.FrameIndex = GraphicsDevice::GetFrameIndex();
    state.Inputs = inputs;
    auto& frame = Current();
    uint64_t maximumClusters = 1;
    for (const auto& asset : VirtualGeometryResources::AssetTable())
    {
        maximumClusters = std::max(maximumClusters, uint64_t(std::max(asset.ClusterCount, asset.GroupCount)));
    }
    uint64_t maximumWork = maximumClusters * inputs.ObjectCapacity;
    auto ensure = [&](GpuSceneBuffer& buffer, uint32_t count, uint32_t stride) {
        buffer.BeginFrame(state.FrameIndex);
        buffer.Ensure(GraphicsDevice::GetDevice(), count, stride, state.FrameIndex, L"Native VirtualGeometry frame",
                      true);
    };
    // Always reset routing, including disabled/failed native frames.
    ensure(frame.Success, inputs.ObjectCapacity, 4);
    ensure(frame.Counters, 64, 4);
    ensure(frame.Arguments, 1, 12);
    state.Options.MaxSelectedClusters = std::clamp(state.Options.MaxSelectedClusters, 1u, 4000000u);
    state.Options.MaxGroupStates = std::clamp(state.Options.MaxGroupStates, 1u, 4000000u);
    uint32_t scratch =
        state.Options.Enabled ? uint32_t(std::min(uint64_t(state.Options.MaxGroupStates), maximumWork)) : 1;
    uint32_t selectedCapacity =
        state.Options.Enabled ? uint32_t(std::min(uint64_t(state.Options.MaxSelectedClusters), maximumWork)) : 1;
    ensure(frame.States, scratch, 4);
    ensure(frame.Queue, scratch, 4);
    ensure(frame.Visible, selectedCapacity, 16);
    bool twoPass = state.Options.Enabled && state.Options.TwoPassOcclusion && state.Options.OcclusionCulling &&
                   RendererSettings::GetTwoPhaseOcclusionEnabled() && OcclusionCulling::IsAvailable();
    uint32_t phaseCapacity = twoPass ? selectedCapacity : 1;
    for (auto* buffer : {&frame.First, &frame.Deferred, &frame.Second, &frame.Combined})
    {
        ensure(*buffer, phaseCapacity, 16);
    }
    ensure(frame.TemporalCounters, 192, 4);
    ensure(frame.TemporalArguments, 3, 12);
    VirtualGeometryResources::Prepare(graph);
    PrepareHistory(graph);
    Initialize();
    auto& p = state.Parameters;
    XMMATRIX view, projection;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    XMStoreFloat4x4(&p.VP, view * projection);
    XMStoreFloat4x4(&p.PreviousVP, XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()) *
                                       XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix()));
    XMStoreFloat4(&p.Camera, XMMatrixInverse(nullptr, view).r[3]);
    p.Camera.w = 0;
    p.StreamingPadding[0] = p.StreamingPadding[1] = 0;
    if (state.Options.PredictiveStreaming && !GPUVisibility::CameraCut() && state.Options.ForceHierarchyLevel < 0)
    {
        const auto currentWorld = XMMatrixInverse(nullptr, view);
        const auto previousWorld = XMMatrixInverse(nullptr, XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()));
        const float seconds = static_cast<float>(std::clamp(World::GetFrameTimeMs() * 0.001, 0.001, 0.1));
        const auto velocity = (currentWorld.r[3] - previousWorld.r[3]) / seconds;
        const auto predictedOffset =
            velocity * std::clamp(state.Options.PrefetchSeconds, 0.0f, 2.0f) + currentWorld.r[2] * 2.0f;
        const float length = XMVectorGetX(XMVector3Length(predictedOffset));
        if (std::isfinite(length) && length > 0.001f && std::isfinite(state.Options.MaxPrefetchDistance))
        {
            XMFLOAT3 direction;
            XMStoreFloat3(&direction, XMVector3Normalize(predictedOffset));
            auto pack = [](float value) {
                return static_cast<uint32_t>(std::clamp((value * 0.5f + 0.5f) * 1023.0f, 0.0f, 1023.0f) + 0.5f);
            };
            p.StreamingPadding[0] = pack(direction.x) | (pack(direction.y) << 10) | (pack(direction.z) << 20);
            p.StreamingPadding[1] = 1;
            p.Camera.w = std::min(length, std::max(0.0f, state.Options.MaxPrefetchDistance));
        }
    }
    XMFLOAT4X4 projectionValues;
    XMStoreFloat4x4(&projectionValues, projection);
    p.ProjectionPixels = 0.5f * float(GraphicsDevice::GetSceneHeight()) * std::abs(projectionValues._22);
    p.OrthographicProjection = 0;
    p.TargetPixels = std::max(0.01f, state.Options.TargetErrorPixels);
    p.ForceLevel = std::clamp(state.Options.ForceHierarchyLevel, -1, 31);
    p.Capacity = selectedCapacity;
    p.StateCapacity = scratch;
    p.ObjectCapacity = inputs.ObjectCapacity;
    p.Assets = VirtualGeometryResources::AssetCount();
    p.PageCount = VirtualGeometryStreaming::PageCount();
    p.RequestCapacity = VirtualGeometryStreaming::RequestCapacity;
    p.Materials = GpuScene::Statistics().Materials;
    p.Enabled = state.Options.Enabled;
    p.TwoPass = twoPass;
    p.HysteresisEnabled = p.Enabled && state.Options.HysteresisEnabled;
    p.HysteresisFraction = std::isfinite(state.Options.HysteresisFraction)
                               ? std::clamp(state.Options.HysteresisFraction, 0.0f, 0.5f)
                               : 0.1f;
    p.Frustum = state.Options.FrustumCulling;
    p.Cone = state.Options.ConeCulling;
    p.Occlusion =
        state.Options.OcclusionCulling && GPUVisibility::HistoryValid() && !GPUVisibility::GetSettings().FreezeCamera;
    p.Width = float(std::max(1u, OcclusionCulling::GetWidth()));
    p.Height = float(std::max(1u, OcclusionCulling::GetHeight()));
    p.Mips = std::max(1u, OcclusionCulling::GetMipCount());
    auto stats = Import(graph, "Nanite Counters", frame.Counters, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto success =
        Import(graph, "Nanite Instance Routing", frame.Success, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto visible =
        Import(graph, "Visible Virtual Clusters", frame.Visible, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto arguments =
        Import(graph, "Nanite Dispatch Arguments", frame.Arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    auto states = Import(graph, "Nanite Group States", frame.States, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto queue = Import(graph, "Nanite Traversal Queue", frame.Queue, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto history = Import(graph, "Native LOD history", state.History, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto historyHeaders =
        Import(graph, "Native LOD identities", frame.HistoryHeaders, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto temporalCounters = Import(graph, "Native temporal counters", frame.TemporalCounters,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto residency = Import(graph, "Geometry Residency", VirtualGeometryStreaming::ResidencyBuffer(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    std::vector<RenderGraph::ResourceHandle> feedback;
    for (auto* buffer : {&VirtualGeometryStreaming::RequestBitsBuffer(), &VirtualGeometryStreaming::RequestBuffer(),
                         &VirtualGeometryStreaming::UsageBuffer()})
    {
        feedback.push_back(Import(graph, "Geometry Feedback", *buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    }
    std::vector<RenderGraph::ResourceHandle> assetHandles;
    GpuSceneBuffer* assets[] = {&VirtualGeometryResources::AssetBuffer(), &VirtualGeometryResources::ClusterBuffer(),
                                &VirtualGeometryResources::GroupBuffer(),
                                &VirtualGeometryResources::HierarchyIndexBuffer(),
                                &VirtualGeometryResources::RootBuffer()};
    for (auto* buffer : assets)
    {
        assetHandles.push_back(
            Import(graph, "Nanite Asset SRV", *buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }
    auto external = [&](ID3D12Resource* resource, const char* name, D3D12_RESOURCE_STATES read) {
        auto existing = graph.FindImportedResource(resource);
        if (existing.IsValid())
        {
            return existing;
        }
        RenderGraph::ImportedResource input{};
        input.Name = name;
        input.Resource = resource;
        input.InitialState = input.FinalState = read;
        input.HasFinalState = true;
        return graph.ImportResource(input);
    };
    auto material =
        external(GpuScene::MaterialBuffer(), "Nanite Materials", D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RenderGraph::ResourceHandle hiz;
    if (OcclusionCulling::GetPreviousResource())
    {
        hiz = external(OcclusionCulling::GetPreviousResource(), "Nanite Hi-Z", OcclusionCulling::ReadState());
    }
    graph.AddPass(
        "Nanite Reset",
        [=](RenderGraph::PassBuilder& b) {
            b.Write(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS).Write(success, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            b.Write(temporalCounters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .ReadWrite(history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (auto handle : feedback)
            {
                b.Write(handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        },
        [](ID3D12GraphicsCommandList* list) {
            Bind(list);
            list->SetPipelineState(Data().ResetPSO.Get());
            uint32_t resetCount = std::max({192u, Data().Inputs.ObjectCapacity, (Data().Parameters.PageCount + 31) / 32,
                                            Data().Parameters.ResetHistory ? Data().Parameters.HistoryCapacity : 0u});
            list->Dispatch(std::max(1u, (resetCount + 63) / 64), 1, 1);
        });
    graph.AddPass(
        "Nanite Hierarchy Traversal",
        [=](RenderGraph::PassBuilder& b) {
            b.Read(inputs.Instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(inputs.Objects, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(inputs.Count, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(material, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            for (auto handle : assetHandles)
            {
                b.Read(handle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            b.Read(residency, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            b.Read(historyHeaders, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .ReadWrite(history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .ReadWrite(temporalCounters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (auto handle : feedback)
            {
                b.ReadWrite(handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (hiz.IsValid())
            {
                b.Read(hiz, OcclusionCulling::ReadState());
            }
            b.ReadWrite(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .Write(success, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .Write(visible, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .Write(states, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .Write(queue, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        [](ID3D12GraphicsCommandList* list) {
            RenderProfiler::ScopedEvent profile("Nanite Hierarchy Traversal", list);
            Bind(list);
            list->SetPipelineState(Data().TraversePSO.Get());
            uint32_t count = Data().Inputs.ObjectCapacity;
            uint32_t x = std::min(std::max(1u, count), 65535u);
            list->Dispatch(x, std::max(1u, (count + x - 1) / x), 1);
        });
    graph.AddPass(
        "Nanite Indirect Dispatch",
        [=](RenderGraph::PassBuilder& b) {
            b.ReadWrite(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .Write(arguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        [](ID3D12GraphicsCommandList* list) {
            Bind(list);
            list->SetPipelineState(Data().ArgumentsPSO.Get());
            list->Dispatch(1, 1, 1);
        });
    state.Active = true;
    state.TemporalStats.HistoryBytes = state.History.MemoryBytes();
    state.TemporalStats.CandidateBytes = 0;
    for (const auto& item : state.Frames)
    {
        state.TemporalStats.HistoryBytes += item.HistoryHeaders.MemoryBytes();
        state.TemporalStats.CandidateBytes += item.First.MemoryBytes() + item.Deferred.MemoryBytes() +
                                              item.Second.MemoryBytes() + item.Combined.MemoryBytes() +
                                              item.TemporalCounters.MemoryBytes() +
                                              item.TemporalArguments.MemoryBytes() + (item.TemporalReadback ? 1024 : 0);
    }
    RenderProfiler::GeometryTemporalStatistics sample{};
    sample.FirstVisible = state.TemporalStats.FirstVisible;
    sample.Candidates = state.TemporalStats.Candidates;
    sample.SecondVisible = state.TemporalStats.SecondVisible;
    sample.LodTransitions = state.TemporalStats.LodTransitions;
    sample.CapacityFallbacks = state.TemporalStats.CapacityFallbacks;
    sample.HistoryBytes = state.TemporalStats.HistoryBytes;
    sample.CandidateBytes = state.TemporalStats.CandidateBytes;
    sample.HasSample = state.TemporalStats.HasSample;
    RenderProfiler::RecordGeometryTemporal(sample);
    if (TwoPassActive())
    {
        std::vector<RenderGraph::ResourceHandle> temporal;
        for (auto* buffer : {&frame.First, &frame.Deferred, &frame.Second, &frame.Combined})
        {
            temporal.push_back(
                Import(graph, "Native phase clusters", *buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        }
        auto temporalArgs =
            Import(graph, "Native phase arguments", frame.TemporalArguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        graph.AddPass(
            "Native temporal partition",
            [=](RenderGraph::PassBuilder& b) {
                b.Read(inputs.Instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(visible, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .Read(stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .ReadWrite(temporalCounters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                for (auto handle : assetHandles)
                {
                    b.Read(handle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                if (hiz.IsValid())
                {
                    b.Read(hiz, OcclusionCulling::ReadState());
                }
                for (auto handle : temporal)
                {
                    b.Write(handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
            },
            [](ID3D12GraphicsCommandList* list) {
                RenderProfiler::ScopedEvent profile("Native temporal partition", list);
                Bind(list, 1);
                list->SetPipelineState(Data().PartitionPSO.Get());
                list->Dispatch((Data().Parameters.Capacity + 63) / 64, 1, 1);
            });
        graph.AddPass(
            "Native phase dispatch arguments",
            [=](RenderGraph::PassBuilder& b) {
                b.Read(temporalCounters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .Write(temporalArgs, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            },
            [](ID3D12GraphicsCommandList* list) {
                Bind(list, 1);
                list->SetPipelineState(Data().TemporalArgumentsPSO.Get());
                list->Dispatch(1, 1, 1);
            });
    }
}

void RecordFeedback(RenderGraph& graph)
{
    if (!Active())
    {
        return;
    }
    auto& frame = Current();
    if (!frame.TemporalReadback)
    {
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        auto description = CD3DX12_RESOURCE_DESC::Buffer(1024);
        Check(GraphicsDevice::GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                   IID_PPV_ARGS(&frame.TemporalReadback)));
    }
    auto counters =
        Import(graph, "Native temporal sample", frame.TemporalCounters, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto routing =
        Import(graph, "Native routing sample", frame.Counters, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto* target = &frame;
    uint64_t serial = Data().Serial;
    graph.AddPass(
        "Native temporal feedback",
        [=](RenderGraph::PassBuilder& b) {
            b.Read(counters, D3D12_RESOURCE_STATE_COPY_SOURCE).Read(routing, D3D12_RESOURCE_STATE_COPY_SOURCE);
        },
        [=](ID3D12GraphicsCommandList* commands) {
            commands->CopyBufferRegion(target->TemporalReadback.Get(), 0, target->TemporalCounters.Resource(), 0, 768);
            commands->CopyBufferRegion(target->TemporalReadback.Get(), 768, target->Counters.Resource(), 0, 256);
            target->SampleFence = GraphicsDevice::GetPendingSubmissionFence();
            target->SampleSerial = serial;
            target->SampleTwoPass = TwoPassActive();
        });
    VirtualGeometryStreaming::RecordFeedbackCopy(graph);
}

void Retest(ID3D12GraphicsCommandList* commands)
{
    if (!Active())
    {
        return;
    }
    RenderGraph graph;
    if (TwoPassActive())
    {
        auto& frame = Current();
        auto deferred =
            Import(graph, "Native deferred clusters", frame.Deferred, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto second =
            Import(graph, "Native second-pass clusters", frame.Second, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto combined =
            Import(graph, "Native combined clusters", frame.Combined, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto counters = Import(graph, "Native phase counters", frame.TemporalCounters,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto arguments =
            Import(graph, "Native phase arguments", frame.TemporalArguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        auto clusters = Import(graph, "Native cluster bounds", VirtualGeometryResources::ClusterBuffer(),
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        RenderGraph::ImportedResource input{};
        input.Name = "Native instance transforms";
        input.Resource = GpuScene::InstanceBuffer();
        input.InitialState = input.FinalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        input.HasFinalState = true;
        auto instances = graph.ImportResource(input);
        RenderGraph::ResourceHandle hiz;
        if (OcclusionCulling::HasCurrent())
        {
            input.Name = "Native current Hi-Z";
            input.Resource = OcclusionCulling::GetCurrentResource();
            input.InitialState = input.FinalState = OcclusionCulling::ReadState();
            hiz = graph.ImportResource(input);
        }
        graph.AddPass(
            "Native current-depth retest",
            [=](RenderGraph::PassBuilder& b) {
                b.Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(clusters, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    .Read(deferred, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .ReadWrite(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .Write(second, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .ReadWrite(combined, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (hiz.IsValid())
                {
                    b.Read(hiz, OcclusionCulling::ReadState());
                }
            },
            [](ID3D12GraphicsCommandList* list) {
                RenderProfiler::ScopedEvent profile("Native current-depth retest", list);
                Bind(list, 2);
                list->SetPipelineState(Data().RetestPSO.Get());
                list->Dispatch((Data().Parameters.Capacity + 63) / 64, 1, 1);
            });
        graph.AddPass(
            "Native final dispatch arguments",
            [=](RenderGraph::PassBuilder& b) {
                b.Read(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    .Write(arguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            },
            [](ID3D12GraphicsCommandList* list) {
                Bind(list, 2);
                list->SetPipelineState(Data().TemporalArgumentsPSO.Get());
                list->Dispatch(1, 1, 1);
            });
    }
    RecordFeedback(graph);
    if (!graph.Compile() || !graph.Execute(commands))
    {
        throw std::runtime_error(graph.GetLastError());
    }
}

void EndFrame()
{
    if (Data().Active && Data().Options.DebugReadback)
    {
        std::vector<uint8_t> bytes;
        if (RenderCapture::ReadBuffer(Current().Counters.Resource(), Current().Counters.State, 256, bytes))
        {
            memcpy(Data().Debug.data(), bytes.data(), 256);
        }
    }
}

void Shutdown()
{
    VirtualGeometryResources::Shutdown();
    Data() = {};
}
} // namespace VirtualGeometryTraversal
