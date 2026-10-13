#include "pch.h"
#include "gpudrivenrenderer.h"
#include "meshlets/meshletmanager.h"
#include "meshshader/meshshaderrenderer.h"
#include "indirectcommand.h"
#include "gpudrivenvalidation.h"
#include "gpuscene/gpuscene.h"
#include "gpuscene/gpuscenebuffer.h"
#include "staticmodel.h"
#include "modelmanager.h"
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
#include "visibility/gpuvisibility.h"
#include "virtualgeometry/virtualgeometrydebug.h"
#include "occlusionculling.h"
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
    struct PrimitiveRange
    {
        uint32_t FirstVertex=0,VertexCapacity=0,FirstIndex=0,IndexCapacity=0;
        uint64_t SourceKey=0;
    };
    std::unordered_map<uint32_t,PrimitiveRange> PrimitiveRanges;
    std::vector<PrimitiveRange> FreePrimitiveRanges;
    uint64_t LastGeometryUploadBytes=0;
    bool Enabled = true;
    bool Active = false;
    bool Failed = false;
    bool PreviousCompatible = false;
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
    ComPtr<ID3D12PipelineState> ResetPSO, CompactPSO, GeneratePSO, OpaquePSO, OccluderPSO;
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
    if (FAILED(result))
    {
        WriteGraphicsLog("GPU driven pipeline HRESULT: 0x%08X\n", result);
        RenderCapture::ReportGraphicsErrors();
        throw std::runtime_error("GPU driven pipeline creation failed");
    }
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
    GPUDrivenValidation::Trace("indirect pipeline initialization begin");
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
    GPUDrivenValidation::Trace("indirect compute pipelines ready");

    CD3DX12_DESCRIPTOR_RANGE constants, textures;
    constants.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    textures.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, FrameConstants::g_kMAX_SRVS, 0, 1);
    CD3DX12_ROOT_PARAMETER opaque[7];
    opaque[0].InitAsConstants(1, 4); // This index is part of the command signature contract.
    opaque[1].InitAsDescriptorTable(1, &constants);
    opaque[2].InitAsShaderResourceView(30);
    opaque[3].InitAsShaderResourceView(31);
    opaque[4].InitAsDescriptorTable(1, &textures, D3D12_SHADER_VISIBILITY_PIXEL);
    opaque[5].InitAsConstants(3, 5);
    opaque[6].InitAsShaderResourceView(32, 0, D3D12_SHADER_VISIBILITY_PIXEL);
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
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
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
    auto occluder = Shader("shader/hlsl/build/GPUOccluderPS.cso");
    pipeline.PS = {occluder->GetBufferPointer(), occluder->GetBufferSize()};
    // Omit back-facing occluders conservatively; the depth-only shader has no material face test.
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pipeline.NumRenderTargets = 0;
    for (auto& format : pipeline.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
    Check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&state.OccluderPSO)));
    GPUDrivenValidation::Trace("indirect opaque pipeline ready");

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
    if (meshID < state.CpuGeometry.size() && state.CpuGeometry[meshID].Ready)
    {
        MeshletManager::Register(meshID,mesh.MeshletLODs[0],state.CpuGeometry[meshID].BaseVertex);
        return;
    }
    // Procedural triangle lists supply synthesized CPU indices without a source index buffer.
    if (!mesh.VertexBuffer || mesh.CpuVertices.size() != mesh.VertexCount || mesh.CpuIndices.size() != mesh.IndexCount ||
        !mesh.IndexCount || !mesh.VertexCount || mesh.IndexBufferView.Format != DXGI_FORMAT_R32_UINT) return;
    for (uint32_t index : mesh.CpuIndices) if (index >= mesh.VertexCount) return;
    const auto address=mesh.VertexBuffer->GetGPUVirtualAddress();
    if (auto existing=state.GeometryMeshIDs.find(address); existing!=state.GeometryMeshIDs.end())
    {
        for (auto previousID : existing->second)
        {
            const auto previous=state.CpuGeometry.at(previousID);
            if (previous.Ready && previous.IndexCount==mesh.IndexCount &&
                size_t(previous.BaseVertex)+mesh.VertexCount<=state.CpuVertices.size() &&
                size_t(previous.StartIndex)+mesh.IndexCount<=state.CpuIndices.size() &&
                std::equal(mesh.CpuIndices.begin(),mesh.CpuIndices.end(),state.CpuIndices.begin()+previous.StartIndex))
            {
                if (state.CpuGeometry.size()<=meshID) state.CpuGeometry.resize(size_t(meshID)+1);
                state.CpuGeometry[meshID]=previous;
                state.NewGeometry.push_back(meshID);
                existing->second.push_back(meshID);
                MeshletManager::Register(meshID,mesh.MeshletLODs[0],previous.BaseVertex);
                return;
            }
        }
    }
    const auto geometry = ReserveGpuDrivenGeometry(state.CpuVertices.size(), state.CpuIndices.size(), mesh.VertexCount, mesh.IndexCount);
    if (state.CpuGeometry.size() <= meshID) state.CpuGeometry.resize(size_t(meshID) + 1);
    for (uint32_t i = 0; i < mesh.VertexCount; ++i) state.NewVertices.push_back(static_cast<uint32_t>(state.CpuVertices.size()) + i);
    for (uint32_t i = 0; i < mesh.IndexCount; ++i) state.NewIndices.push_back(static_cast<uint32_t>(state.CpuIndices.size()) + i);
    state.CpuVertices.insert(state.CpuVertices.end(), mesh.CpuVertices.begin(), mesh.CpuVertices.end());
    state.CpuIndices.insert(state.CpuIndices.end(), mesh.CpuIndices.begin(), mesh.CpuIndices.end());
    state.CpuGeometry[meshID] = geometry;
    state.NewGeometry.push_back(meshID);
    state.GeometryMeshIDs[mesh.VertexBuffer->GetGPUVirtualAddress()].push_back(meshID);
    MeshletManager::Register(meshID, mesh.MeshletLODs[0], geometry.BaseVertex);
}

