#include "pch.h"
#include "virtualgeometrybinning.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "gpuscene/gpuscene.h"
#include "renderprofiler.h"
#include "shaderpipelines.h"
#include <algorithm>

namespace VirtualGeometryBinning
{
namespace
{
struct Frame
{
    GpuSceneBuffer Bins, Visible, Arguments, Counters;
    ComPtr<ID3D12Resource> Readback;
    uint64_t Fence = 0, Serial = 0;
    uint32_t SampleMask = 0;
};
struct State
{
    Settings Options;
    Statistics Stats;
    std::string Reason;
    std::array<Frame,GpuSceneBuffer::FrameCount> Frames;
    std::array<uint64_t,3> SampleSerial{};
    ComPtr<ID3D12RootSignature> Root;
    ComPtr<ID3D12PipelineState> Reset, Count, Prefix, Scatter;
    uint32_t FrameIndex = 0, Capacity = 1, ExactBins = 1, Materials = 0;
    uint64_t Serial = 0;
    bool Available = false, Failed = false;
};
State& Data() { static State state; return state; }
Frame& Current() { return Data().Frames.at(Data().FrameIndex); }
void Check(HRESULT result)
{
    if (FAILED(result)) throw std::runtime_error("Virtual Geometry material bin allocation/pipeline failed");
}
void Initialize()
{
    auto& state = Data();
    if (state.Scatter) return;
    CD3DX12_ROOT_PARAMETER parameters[7];
    parameters[0].InitAsShaderResourceView(0);
    parameters[1].InitAsShaderResourceView(1);
    for (uint32_t index = 0; index < 4; ++index) parameters[2+index].InitAsUnorderedAccessView(index);
    parameters[6].InitAsConstants(4,0);
    CD3DX12_ROOT_SIGNATURE_DESC description(7,parameters);
    ComPtr<ID3DBlob> blob, error;
    Check(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    auto* device = GraphicsDevice::GetDevice();
    Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&state.Root)));
    auto pipeline = [&](const char* path, ComPtr<ID3D12PipelineState>& output)
    {
        ComPtr<ID3DBlob> shader;
        ShaderDescription input{};
        input.csoPath = path;
        input.ppBlob = shader.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(input)) throw std::runtime_error(path);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = state.Root.Get();
        desc.CS = {shader->GetBufferPointer(),shader->GetBufferSize()};
        Check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&output)));
    };
    pipeline("shader/hlsl/build/VirtualGeometryBinsResetCS.cso",state.Reset);
    pipeline("shader/hlsl/build/VirtualGeometryBinsCountCS.cso",state.Count);
    pipeline("shader/hlsl/build/VirtualGeometryBinsPrefixCS.cso",state.Prefix);
    pipeline("shader/hlsl/build/VirtualGeometryBinsScatterCS.cso",state.Scatter);
}
RenderGraph::ResourceHandle Import(RenderGraph& graph, const char* name, GpuSceneBuffer& buffer, D3D12_RESOURCE_STATES final)
{
    auto existing = graph.FindImportedResource(buffer.Resource());
    if (existing.IsValid()) return existing;
    RenderGraph::ImportedResource resource{};
    resource.Name = name;
    resource.Resource = buffer.Resource();
    resource.InitialState = buffer.State;
    resource.FinalState = final;
    resource.HasFinalState = true;
    buffer.State = final;
    return graph.ImportResource(resource);
}
void Bind(ID3D12GraphicsCommandList* commands, VirtualGeometryTraversal::RasterPhase phase)
{
    auto& state = Data();
    auto& frame = Current();
    commands->SetComputeRootSignature(state.Root.Get());
    commands->SetComputeRootShaderResourceView(0,VirtualGeometryTraversal::VisibleBuffer(phase).Resource()->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(1,VirtualGeometryTraversal::CounterBuffer(phase).Resource()->GetGPUVirtualAddress()
        +VirtualGeometryTraversal::CounterOffset(phase));
    GpuSceneBuffer* outputs[] = {&frame.Bins,&frame.Visible,&frame.Arguments,&frame.Counters};
    for (uint32_t index = 0; index < 4; ++index)
        commands->SetComputeRootUnorderedAccessView(2+index,outputs[index]->Resource()->GetGPUVirtualAddress());
    const uint32_t constants[] = {state.Capacity,state.ExactBins,state.Materials,0};
    commands->SetComputeRoot32BitConstants(6,4,constants,0);
}
void Poll()
{
    auto& state = Data();
    uint64_t completed = GraphicsDevice::GetCompletedFence();
    for (auto& frame : state.Frames)
    {
        if (!frame.Fence || frame.Fence > completed) continue;
        D3D12_RANGE range{0,96};
        void* mapped = nullptr;
        Check(frame.Readback->Map(0,&range,&mapped));
        const auto* values = static_cast<const uint32_t*>(mapped);
        for (uint32_t phase = 0; phase < 3; ++phase)
        {
            if ((frame.SampleMask&(1u<<phase)) == 0 || frame.Serial <= state.SampleSerial[phase]) continue;
            auto& sample = state.Stats.Phases[phase];
            sample.ActiveBins = values[phase*8];
            sample.Clusters = values[phase*8+1];
            sample.OverflowClusters = values[phase*8+2];
            sample.InvalidMaterials = values[phase*8+3];
            sample.RejectedClusters = values[phase*8+4]+values[phase*8+6];
            sample.HasSample = true;
            if (sample.OverflowClusters && Data().Options.MaxMaterialBins < 65535u)
            {
                Data().Options.MaxMaterialBins = std::min(65535u, Data().Options.MaxMaterialBins*2);
            }
            state.SampleSerial[phase] = frame.Serial;
        }
        D3D12_RANGE noWrite{0,0};
        frame.Readback->Unmap(0,&noWrite);
        frame.Fence = 0;
    }
}
}
Settings& GetSettings() { return Data().Options; }
const Statistics& GetStatistics() { return Data().Stats; }
const std::string& UnavailableReason() { return Data().Reason; }
bool Active() { return Data().Available; }
GpuSceneBuffer& VisibleBuffer() { return Current().Visible; }
GpuSceneBuffer& ArgumentBuffer() { return Current().Arguments; }
GpuSceneBuffer& CounterBuffer() { return Current().Counters; }
uint32_t MaximumCommands() { return Data().ExactBins+1; }

