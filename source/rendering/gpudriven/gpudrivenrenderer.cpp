#include "pch.h"
#include "gpudrivenrenderer.h"
#include "indirectcommand.h"
#include "gpuscene/gpuscene.h"
#include "gpuscene/gpuscenebuffer.h"
#include "staticmodel.h"
#include "graphicsdevice.h"
#include "rendergraph.h"
#include "renderprofiler.h"
#include "renderconfiguration.h"
#include "renderersettings.h"
#include "frameconstants.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "componentmanager.h"
#include "vertexdeformation.h"
#include "texturemanager.h"
#include "camera.h"
#include "rendercapture.h"
#include "graphicslog.h"
#include <unordered_set>
#include <unordered_map>

namespace
{
constexpr uint32_t OpaqueInstanceFlag = 4;
struct FrameBuffers
{
    GpuSceneBuffer Visible, Count, Commands;
};
struct State
{
    bool Enabled = true;
    bool Active = false;
    bool Failed = false;
    std::array<FrameBuffers, GpuSceneBuffer::FrameCount> Frames;
    GpuSceneBuffer Vertices, Indices, Geometry;
    std::vector<StaticModelVertex> CpuVertices;
    std::vector<uint32_t> CpuIndices;
    std::vector<GPUDrivenGeometryData> CpuGeometry;
    std::vector<uint32_t> NewVertices, NewIndices, NewGeometry;
    std::unordered_map<uint64_t, std::vector<uint32_t>> GeometryMeshIDs;
    std::unordered_map<uint32_t, std::vector<uint32_t>> EntityTextures;
    std::unordered_map<uint32_t, uint32_t> TextureUsers;
    ComPtr<ID3D12RootSignature> ComputeRoot, OpaqueRoot, ResetRoot;
    ComPtr<ID3D12PipelineState> ResetPSO, CompactPSO, GeneratePSO, OpaquePSO;
    ComPtr<ID3D12CommandSignature> Signature;
    uint32_t Slots = 0, Meshes = 0, Calls = 0, FrameIndex = 0;
    double SubmissionMs = 0;
    double CpuOpaqueMs = 0, GpuOpaqueMs = 0;
    bool DebugReadback = false;
    uint32_t DebugCount = 0;
    std::vector<GPUIndirectDrawCommand> DebugCommands;
};
State& Data() { static State state; return state; }
FrameBuffers& Frame() { return Data().Frames.at(Data().FrameIndex); }
void Check(HRESULT result)
{
    if (FAILED(result)) throw std::runtime_error("GPU driven pipeline creation failed");
}
void Root(ID3D12Device* device, CD3DX12_ROOT_SIGNATURE_DESC& description, ComPtr<ID3D12RootSignature>& output)
{
    ComPtr<ID3DBlob> blob, error;
    HRESULT result = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(result) && error) WriteGraphicsLog("GPU driven root: %s\n", static_cast<const char*>(error->GetBufferPointer()));
    Check(result);
    Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&output)));
}
ComPtr<ID3DBlob> Shader(const char* path)
{
    ComPtr<ID3DBlob> blob;
    ShaderDescription description{};
    description.csoPath = path;
    description.ppBlob = blob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(description)) throw std::runtime_error("GPU driven shader missing");
    return blob;
}
void Compute(ID3D12Device* device, ID3D12RootSignature* root, const char* path, ComPtr<ID3D12PipelineState>& output)
{
    auto shader = Shader(path);
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = root;
    description.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    Check(device->CreateComputePipelineState(&description, IID_PPV_ARGS(&output)));
}
void Initialize()
{
    auto& state = Data();
    if (state.Signature) return;
    auto* device = GraphicsDevice::GetDevice();
    CD3DX12_ROOT_PARAMETER reset;
    reset.InitAsUnorderedAccessView(0);
    CD3DX12_ROOT_SIGNATURE_DESC resetDescription(1, &reset);
    Root(device, resetDescription, state.ResetRoot);
    Compute(device, state.ResetRoot.Get(), "shader/hlsl/build/GPUDrivenResetCS.cso", state.ResetPSO);

    CD3DX12_ROOT_PARAMETER compute[7];
    for (uint32_t i = 0; i < 4; ++i) compute[i].InitAsShaderResourceView(i);
    compute[4].InitAsUnorderedAccessView(0);
    compute[5].InitAsUnorderedAccessView(1);
    compute[6].InitAsConstants(3, 0);
    CD3DX12_ROOT_SIGNATURE_DESC computeDescription(_countof(compute), compute);
    Root(device, computeDescription, state.ComputeRoot);
    Compute(device, state.ComputeRoot.Get(), "shader/hlsl/build/GPUDrivenCompactCS.cso", state.CompactPSO);
    Compute(device, state.ComputeRoot.Get(), "shader/hlsl/build/GenerateIndirectCommandsCS.cso", state.GeneratePSO);

    CD3DX12_DESCRIPTOR_RANGE constants, textures;
    constants.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    textures.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, FrameConstants::g_kMAX_SRVS, 0, 1);
    CD3DX12_ROOT_PARAMETER opaque[6];
    opaque[0].InitAsConstants(1, 4); // This index is part of the command signature contract.
    opaque[1].InitAsDescriptorTable(1, &constants);
    opaque[2].InitAsShaderResourceView(30);
    opaque[3].InitAsShaderResourceView(31);
    opaque[4].InitAsDescriptorTable(1, &textures, D3D12_SHADER_VISIBILITY_PIXEL);
    opaque[5].InitAsConstants(2, 5);
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    CD3DX12_ROOT_SIGNATURE_DESC opaqueDescription(_countof(opaque), opaque, 1, &sampler,
                                                  D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    Root(device, opaqueDescription, state.OpaqueRoot);
    auto vs = Shader("shader/hlsl/build/GPUDrivenOpaqueVS.cso");
    auto ps = Shader("shader/hlsl/build/GPUDrivenOpaquePS.cso");
    static const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    static_assert(sizeof(StaticModelVertex) == 48);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = state.OpaqueRoot.Get();
    pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pipeline.InputLayout = {layout, _countof(layout)};
    pipeline.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pipeline.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pipeline.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pipeline.SampleMask = UINT_MAX;
    pipeline.SampleDesc.Count = 1;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 5;
    for (uint32_t i = 0; i < 5; ++i) pipeline.RTVFormats[i] = RendererState::GetGBufferFormat(static_cast<GBufferType>(i));
    pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    Check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&state.OpaquePSO)));

    D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant.RootParameterIndex = 0;
    arguments[0].Constant.Num32BitValuesToSet = 1;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = sizeof(GPUIndirectDrawCommand);
    signature.NumArgumentDescs = _countof(arguments);
    signature.pArgumentDescs = arguments;
    Check(device->CreateCommandSignature(&signature, state.OpaqueRoot.Get(), IID_PPV_ARGS(&state.Signature)));
}
RenderGraph::ResourceHandle Import(RenderGraph& graph, const char* name, GpuSceneBuffer& buffer,
                                   D3D12_RESOURCE_STATES finalState)
{
    RenderGraph::ImportedResource description{};
    description.Name = name;
    description.Resource = buffer.Resource();
    description.InitialState = buffer.State;
    description.HasFinalState = true;
    description.FinalState = finalState;
    buffer.State = finalState;
    return graph.ImportResource(description);
}
bool CompatibleMode()
{
    return RenderConfiguration::GetRenderMode() == RenderMode::DEFERRED && !RendererSettings::GetComputeGBufferEnabled();
}
} // namespace