void GPUDrivenRenderer::RegisterPrimitiveGeometry(uint32_t meshID, const StaticMeshData &mesh)
{
    auto &state = Data();
    if (mesh.CpuVertices.empty() || mesh.CpuIndices.empty() || !mesh.VirtualGeometryLODs[0])
        return;
    const auto key = mesh.VirtualGeometryLODs[0]->Key;
    auto found = state.PrimitiveRanges.find(meshID);
    if (found != state.PrimitiveRanges.end() && found->second.SourceKey == key)
    {
        MeshletManager::Register(meshID, mesh.MeshletLODs[0], found->second.FirstVertex);
        return;
    }
    State::PrimitiveRange range;
    if (found != state.PrimitiveRanges.end())
    {
        range = found->second;
        if (range.VertexCapacity < mesh.VertexCount || range.IndexCapacity < mesh.IndexCount)
        {
            state.FreePrimitiveRanges.push_back(range);
            range = {};
        }
    }
    if (!range.VertexCapacity)
    {
        auto free = std::find_if(state.FreePrimitiveRanges.begin(), state.FreePrimitiveRanges.end(),
                                 [&](const auto &candidate)
                                 {
                                     return candidate.VertexCapacity >= mesh.VertexCount &&
                                            candidate.IndexCapacity >= mesh.IndexCount;
                                 });
        if (free != state.FreePrimitiveRanges.end())
        {
            range = *free;
            state.FreePrimitiveRanges.erase(free);
        }
        else
        {
            range.VertexCapacity = GpuDrivenCapacity(0, mesh.VertexCount);
            range.IndexCapacity = GpuDrivenCapacity(0, mesh.IndexCount);
            const auto reservation = ReserveGpuDrivenGeometry(
                state.CpuVertices.size(), state.CpuIndices.size(), range.VertexCapacity, range.IndexCapacity);
            range.FirstVertex = uint32_t(reservation.BaseVertex);
            range.FirstIndex = reservation.StartIndex;
            state.CpuVertices.resize(state.CpuVertices.size() + range.VertexCapacity);
            state.CpuIndices.resize(state.CpuIndices.size() + range.IndexCapacity);
        }
    }
    std::copy(mesh.CpuVertices.begin(), mesh.CpuVertices.end(),
              state.CpuVertices.begin() + range.FirstVertex);
    std::copy(mesh.CpuIndices.begin(), mesh.CpuIndices.end(), state.CpuIndices.begin() + range.FirstIndex);
    for (uint32_t i = 0; i < mesh.VertexCount; ++i)
        state.NewVertices.push_back(range.FirstVertex + i);
    for (uint32_t i = 0; i < mesh.IndexCount; ++i)
        state.NewIndices.push_back(range.FirstIndex + i);
    if (state.CpuGeometry.size() <= meshID)
        state.CpuGeometry.resize(size_t(meshID) + 1);
    state.CpuGeometry[meshID] = {range.FirstIndex, mesh.IndexCount, int32_t(range.FirstVertex), 1};
    state.NewGeometry.push_back(meshID);
    range.SourceKey = key;
    state.PrimitiveRanges[meshID] = range;
    MeshletManager::Replace(meshID, mesh.MeshletLODs[0], range.FirstVertex);
}
void GPUDrivenRenderer::ReleasePrimitiveGeometry(uint32_t meshID)
{
    auto &state = Data();
    const auto found = state.PrimitiveRanges.find(meshID);
    if (found == state.PrimitiveRanges.end())
        return;
    state.FreePrimitiveRanges.push_back(found->second);
    state.PrimitiveRanges.erase(found);
    state.CpuGeometry.at(meshID) = {};
    state.NewGeometry.push_back(meshID);
    MeshletManager::Replace(meshID, {}, 0);
}
uint64_t GPUDrivenRenderer::GeometryArenaBytes()
{
    const auto &state = Data();
    return uint64_t(state.CpuVertices.size()) * sizeof(StaticModelVertex) +
           uint64_t(state.CpuIndices.size()) * 4;
}
uint64_t GPUDrivenRenderer::GeometryUploadBytes()
{
    return Data().LastGeometryUploadBytes;
}
void GPUDrivenRenderer::RegisterLODGeometry(uint32_t meshID, uint32_t baseMeshID,
                                            const std::vector<uint32_t> &indices,
                                            const Meshlets::Asset *meshlets)
{
    auto& state = Data();
    if (GeometryReady(meshID))
    {
        if (meshlets) MeshletManager::Register(meshID,*meshlets,state.CpuGeometry[meshID].BaseVertex);
        return;
    }
    if (!GeometryReady(baseMeshID) || indices.empty()) return;
    const auto base = state.CpuGeometry[baseMeshID];
    const auto reservation = ReserveGpuDrivenGeometry(0, state.CpuIndices.size(), 0, static_cast<uint32_t>(indices.size()));
    if (state.CpuGeometry.size() <= meshID) state.CpuGeometry.resize(size_t(meshID) + 1);
    state.CpuGeometry[meshID] = {reservation.StartIndex, reservation.IndexCount, base.BaseVertex, 1};
    for (uint32_t i = 0; i < indices.size(); ++i) state.NewIndices.push_back(static_cast<uint32_t>(state.CpuIndices.size()) + i);
    state.CpuIndices.insert(state.CpuIndices.end(), indices.begin(), indices.end());
    state.NewGeometry.push_back(meshID);
    if (meshlets) MeshletManager::Register(meshID, *meshlets, base.BaseVertex);
}