void BeginFrame(bool rasterAvailable)
{
    auto& state = Data();
    state.Available = false;
    state.FrameIndex = GraphicsDevice::GetFrameIndex();
    ++state.Serial;
    Poll();
    if (!state.Options.Enabled)
    {
        state.Failed = false;
        state.Reason = "Disabled";
        return;
    }
    if (!rasterAvailable)
    {
        state.Reason = "Binned raster shaders unavailable; using original dispatch";
        return;
    }
    if (state.Failed || !VirtualGeometryTraversal::Active() || !VirtualGeometryTraversal::GetSettings().Enabled) return;
    try
    {
        Initialize();
        auto& frame = Current();
        state.Options.MaxMaterialBins = std::clamp(state.Options.MaxMaterialBins,1u,65535u);
        state.Materials = GpuScene::Statistics().Materials;
        state.ExactBins = std::min(std::max(1u,state.Materials),state.Options.MaxMaterialBins);
        // All phase cuts are subsets of this pre-reserved full resident cut.
        state.Capacity = std::min(4000000u,VirtualGeometryTraversal::VisibleBuffer().Capacity());
        auto ensure = [&](GpuSceneBuffer& buffer, uint32_t count, uint32_t stride)
        {
            buffer.BeginFrame(state.FrameIndex);
            buffer.Ensure(GraphicsDevice::GetDevice(),count,stride,state.FrameIndex,L"Native material bins",true);
        };
        ensure(frame.Bins,state.ExactBins+1,16);
        ensure(frame.Visible,state.Capacity,16);
        ensure(frame.Arguments,state.ExactBins+1,sizeof(IndirectCommand));
        ensure(frame.Counters,8,4);
        if (!frame.Readback)
        {
            auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
            auto description = CD3DX12_RESOURCE_DESC::Buffer(96);
            Check(GraphicsDevice::GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&frame.Readback)));
        }
        frame.SampleMask = 0;
        state.Stats.BufferBytes = 0;
        for (const auto& item : state.Frames)
            state.Stats.BufferBytes += item.Bins.MemoryBytes()+item.Visible.MemoryBytes()+item.Arguments.MemoryBytes()
                +item.Counters.MemoryBytes()+(item.Readback ? 96 : 0);
        state.Reason.clear();
        state.Available = true;
        if (!VirtualGeometryTraversal::TwoPassActive()) state.Stats.Phases[1] = {};
        RenderProfiler::GeometryBinningStatistics sample{};
        for (uint32_t phase = 0; phase < 3; ++phase)
        {
            const auto& latest = state.Stats.Phases[phase];
            sample.ActiveBins[phase] = latest.ActiveBins;
            sample.Clusters[phase] = latest.Clusters;
            sample.OverflowClusters[phase] = latest.OverflowClusters;
            sample.InvalidMaterials[phase] = latest.InvalidMaterials;
            sample.HasSample[phase] = latest.HasSample;
        }
        sample.BufferBytes = state.Stats.BufferBytes;
        RenderProfiler::RecordGeometryBinning(sample);
    }
    catch (const std::exception& error)
    {
        state.Failed = true;
        state.Reason = error.what();
        WriteGraphicsLog("Native material binning unavailable; using original dispatch: %s\n",error.what());
    }
}