void GPUDrivenRenderer::RegisterGeometry(uint32_t meshID, const StaticMeshData& mesh)
{
    auto& state = Data();
    if (meshID < state.CpuGeometry.size() && state.CpuGeometry[meshID].Ready) return;
    if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.CpuVertices.size() != mesh.VertexCount || mesh.CpuIndices.size() != mesh.IndexCount ||
        !mesh.IndexCount || !mesh.VertexCount || mesh.IndexBufferView.Format != DXGI_FORMAT_R32_UINT) return;
    for (uint32_t index : mesh.CpuIndices) if (index >= mesh.VertexCount) return;
    const auto geometry = ReserveGpuDrivenGeometry(state.CpuVertices.size(), state.CpuIndices.size(), mesh.VertexCount, mesh.IndexCount);
    if (state.CpuGeometry.size() <= meshID) state.CpuGeometry.resize(size_t(meshID) + 1);
    for (uint32_t i = 0; i < mesh.VertexCount; ++i) state.NewVertices.push_back(static_cast<uint32_t>(state.CpuVertices.size()) + i);
    for (uint32_t i = 0; i < mesh.IndexCount; ++i) state.NewIndices.push_back(static_cast<uint32_t>(state.CpuIndices.size()) + i);
    state.CpuVertices.insert(state.CpuVertices.end(), mesh.CpuVertices.begin(), mesh.CpuVertices.end());
    state.CpuIndices.insert(state.CpuIndices.end(), mesh.CpuIndices.begin(), mesh.CpuIndices.end());
    state.CpuGeometry[meshID] = geometry;
    state.NewGeometry.push_back(meshID);
    state.GeometryMeshIDs[mesh.VertexBuffer->GetGPUVirtualAddress()].push_back(meshID);
}