void GPUDrivenRenderer::UpdateGeometry(const StaticMeshData& mesh)
{
    if (!mesh.VertexBuffer) return;
    auto& state = Data();
    const auto found = state.GeometryMeshIDs.find(mesh.VertexBuffer->GetGPUVirtualAddress());
    if (found == state.GeometryMeshIDs.end()) return;
    std::unordered_set<uint32_t> updatedVertices;
    for (uint32_t meshID : found->second)
    {
        const auto& geometry = state.CpuGeometry.at(meshID);
        // Each registered LOD uses the same source vertices but its own leaf clusters.
        const Meshlets::Asset* asset = &mesh.MeshletLODs[0];
        for (uint32_t lod=1; lod<StaticMeshData::LodCount; ++lod)
            if (geometry.IndexCount == mesh.CpuLodIndices[lod-1].size() &&
                geometry.StartIndex + geometry.IndexCount <= state.CpuIndices.size() &&
                std::equal(mesh.CpuLodIndices[lod-1].begin(),mesh.CpuLodIndices[lod-1].end(),
                    state.CpuIndices.begin()+geometry.StartIndex)) { asset=&mesh.MeshletLODs[lod]; break; }
        MeshletManager::Replace(meshID,*asset,geometry.BaseVertex);
        const size_t first = static_cast<size_t>(geometry.BaseVertex);
        if (!updatedVertices.insert(static_cast<uint32_t>(first)).second) continue;
        if (first + mesh.CpuVertices.size() > state.CpuVertices.size())
            throw std::runtime_error("GPU driven geometry update exceeds registered range");
        std::copy(mesh.CpuVertices.begin(), mesh.CpuVertices.end(), state.CpuVertices.begin() + first);
        for (uint32_t i = 0; i < mesh.CpuVertices.size(); ++i) state.NewVertices.push_back(static_cast<uint32_t>(first) + i);
    }
}

