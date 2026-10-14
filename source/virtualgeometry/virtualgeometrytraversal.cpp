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
};
static_assert(sizeof(Constants) == 208);
struct Frame
{
    GpuSceneBuffer States, Visible, Counters, Arguments, Success, Queue;
};
struct State
{
    Settings Options;
    Constants Parameters;
    FrameInputs Inputs;
    std::array<Frame, GpuSceneBuffer::FrameCount> Frames;
    uint32_t FrameIndex = 0;
    bool Active = false;
    ComPtr<ID3D12RootSignature> Root;
    ComPtr<ID3D12PipelineState> ResetPSO, TraversePSO, ArgumentsPSO;
    std::array<uint32_t,64> Debug{};
};
State& Data() { static State state; return state; }
Frame& Current() { return Data().Frames.at(Data().FrameIndex); }
void Check(HRESULT result)
{
    if (FAILED(result)) throw std::runtime_error("Native VirtualGeometry compute initialization failed");
}
void Initialize()
{
    auto& state = Data();
    if (state.ArgumentsPSO) return;
    CD3DX12_ROOT_PARAMETER parameters[17];
    for (uint32_t index = 0; index < 9; ++index) parameters[index].InitAsShaderResourceView(index);
    for (uint32_t index = 0; index < 6; ++index) parameters[9+index].InitAsUnorderedAccessView(index);
    CD3DX12_DESCRIPTOR_RANGE hiz, cbv;
    hiz.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,9);
    cbv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0);
    parameters[15].InitAsDescriptorTable(1,&hiz);
    parameters[16].InitAsDescriptorTable(1,&cbv);
    CD3DX12_ROOT_SIGNATURE_DESC description(17,parameters);
    ComPtr<ID3DBlob> blob, error;
    Check(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    Check(GraphicsDevice::GetDevice()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&state.Root)));
    auto pipeline = [&](const char* path, ComPtr<ID3D12PipelineState>& target)
    {
        ComPtr<ID3DBlob> shader;
        ShaderDescription input{};
        input.csoPath = path;
        input.ppBlob = shader.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(input)) throw std::runtime_error(path);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = state.Root.Get();
        desc.CS = {shader->GetBufferPointer(),shader->GetBufferSize()};
        Check(GraphicsDevice::GetDevice()->CreateComputePipelineState(&desc,IID_PPV_ARGS(&target)));
    };
    pipeline("shader/hlsl/build/VirtualGeometryResetCS.cso",state.ResetPSO);
    pipeline("shader/hlsl/build/VirtualGeometryTraversalCS.cso",state.TraversePSO);
    pipeline("shader/hlsl/build/VirtualGeometryArgumentsCS.cso",state.ArgumentsPSO);
}
RenderGraph::ResourceHandle Import(RenderGraph& graph, const char* name, GpuSceneBuffer& buffer, D3D12_RESOURCE_STATES final)
{
    auto existing = graph.FindImportedResource(buffer.Resource());
    if (existing.IsValid()) return existing;
    RenderGraph::ImportedResource resource{};
    resource.Name = name;
    resource.Resource = buffer.Resource();
    resource.InitialState = buffer.State;
    resource.HasFinalState = true;
    resource.FinalState = final;
    buffer.State = final;
    return graph.ImportResource(resource);
}
void Bind(ID3D12GraphicsCommandList* list)
{
    auto& state = Data();
    auto& frame = Current();
    ID3D12DescriptorHeap* heap = FrameConstants::GetCbvHeap();
    list->SetDescriptorHeaps(1,&heap);
    list->SetComputeRootSignature(state.Root.Get());
    ID3D12Resource* sources[] = {GpuScene::InstanceBuffer(),state.Inputs.ObjectResource,
        VirtualGeometryResources::AssetBuffer().Resource(),VirtualGeometryResources::ClusterBuffer().Resource(),
        VirtualGeometryResources::GroupBuffer().Resource(),VirtualGeometryResources::HierarchyIndexBuffer().Resource(),
        VirtualGeometryResources::RootBuffer().Resource(),state.Inputs.CountResource,GpuScene::MaterialBuffer()};
    for (uint32_t index = 0; index < 9; ++index) list->SetComputeRootShaderResourceView(index,sources[index]->GetGPUVirtualAddress());
    GpuSceneBuffer* outputs[] = {&frame.States,&frame.Visible,&frame.Counters,&frame.Arguments,&frame.Success,&frame.Queue};
    for (uint32_t index = 0; index < 6; ++index) list->SetComputeRootUnorderedAccessView(9+index,outputs[index]->Resource()->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(15,OcclusionCulling::GetPreviousSrv());
    list->SetComputeRootDescriptorTable(16,FrameConstants::AllocateTransientConstantBuffer(&state.Parameters,sizeof(state.Parameters)));
}
}
Settings& GetSettings() { return Data().Options; }
bool Active() { return Data().Active; }
GpuSceneBuffer& VisibleBuffer() { return Current().Visible; }
GpuSceneBuffer& CounterBuffer() { return Current().Counters; }
GpuSceneBuffer& ArgumentBuffer() { return Current().Arguments; }
GpuSceneBuffer& SuccessBuffer() { return Current().Success; }
const std::array<uint32_t,64>& DebugCounters() { return Data().Debug; }
void Prepare(RenderGraph& graph, const FrameInputs& inputs)
{
    auto& state = Data();
    state.Active = false;
    state.FrameIndex = GraphicsDevice::GetFrameIndex();
    state.Inputs = inputs;
    auto& frame = Current();
    uint64_t maximumClusters = 1;
    for (const auto& asset : VirtualGeometryResources::AssetTable())
    {
        maximumClusters = std::max(maximumClusters,uint64_t(std::max(asset.ClusterCount,asset.GroupCount)));
    }
    uint64_t maximumWork = maximumClusters*inputs.ObjectCapacity;
    auto ensure = [&](GpuSceneBuffer& buffer,uint32_t count,uint32_t stride)
    {
        buffer.BeginFrame(state.FrameIndex);
        buffer.Ensure(GraphicsDevice::GetDevice(),count,stride,state.FrameIndex,L"Native VirtualGeometry frame",true);
    };
    // Always reset routing, including disabled/failed native frames.
    ensure(frame.Success,inputs.ObjectCapacity,4);
    ensure(frame.Counters,64,4);
    ensure(frame.Arguments,1,12);
    state.Options.MaxSelectedClusters = std::clamp(state.Options.MaxSelectedClusters,1u,4000000u);
    state.Options.MaxGroupStates = std::clamp(state.Options.MaxGroupStates,1u,4000000u);
    uint32_t scratch = state.Options.Enabled ? uint32_t(std::min(uint64_t(state.Options.MaxGroupStates),maximumWork)) : 1;
    uint32_t selectedCapacity = state.Options.Enabled ? uint32_t(std::min(uint64_t(state.Options.MaxSelectedClusters),maximumWork)) : 1;
    ensure(frame.States,scratch,4);
    ensure(frame.Queue,scratch,4);
    ensure(frame.Visible,selectedCapacity,16);
    VirtualGeometryResources::Prepare(graph);
    Initialize();
    auto& p = state.Parameters;
    XMMATRIX view, projection;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(),view,projection);
    XMStoreFloat4x4(&p.VP,view*projection);
    XMStoreFloat4x4(&p.PreviousVP,XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix())*XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix()));
    XMStoreFloat4(&p.Camera,XMMatrixInverse(nullptr,view).r[3]);
    XMFLOAT4X4 projectionValues;
    XMStoreFloat4x4(&projectionValues,projection);
    p.ProjectionPixels = 0.5f*float(GraphicsDevice::GetSceneHeight())*std::abs(projectionValues._22);
    p.TargetPixels = std::max(0.01f,state.Options.TargetErrorPixels);
    p.ForceLevel = std::clamp(state.Options.ForceHierarchyLevel,-1,31);
    p.Capacity = selectedCapacity;
    p.StateCapacity = scratch;
    p.ObjectCapacity = inputs.ObjectCapacity;
    p.Assets = VirtualGeometryResources::AssetCount();
    p.Materials = GpuScene::Statistics().Materials;
    p.Enabled = state.Options.Enabled;
    p.Frustum = state.Options.FrustumCulling;
    p.Cone = state.Options.ConeCulling;
    p.Occlusion = state.Options.OcclusionCulling && GPUVisibility::HistoryValid() && !GPUVisibility::GetSettings().FreezeCamera;
    p.Width = float(std::max(1u,OcclusionCulling::GetWidth()));
    p.Height = float(std::max(1u,OcclusionCulling::GetHeight()));
    p.Mips = std::max(1u,OcclusionCulling::GetMipCount());
    auto stats = Import(graph,"Nanite Counters",frame.Counters,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto success = Import(graph,"Nanite Instance Routing",frame.Success,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto visible = Import(graph,"Visible Virtual Clusters",frame.Visible,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto arguments = Import(graph,"Nanite Dispatch Arguments",frame.Arguments,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    auto states = Import(graph,"Nanite Group States",frame.States,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto queue = Import(graph,"Nanite Traversal Queue",frame.Queue,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    std::vector<RenderGraph::ResourceHandle> assetHandles;
    GpuSceneBuffer* assets[] = {&VirtualGeometryResources::AssetBuffer(),&VirtualGeometryResources::ClusterBuffer(),
        &VirtualGeometryResources::GroupBuffer(),&VirtualGeometryResources::HierarchyIndexBuffer(),&VirtualGeometryResources::RootBuffer()};
    for (auto* buffer : assets) assetHandles.push_back(Import(graph,"Nanite Asset SRV",*buffer,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    auto external = [&](ID3D12Resource* resource,const char* name,D3D12_RESOURCE_STATES read)
    {
        auto existing = graph.FindImportedResource(resource);
        if (existing.IsValid()) return existing;
        RenderGraph::ImportedResource input{};
        input.Name = name; input.Resource = resource;
        input.InitialState = input.FinalState = read; input.HasFinalState = true;
        return graph.ImportResource(input);
    };
    auto material = external(GpuScene::MaterialBuffer(),"Nanite Materials",D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RenderGraph::ResourceHandle hiz;
    if (OcclusionCulling::GetPreviousResource()) hiz = external(OcclusionCulling::GetPreviousResource(),"Nanite Hi-Z",OcclusionCulling::ReadState());
    graph.AddPass("Nanite Reset",
        [=](RenderGraph::PassBuilder& b) { b.Write(stats,D3D12_RESOURCE_STATE_UNORDERED_ACCESS).Write(success,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); },
        [](ID3D12GraphicsCommandList* list)
        {
            Bind(list); list->SetPipelineState(Data().ResetPSO.Get());
            list->Dispatch(std::max(1u,(Data().Inputs.ObjectCapacity+63)/64),1,1);
        });
    graph.AddPass("Nanite Hierarchy Traversal",
        [=](RenderGraph::PassBuilder& b)
        {
            b.Read(inputs.Instances,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(inputs.Objects,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(inputs.Count,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                .Read(material,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            for (auto handle : assetHandles) b.Read(handle,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (hiz.IsValid()) b.Read(hiz,OcclusionCulling::ReadState());
            b.ReadWrite(stats,D3D12_RESOURCE_STATE_UNORDERED_ACCESS).Write(success,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .Write(visible,D3D12_RESOURCE_STATE_UNORDERED_ACCESS).Write(states,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                .Write(queue,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        },
        [](ID3D12GraphicsCommandList* list)
        {
            RenderProfiler::ScopedEvent profile("Nanite Hierarchy Traversal",list);
            Bind(list); list->SetPipelineState(Data().TraversePSO.Get());
            uint32_t count = Data().Inputs.ObjectCapacity;
            uint32_t x = std::min(std::max(1u,count),65535u);
            list->Dispatch(x,std::max(1u,(count+x-1)/x),1);
        });
    graph.AddPass("Nanite Indirect Dispatch",
        [=](RenderGraph::PassBuilder& b) { b.ReadWrite(stats,D3D12_RESOURCE_STATE_UNORDERED_ACCESS).Write(arguments,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); },
        [](ID3D12GraphicsCommandList* list) { Bind(list); list->SetPipelineState(Data().ArgumentsPSO.Get()); list->Dispatch(1,1,1); });
    state.Active = true;
}
void EndFrame()
{
    if (Data().Active && Data().Options.DebugReadback)
    {
        std::vector<uint8_t> bytes;
        if (RenderCapture::ReadBuffer(Current().Counters.Resource(),Current().Counters.State,256,bytes))
        {
            memcpy(Data().Debug.data(),bytes.data(),256);
        }
    }
}
void Shutdown() { VirtualGeometryResources::Shutdown(); Data() = {}; }
}