void GPUDrivenRenderer::UpdateGeometry(const StaticMeshData& mesh)
{
    if (!mesh.VertexBuffer) return;
    auto& state = Data();
    const auto found = state.GeometryMeshIDs.find(mesh.VertexBuffer->GetGPUVirtualAddress());
    if (found == state.GeometryMeshIDs.end()) return;
    for (uint32_t meshID : found->second)
    {
        const auto& geometry = state.CpuGeometry.at(meshID);
        const size_t first = static_cast<size_t>(geometry.BaseVertex);
        if (first + mesh.CpuVertices.size() > state.CpuVertices.size())
            throw std::runtime_error("GPU driven geometry update exceeds registered range");
        std::copy(mesh.CpuVertices.begin(), mesh.CpuVertices.end(), state.CpuVertices.begin() + first);
        for (uint32_t i = 0; i < mesh.CpuVertices.size(); ++i) state.NewVertices.push_back(static_cast<uint32_t>(first) + i);
    }
}

bool GPUDrivenRenderer::SupportsEntity(uint32_t entity)
{
    if (!ComponentManager::HasComponent<StaticModelComponent>(entity) ||
        !ComponentManager::HasComponent<MaterialComponent>(entity) || VertexDeformation::IsRequested(entity)) return false;
    if (ComponentManager::HasComponent<LODComponent>(entity) && ComponentManager::GetComponentUnchecked<LODComponent>(entity).UseLOD) return false;
    if (ComponentManager::HasComponent<MeshComponent>(entity) && ComponentManager::GetComponentUnchecked<MeshComponent>(entity).IsEnvironment) return false;
    if (ComponentManager::HasComponent<NameComponent>(entity) && ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky") return false;
    const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    if (material.ShaderClassMode != MaterialMode::Manual || material.ShaderClass != ShaderClass::PBR ||
        material.IsTransparent || material.Alpha < 1.0f) return false;
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if ((!shader.VsPath.empty() && shader.VsPath != "shader/hlsl/build/modelshaderVS.cso") ||
            (!shader.PsPath.empty() && shader.PsPath != "shader/hlsl/build/modelshaderPS.cso")) return false;
    }
    return true;
}

bool GPUDrivenRenderer::GeometryReady(uint32_t meshID)
{
    return meshID < Data().CpuGeometry.size() && Data().CpuGeometry[meshID].Ready != 0;
}

bool GPUDrivenRenderer::HandlesEntity(uint32_t entity)
{
    if (!Data().Active) return false;
    const auto& ids = GpuScene::GetEntityInstances(entity);
    if (ids.empty()) return false;
    for (auto id : ids)
    {
        const auto* instance = GpuScene::GetInstance(id);
        if (!instance || !(instance->Flags & OpaqueInstanceFlag)) return false;
    }
    return true;
}