bool GPUDrivenRenderer::SupportsEntity(uint32_t entity)
{
    const bool staticModel = ComponentManager::HasComponent<StaticModelComponent>(entity);
    const bool primitive =
        ComponentManager::HasComponent<MeshComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<MeshComponent>(entity).NaniteMesh != nullptr;
    if ((!staticModel && !primitive) || ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
        !ComponentManager::HasComponent<MaterialComponent>(entity) || VertexDeformation::IsRequested(entity))
        return false;
    if (ComponentManager::HasComponent<MeshComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<MeshComponent>(entity).IsEnvironment)
        return false;
    if (ComponentManager::HasComponent<NameComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky")
        return false;
    const auto &material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    const bool manualPbr =
        material.ShaderClassMode == MaterialMode::Manual && material.ShaderClass == ShaderClass::PBR;
    const bool automaticModel = staticModel && material.ShaderClassMode == MaterialMode::Auto;
    if ((!manualPbr && !automaticModel) || material.IsTransparent || material.Alpha < 1.0f)
        return false;
    if (automaticModel)
    {
        // The GPU deferred shader does not yet reproduce the model normal-map path.
        if (material.NormalMapID >= 0)
            return false;
        const auto *model = ModelManager::GetStaticModel(
            ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity).ModelId);
        if (!model)
            return false;
        for (uint32_t mesh = 0; mesh < model->GetMeshCount(); ++mesh)
        {
            const auto part = model->GetMeshData(mesh).AppliedMaterialPartId;
            if (part == float(ShaderClass::Transparent) || part == float(ShaderClass::Shadow))
                return false;
        }
    }
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto &shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if ((!shader.VsPath.empty() && shader.VsPath != "shader/hlsl/build/modelshaderVS.cso") ||
            (!shader.PsPath.empty() && shader.PsPath != "shader/hlsl/build/modelshaderPS.cso"))
            return false;
    }
    return true;
}