void Prepare(RenderGraph& graph, VirtualGeometryTraversal::RasterPhase phase)
{
    if (!Active()) return;
    auto& frame = Current();
    auto input = Import(graph,"Native bin input",VirtualGeometryTraversal::VisibleBuffer(phase),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto count = Import(graph,"Native bin input count",VirtualGeometryTraversal::CounterBuffer(phase),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto bins = Import(graph,"Native material ranges",frame.Bins,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto sorted = Import(graph,"Native material sorted clusters",frame.Visible,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto args = Import(graph,"Native material commands",frame.Arguments,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    auto stats = Import(graph,"Native material bin counters",frame.Counters,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    graph.AddPass("Native material bin reset",
        [=](RenderGraph::PassBuilder& b) { b.Write(bins,D3D12_RESOURCE_STATE_UNORDERED_ACCESS).Write(stats,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); },
        [=](ID3D12GraphicsCommandList* list)
        {
            Bind(list,phase);
            list->SetPipelineState(Data().Reset.Get());
            list->Dispatch((std::max(8u,Data().ExactBins+1)+63)/64,1,1);
        });
    graph.AddPass("Native material bin count",
        [=](RenderGraph::PassBuilder& b)
        {
            b.Read(input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE).Read(count,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .ReadWrite(bins,D3D12_RESOURCE_STATE_UNORDERED_ACCESS).ReadWrite(stats,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        [=](ID3D12GraphicsCommandList* list)
        {
            RenderProfiler::ScopedEvent profile("Native material bin count",list);
            Bind(list,phase);
            list->SetPipelineState(Data().Count.Get());
            list->Dispatch((Data().Capacity+63)/64,1,1);
        });
    graph.AddPass("Native material bin prefix and commands",
        [=](RenderGraph::PassBuilder& b)
        {
            b.ReadWrite(bins,D3D12_RESOURCE_STATE_UNORDERED_ACCESS).ReadWrite(stats,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .Write(args,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        [=](ID3D12GraphicsCommandList* list)
        {
            RenderProfiler::ScopedEvent profile("Native material bin prefix",list);
            Bind(list,phase);
            list->SetPipelineState(Data().Prefix.Get());
            list->Dispatch(1,1,1);
        });
    graph.AddPass("Native material bin scatter",
        [=](RenderGraph::PassBuilder& b)
        {
            b.Read(input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE).Read(count,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .ReadWrite(bins,D3D12_RESOURCE_STATE_UNORDERED_ACCESS).Write(sorted,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        [=](ID3D12GraphicsCommandList* list)
        {
            RenderProfiler::ScopedEvent profile("Native material bin scatter",list);
            Bind(list,phase);
            list->SetPipelineState(Data().Scatter.Get());
            list->Dispatch((Data().Capacity+63)/64,1,1);
        });
    auto* target = &frame;
    uint32_t phaseIndex = uint32_t(phase);
    uint64_t serial = Data().Serial;
    graph.AddPass("Native material bin feedback",
        [=](RenderGraph::PassBuilder& b) { b.Read(stats,D3D12_RESOURCE_STATE_COPY_SOURCE); },
        [=](ID3D12GraphicsCommandList* list)
        {
            list->CopyBufferRegion(target->Readback.Get(),uint64_t(phaseIndex)*32,target->Counters.Resource(),0,32);
            target->Fence = GraphicsDevice::GetPendingSubmissionFence();
            target->Serial = serial;
            target->SampleMask |= 1u<<phaseIndex;
        });
}
void Shutdown() { Data() = {}; }
}