void GPUDrivenRenderer::Prepare(RenderGraph& graph, RenderGraph::ResourceHandle instances,
                               RenderGraph::ResourceHandle visibility, uint32_t slots, uint32_t capacity, uint32_t meshes)
{
    auto& state = Data();
    state.Active = false;
    state.Calls = 0;
    state.SubmissionMs = 0;
    if (!state.Enabled || state.Failed || !CompatibleMode()) return;
    try { Initialize(); }
    catch (const std::exception& error)
    {
        state.Failed = true;
        WriteGraphicsLog("GPU Driven CPU fallback: %s\n", error.what());
        return;
    }
    auto* device = GraphicsDevice::GetDevice();
    const uint32_t frameIndex = GraphicsDevice::GetFrameIndex();
    state.FrameIndex = frameIndex;
    auto& frame = Frame();
    state.Slots = slots;
    state.Meshes = meshes;
    // A sparse mesh table may include animated records without arena geometry.
    state.CpuGeometry.resize((std::max)(size_t(meshes), state.CpuGeometry.size()));
    auto prepare = [&](GpuSceneBuffer& buffer, uint32_t count, uint32_t stride, const wchar_t* name, bool uav = false) {
        buffer.BeginFrame(frameIndex);
        buffer.Ensure(device, count, stride, frameIndex, name, uav);
    };
    prepare(state.Vertices, static_cast<uint32_t>(state.CpuVertices.size()), sizeof(StaticModelVertex), L"Global Static Vertices");
    prepare(state.Indices, static_cast<uint32_t>(state.CpuIndices.size()), 4, L"Global Static Indices");
    prepare(state.Geometry, meshes, sizeof(GPUDrivenGeometryData), L"GPU Driven Mesh Geometry");
    prepare(frame.Visible, capacity, 4, L"GPU Driven Visible IDs", true);
    prepare(frame.Commands, capacity, sizeof(GPUIndirectDrawCommand), L"GPU Driven Commands", true);
    prepare(frame.Count, 1, 4, L"GPU Driven Count", true);
    const uint64_t vertexBytes = state.Vertices.Stage(device, frameIndex, state.CpuVertices.data(), static_cast<uint32_t>(state.CpuVertices.size()), state.NewVertices);
    const uint64_t indexBytes = state.Indices.Stage(device, frameIndex, state.CpuIndices.data(), static_cast<uint32_t>(state.CpuIndices.size()), state.NewIndices);
    const uint64_t meshBytes = state.Geometry.Stage(device, frameIndex, state.CpuGeometry.data(), meshes, state.NewGeometry);
    state.NewVertices.clear(); state.NewIndices.clear(); state.NewGeometry.clear();
    const auto vertices = Import(graph, "Global Static Vertices", state.Vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    const auto indices = Import(graph, "Global Static Indices", state.Indices, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    const auto geometry = Import(graph, "GPU Driven Geometry", state.Geometry, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto visible = Import(graph, "Visible Instance List", frame.Visible, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto count = Import(graph, "Indirect Command Count", frame.Count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    const auto arguments = Import(graph, "Indirect Commands", frame.Commands, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    if (vertexBytes || indexBytes || meshBytes)
        graph.AddPass("GPU Driven Geometry Update", [=](RenderGraph::PassBuilder& builder) {
            if (vertexBytes) builder.Write(vertices, D3D12_RESOURCE_STATE_COPY_DEST);
            if (indexBytes) builder.Write(indices, D3D12_RESOURCE_STATE_COPY_DEST);
            if (meshBytes) builder.Write(geometry, D3D12_RESOURCE_STATE_COPY_DEST);
        }, [frameIndex](ID3D12GraphicsCommandList* list) {
            auto& data = Data();
            data.Vertices.Copy(list, frameIndex); data.Indices.Copy(list, frameIndex); data.Geometry.Copy(list, frameIndex);
        });
    graph.AddPass("GPU Driven Counter Reset", [=](RenderGraph::PassBuilder& builder) {
        builder.Write(count, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [](ID3D12GraphicsCommandList* list) {
        auto& data = Data();
        list->SetComputeRootSignature(data.ResetRoot.Get());
        list->SetPipelineState(data.ResetPSO.Get());
        list->SetComputeRootUnorderedAccessView(0, Frame().Count.Resource()->GetGPUVirtualAddress());
        list->Dispatch(1, 1, 1);
    });
    auto bindCompute = [](ID3D12GraphicsCommandList* list, bool generate) {
        auto& data = Data(); auto& buffers = Frame();
        list->SetComputeRootSignature(data.ComputeRoot.Get());
        list->SetPipelineState(generate ? data.GeneratePSO.Get() : data.CompactPSO.Get());
        list->SetComputeRootShaderResourceView(0, GpuScene::InstanceBuffer()->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1, (generate ? buffers.Visible.Resource() : GpuScene::VisibilityBuffer())->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, data.Geometry.Resource()->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(3, buffers.Count.Resource()->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(4, (generate ? buffers.Commands.Resource() : buffers.Visible.Resource())->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(5, buffers.Count.Resource()->GetGPUVirtualAddress());
        const uint32_t limits[] = {data.Slots, buffers.Commands.Capacity(), data.Meshes};
        list->SetComputeRoot32BitConstants(6, 3, limits, 0);
    };
    graph.AddPass("Compact Visible Instances", [=](RenderGraph::PassBuilder& builder) {
        builder.Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            .Read(visibility, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE).Read(geometry, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            .Write(visible, D3D12_RESOURCE_STATE_UNORDERED_ACCESS).ReadWrite(count, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [bindCompute](ID3D12GraphicsCommandList* list) {
        RenderProfiler::ScopedEvent profile("Compact Visible Instances", list);
        bindCompute(list, false);
        if (Data().Slots) list->Dispatch((Data().Slots + 63) / 64, 1, 1);
    });
    graph.AddPass("Generate Indirect Commands", [=](RenderGraph::PassBuilder& builder) {
        builder.Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE).Read(geometry, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            .Read(visible, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE).Read(count, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            .Write(arguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [bindCompute](ID3D12GraphicsCommandList* list) {
        RenderProfiler::ScopedEvent profile("Generate Indirect Commands", list);
        bindCompute(list, true);
        list->Dispatch((Frame().Commands.Capacity() + 63) / 64, 1, 1);
    });
    state.Active = true;
    for (const auto& [texture, users] : state.TextureUsers)
        if (users) TextureManager::TouchTexture(static_cast<int>(texture));
}

void GPUDrivenRenderer::Draw(ID3D12GraphicsCommandList* commands)
{
    auto& state = Data();
    if (!state.Active) return;
    if (state.CpuVertices.empty() || state.CpuIndices.empty()) return;
    const auto start = std::chrono::steady_clock::now();
    RenderGraph graph;
    RenderGraph::ImportedResource input{};
    input.HasFinalState = true;
    input.InitialState = input.FinalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    input.Name = "Opaque Scene Instances"; input.Resource = GpuScene::InstanceBuffer();
    const auto instances = graph.ImportResource(input);
    input.Name = "Opaque Scene Materials"; input.Resource = GpuScene::MaterialBuffer();
    const auto materials = graph.ImportResource(input);
    const auto vertices = Import(graph, "Opaque Global Vertices", state.Vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    const auto indices = Import(graph, "Opaque Global Indices", state.Indices, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    const auto arguments = Import(graph, "Opaque Indirect Commands", Frame().Commands, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    const auto count = Import(graph, "Opaque Command Count", Frame().Count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    graph.AddPass("Opaque GPU Driven", [=](RenderGraph::PassBuilder& builder) {
        const auto sceneRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        builder.Read(instances, sceneRead).Read(materials, sceneRead)
            .Read(vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER).Read(indices, D3D12_RESOURCE_STATE_INDEX_BUFFER)
            .Read(arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT).Read(count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }, [](ID3D12GraphicsCommandList* list) {
        auto& data = Data();
        RenderProfiler::ScopedEvent profile("Opaque ExecuteIndirect", list);
        ID3D12DescriptorHeap* heap = FrameConstants::GetCbvHeap();
        list->SetDescriptorHeaps(1, &heap);
        list->SetGraphicsRootSignature(data.OpaqueRoot.Get());
        list->SetPipelineState(data.OpaquePSO.Get());
        ConstantBuffer3D constants{};
        XMMATRIX view, projection;
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        constants.View = XMMatrixTranspose(view); constants.Projection = XMMatrixTranspose(projection);
        list->SetGraphicsRootDescriptorTable(1, FrameConstants::AllocateTransientConstantBuffer(constants));
        list->SetGraphicsRootShaderResourceView(2, GpuScene::InstanceBuffer()->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(3, GpuScene::MaterialBuffer()->GetGPUVirtualAddress());
        const CD3DX12_GPU_DESCRIPTOR_HANDLE textures(heap->GetGPUDescriptorHandleForHeapStart(), FrameConstants::g_kTEXTURE_SRV_START_INDEX, FrameConstants::GetCbvIncrementSize());
        list->SetGraphicsRootDescriptorTable(4, textures);
        const uint32_t textureLimits[] = {FrameConstants::g_kTEXTURE_SRV_START_INDEX,
            static_cast<uint32_t>(TextureManager::GetDefaultTextureIndex()) - FrameConstants::g_kTEXTURE_SRV_START_INDEX};
        list->SetGraphicsRoot32BitConstants(5, 2, textureLimits, 0);
        const D3D12_VERTEX_BUFFER_VIEW vb{data.Vertices.Resource()->GetGPUVirtualAddress(),
            static_cast<UINT>(data.CpuVertices.size() * sizeof(StaticModelVertex)), sizeof(StaticModelVertex)};
        const D3D12_INDEX_BUFFER_VIEW ib{data.Indices.Resource()->GetGPUVirtualAddress(),
            static_cast<UINT>(data.CpuIndices.size() * 4), DXGI_FORMAT_R32_UINT};
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vb); list->IASetIndexBuffer(&ib);
        list->ExecuteIndirect(data.Signature.Get(), Frame().Commands.Capacity(), Frame().Commands.Resource(), 0, Frame().Count.Resource(), 0);
        ++data.Calls;
    });
    if (!graph.Compile() || !graph.Execute(commands)) throw std::runtime_error(graph.GetLastError());
    state.SubmissionMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool GPUDrivenRenderer::IsEnabled() { return Data().Enabled; }
bool GPUDrivenRenderer::IsActive() { return Data().Active; }
void GPUDrivenRenderer::SetEnabled(bool enabled) { Data().Enabled = enabled; }
void GPUDrivenRenderer::Shutdown() { Data() = {}; }
ID3D12Resource* GPUDrivenRenderer::CommandBuffer() { return Frame().Commands.Resource(); }
ID3D12Resource* GPUDrivenRenderer::CountBuffer() { return Frame().Count.Resource(); }
ID3D12Resource* GPUDrivenRenderer::VisibleBuffer() { return Frame().Visible.Resource(); }
ID3D12Resource* GPUDrivenRenderer::GeometryBuffer() { return Data().Geometry.Resource(); }
uint32_t GPUDrivenRenderer::CommandCapacity() { return Frame().Commands.Capacity(); }
double GPUDrivenRenderer::SubmissionMilliseconds() { return Data().SubmissionMs; }
uint32_t GPUDrivenRenderer::ExecuteCalls() { return Data().Calls; }
void GPUDrivenRenderer::RecordOpaqueSubmission(double milliseconds)
{
    (Data().Active ? Data().GpuOpaqueMs : Data().CpuOpaqueMs) = milliseconds;
}
void GPUDrivenRenderer::TrackEntityTextures(uint32_t entity, const std::vector<uint32_t>& descriptors)
{
    auto& state = Data();
    if (auto found = state.EntityTextures.find(entity); found != state.EntityTextures.end())
    {
        if (found->second == descriptors) return;
        for (uint32_t descriptor : found->second)
        {
            auto user = state.TextureUsers.find(descriptor);
            if (user != state.TextureUsers.end() && --user->second == 0) state.TextureUsers.erase(user);
        }
        state.EntityTextures.erase(found);
    }
    if (descriptors.empty()) return;
    state.EntityTextures[entity] = descriptors;
    for (uint32_t descriptor : descriptors) ++state.TextureUsers[descriptor];
}

void GPUDrivenRenderer::DrawDebug()
{
    auto& state = Data();
    if (!ImGui::TreeNode("GPU Driven Rendering")) return;
    ImGui::Checkbox("Enabled", &state.Enabled);
    ImGui::Text("Opaque path: %s", state.Active ? "GPU Driven" : "CPU fallback");
    if (state.Enabled && !CompatibleMode()) ImGui::TextUnformatted("Requires Deferred mode with Compute GBuffer disabled.");
    if (state.Failed) ImGui::TextUnformatted("Pipeline initialization failed; see init_log.txt");
    ImGui::Text("ExecuteIndirect calls: %u", state.Calls);
    ImGui::Text("Max command capacity: %u", Frame().Commands.Capacity());
    ImGui::Text("Indirect buffer memory: %.2f KiB", double(Frame().Commands.MemoryBytes() + Frame().Count.MemoryBytes()) / 1024);
    ImGui::Text("GPU submission CPU: %.4f ms", state.SubmissionMs);
    ImGui::Text("Opaque CPU submission comparison: CPU %.4f / GPU %.4f ms", state.CpuOpaqueMs, state.GpuOpaqueMs);
    uint64_t memory = 0;
    for (const auto& buffers : state.Frames) memory += buffers.Commands.MemoryBytes() + buffers.Count.MemoryBytes() + buffers.Visible.MemoryBytes();
    ImGui::Text("All frame command/count/visible memory: %.2f KiB", double(memory) / 1024);
    ImGui::Text("Global geometry memory: %.2f MiB", double(state.Vertices.MemoryBytes() + state.Indices.MemoryBytes() + state.Geometry.MemoryBytes()) / (1024 * 1024));
    for (const auto& pass : RenderProfiler::GetLatest().Passes)
        if (pass.Name == "GPU Scene Culling" || pass.Name == "Generate Indirect Commands" ||
            pass.Name == "Opaque ExecuteIndirect" || pass.Name == "GBuffer / Opaque")
            ImGui::Text("%s: GPU %.4f ms / CPU %.4f ms", pass.Name.c_str(), pass.GpuMs, pass.CpuMs);
    ImGui::Checkbox("Debug command readback (stalls GPU)", &state.DebugReadback);
    if (state.DebugReadback)
    {
        ImGui::Text("Debug sample visible/generated/executed commands: %u", state.DebugCount);
        for (const auto& command : state.DebugCommands)
        {
            const auto* instance = GpuScene::GetInstance(command.InstanceID);
            ImGui::Text("Instance %u, mesh %u: indices %u, start %u, base %d", command.InstanceID,
                instance ? instance->MeshIndex : UINT32_MAX, command.Draw.IndexCountPerInstance,
                command.Draw.StartIndexLocation, command.Draw.BaseVertexLocation);
        }
    }
    else ImGui::TextUnformatted("Counts remain on GPU; enable debug readback to inspect.");
    ImGui::TreePop();
}

void GPUDrivenRenderer::EndFrame()
{
    auto& state = Data();
    if (state.DebugReadback && state.Active)
    {
        std::vector<uint8_t> bytes;
        if (RenderCapture::ReadBuffer(CountBuffer(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, 4, bytes))
        {
            std::memcpy(&state.DebugCount, bytes.data(), 4);
            const uint32_t samples = (std::min)(state.DebugCount, 16u);
            if (samples && RenderCapture::ReadBuffer(CommandBuffer(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    uint64_t(samples) * sizeof(GPUIndirectDrawCommand), bytes))
            {
                state.DebugCommands.resize(samples);
                std::memcpy(state.DebugCommands.data(), bytes.data(), bytes.size());
            }
            else state.DebugCommands.clear();
        }
    }
}