bool GPUDrivenRenderer::GeometryReady(uint32_t meshID)
{
    return meshID < Data().CpuGeometry.size() && Data().CpuGeometry[meshID].Ready != 0;
}

bool GPUDrivenRenderer::HandlesEntity(uint32_t entity)
{
    if (!Data().Active)
        return false;
    const auto &ids = GpuScene::GetEntityInstances(entity);
    if (ids.empty())
        return false;
    for (auto id : ids)
    {
        const auto *instance = GpuScene::GetInstance(id);
        if (!instance || !(instance->Flags & OpaqueInstanceFlag))
            return false;
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
    const bool compatible = state.Enabled && !state.Failed && CompatibleMode();
    if (compatible != state.PreviousCompatible) GPUVisibility::Invalidate("GPU/CPU path changed");
    state.PreviousCompatible = compatible;
    if (!compatible) return;
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
    prepare(frame.Visible, capacity, sizeof(Visibility::VisibleInstanceData), L"GPU Driven Visible LOD Instances", true);
    prepare(frame.Commands, capacity, sizeof(GPUIndirectDrawCommand), L"GPU Driven Commands", true);
    prepare(frame.Count, 1, 4, L"GPU Driven Count", true);
    const uint64_t vertexBytes = state.Vertices.Stage(device, frameIndex, state.CpuVertices.data(), static_cast<uint32_t>(state.CpuVertices.size()), state.NewVertices);
    const uint64_t indexBytes = state.Indices.Stage(device, frameIndex, state.CpuIndices.data(), static_cast<uint32_t>(state.CpuIndices.size()), state.NewIndices);
    const uint64_t meshBytes = state.Geometry.Stage(device, frameIndex, state.CpuGeometry.data(), meshes, state.NewGeometry);
    state.LastGeometryUploadBytes=vertexBytes+indexBytes;
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
    RenderGraph::ResourceHandle selected;
    try { selected = GPUVisibility::Prepare(graph, instances, visibility, geometry, state.Geometry.Resource(), slots, capacity, meshes); }
    catch (const std::exception& error)
    {
        state.Failed = true;
        GPUVisibility::Invalidate("Visibility initialization failed");
        WriteGraphicsLog("GPU Visibility CPU fallback: %s\n", error.what());
        return;
    }
    auto bindCompute = [](ID3D12GraphicsCommandList* list, bool generate) {
        auto& data = Data(); auto& buffers = Frame();
        list->SetComputeRootSignature(data.ComputeRoot.Get());
        list->SetPipelineState(generate ? data.GeneratePSO.Get() : data.CompactPSO.Get());
        list->SetComputeRootShaderResourceView(0, GpuScene::InstanceBuffer()->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1, (generate ? buffers.Visible.Resource() : GPUVisibility::ResultBuffer())->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, data.Geometry.Resource()->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(3, buffers.Count.Resource()->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(4, (generate ? buffers.Commands.Resource() : buffers.Visible.Resource())->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(5, buffers.Count.Resource()->GetGPUVirtualAddress());
        const uint32_t limits[] = {data.Slots, buffers.Commands.Capacity(), data.Meshes};
        list->SetComputeRoot32BitConstants(6, 3, limits, 0);
    };
    graph.AddPass("Compact Visible Instances", [=](RenderGraph::PassBuilder& builder) {
        builder.Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            .Read(selected, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE).Read(geometry, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
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
    MeshShaderRenderer::Prepare(graph, instances, visible, count, arguments, frame.Visible.Resource(), frame.Count.Resource(), frame.Commands.Resource(), frame.Commands.Capacity(), meshes);
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
    if (MeshShaderRenderer::IsActive())
    {
        const D3D12_VERTEX_BUFFER_VIEW vb{state.Vertices.Resource()->GetGPUVirtualAddress(), static_cast<UINT>(state.CpuVertices.size() * sizeof(StaticModelVertex)), sizeof(StaticModelVertex)};
        const D3D12_INDEX_BUFFER_VIEW ib{state.Indices.Resource()->GetGPUVirtualAddress(), static_cast<UINT>(state.CpuIndices.size() * 4), DXGI_FORMAT_R32_UINT};
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->IASetVertexBuffers(0, 1, &vb); commands->IASetIndexBuffer(&ib);
        MeshShaderRenderer::Draw(commands, state.Vertices);
        state.Calls = 2;
        state.SubmissionMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return;
    }
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
    input.Name = "Opaque Visibility Debug"; input.Resource = GPUVisibility::ResultBuffer();
    const auto selected = graph.ImportResource(input);
    graph.AddPass("Opaque GPU Driven", [=](RenderGraph::PassBuilder& builder) {
        const auto sceneRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        builder.Read(instances, sceneRead).Read(materials, sceneRead)
            .Read(selected, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
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
        list->SetGraphicsRootShaderResourceView(6, GPUVisibility::ResultBuffer()->GetGPUVirtualAddress());
        const CD3DX12_GPU_DESCRIPTOR_HANDLE textures(heap->GetGPUDescriptorHandleForHeapStart(), FrameConstants::g_kTEXTURE_SRV_START_INDEX, FrameConstants::GetCbvIncrementSize());
        list->SetGraphicsRootDescriptorTable(4, textures);
        const uint32_t textureLimits[] = {FrameConstants::g_kTEXTURE_SRV_START_INDEX,
            static_cast<uint32_t>(TextureManager::GetDefaultTextureIndex()) - FrameConstants::g_kTEXTURE_SRV_START_INDEX,
            static_cast<uint32_t>(GPUVisibility::GetSettings().DebugMode)};
        list->SetGraphicsRoot32BitConstants(5, 3, textureLimits, 0);
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

void GPUDrivenRenderer::BuildHiZ(ID3D12GraphicsCommandList* commands)
{
    auto& state = Data();
    if (!state.Active || !GPUVisibility::GetSettings().Occlusion || !OcclusionCulling::IsAvailable())
    { OcclusionCulling::InvalidateHistory(); return; }
    RenderGraph graph;
    const auto vertices = Import(graph, "Occluder Vertices", state.Vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    const auto indices = Import(graph, "Occluder Indices", state.Indices, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    const auto arguments = Import(graph, "Occluder Commands", Frame().Commands, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    const auto count = Import(graph, "Occluder Count", Frame().Count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    RenderGraph::ImportedResource input{};
    input.Name = "Opaque Occluder Depth"; input.Resource = OcclusionCulling::GetOccluderDepth();
    input.InitialState = input.FinalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; input.HasFinalState = true;
    const auto depth = graph.ImportResource(input);
    input.Name = "Occluder Instances"; input.Resource = GpuScene::InstanceBuffer();
    const auto instances = graph.ImportResource(input);
    input.Name = "Occluder Materials"; input.Resource = GpuScene::MaterialBuffer();
    const auto materials = graph.ImportResource(input);
    graph.AddPass("Opaque Occluder Depth", [=](RenderGraph::PassBuilder& builder) {
        builder.Read(vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER).Read(indices, D3D12_RESOURCE_STATE_INDEX_BUFFER)
            .Read(arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT).Read(count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
            .Read(instances, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            .Read(materials, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE).Write(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }, [](ID3D12GraphicsCommandList* list) {
        RenderProfiler::ScopedEvent profile("Opaque Occluder Depth", list);
        auto& data = Data();
        const auto dsv = OcclusionCulling::GetOccluderDsv();
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        const D3D12_VIEWPORT viewport{0, 0, float(OcclusionCulling::GetWidth()), float(OcclusionCulling::GetHeight()), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(OcclusionCulling::GetWidth()), LONG(OcclusionCulling::GetHeight())};
        list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heap = FrameConstants::GetCbvHeap(); list->SetDescriptorHeaps(1, &heap);
        list->SetGraphicsRootSignature(data.OpaqueRoot.Get()); list->SetPipelineState(data.OccluderPSO.Get());
        XMMATRIX view, projection; Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        ConstantBuffer3D constants{}; constants.View = XMMatrixTranspose(view); constants.Projection = XMMatrixTranspose(projection);
        list->SetGraphicsRootDescriptorTable(1, FrameConstants::AllocateTransientConstantBuffer(constants));
        list->SetGraphicsRootShaderResourceView(2, GpuScene::InstanceBuffer()->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(3, GpuScene::MaterialBuffer()->GetGPUVirtualAddress());
        const D3D12_VERTEX_BUFFER_VIEW vb{data.Vertices.Resource()->GetGPUVirtualAddress(), uint32_t(data.CpuVertices.size() * sizeof(StaticModelVertex)), sizeof(StaticModelVertex)};
        const D3D12_INDEX_BUFFER_VIEW ib{data.Indices.Resource()->GetGPUVirtualAddress(), uint32_t(data.CpuIndices.size() * sizeof(uint32_t)), DXGI_FORMAT_R32_UINT};
        list->IASetVertexBuffers(0, 1, &vb); list->IASetIndexBuffer(&ib); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->ExecuteIndirect(data.Signature.Get(), Frame().Commands.Capacity(), Frame().Commands.Resource(), 0, Frame().Count.Resource(), 0);
        list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    });
    if (!graph.Compile() || !graph.Execute(commands)) throw std::runtime_error(graph.GetLastError());
    OcclusionCulling::BuildTemporal(commands, OcclusionCulling::GetOccluderDepth(), OcclusionCulling::GetOccluderSrv());
}

void GPUDrivenRenderer::DrawVelocity(ID3D12GraphicsCommandList* commands)
{
    auto& state=Data();
    if(!state.Active||!MeshShaderRenderer::IsActive()) return;
    const D3D12_VERTEX_BUFFER_VIEW vb{state.Vertices.Resource()->GetGPUVirtualAddress(),static_cast<UINT>(state.CpuVertices.size()*sizeof(StaticModelVertex)),sizeof(StaticModelVertex)};
    const D3D12_INDEX_BUFFER_VIEW ib{state.Indices.Resource()->GetGPUVirtualAddress(),static_cast<UINT>(state.CpuIndices.size()*4),DXGI_FORMAT_R32_UINT};
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->IASetVertexBuffers(0,1,&vb); commands->IASetIndexBuffer(&ib);
    MeshShaderRenderer::DrawVelocity(commands,state.Vertices);
}

bool GPUDrivenRenderer::IsEnabled() { return Data().Enabled; }
bool GPUDrivenRenderer::IsActive() { return Data().Active; }
void GPUDrivenRenderer::SetEnabled(bool enabled) { Data().Enabled = enabled; }
void GPUDrivenRenderer::Shutdown() { MeshShaderRenderer::Shutdown(); GPUVisibility::Shutdown(); Data() = {}; }
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
    VirtualGeometryDebug::DrawControls();
    MeshShaderRenderer::DrawDebug();
    GPUVisibility::DrawDebug();
    auto& state = Data();
    if (!ImGui::TreeNode("GPU Driven Rendering")) return;
    ImGui::Checkbox("Enabled", &state.Enabled);
    int geometryPath=state.Enabled?(MeshShaderRenderer::GetSettings().Enabled?2:1):0;
    const char* paths[]={"Traditional","GPU Driven Traditional","GPU Driven Mesh Shader"};
    if(ImGui::Combo("Rendering Geometry Path",&geometryPath,paths,3))
    {
        state.Enabled=geometryPath!=0;
        MeshShaderRenderer::GetSettings().Enabled=geometryPath==2;
        GPUVisibility::Invalidate("Geometry path changed");
    }
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
    MeshShaderRenderer::EndFrame();
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
