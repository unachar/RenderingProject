#include "pch.h"
#include "niagararenderer.h"
#include "thumbnailrenderer.h"
#include "camera.h"
#include "editordescriptors.h"
#include "graphicsdevice.h"
#include "persistentpsocache.h"
#include "rendertargets.h"
#include "scenedepth.h"
#include <cmath>
#include <cstring>

namespace
{
    constexpr UINT previewSize = 768;
    constexpr size_t maxVertices = 4000000;

    struct Vertex
    {
        XMFLOAT3 Position;
        XMFLOAT4 Color;
        XMFLOAT2 UV;
        float Round = 0;
    };

    struct Upload
    {
        ComPtr<ID3D12Resource> Resource;
        uint8_t* Data = nullptr;
        size_t Capacity = 0;
        UINT64 Fence = 0;
    };

    struct State
    {
        ComPtr<ID3D12RootSignature> Root;
        ComPtr<ID3DBlob> VS;
        ComPtr<ID3DBlob> PS;
        ComPtr<ID3D12PipelineState> PreviewPipeline;
        ComPtr<ID3D12PipelineState> GridPipeline;
        ComPtr<ID3D12PipelineState> ScenePipeline;
        ComPtr<ID3D12PipelineState> ThumbnailPipeline;
        std::vector<std::pair<UINT64, ComPtr<ID3D12PipelineState>>> RetiredPipelines;
        DXGI_FORMAT SceneFormat = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D12Resource> Target;
        ComPtr<ID3D12Resource> Depth;
        ComPtr<ID3D12DescriptorHeap> RTV;
        ComPtr<ID3D12DescriptorHeap> DSV;
        std::shared_ptr<EditorDescriptorLease> Descriptor;
        std::vector<Upload> Uploads;
        std::vector<Vertex> PreviewVertices;
        std::vector<Vertex> GridVertices;
        XMFLOAT4X4 PreviewViewProjection{};
        XMFLOAT4 Background{0.025f, 0.028f, 0.035f, 1};
        bool Visible = false;
        std::string Error;
    };

    State state;

    bool InitializeShaders()
    {
        if (state.Root)
        {
            return true;
        }
        auto* device = GraphicsDevice::GetDevice();
        if (!device)
        {
            return false;
        }
        ComPtr<ID3DBlob> diagnostic;
        if (FAILED(D3DCompileFromFile(L"shader/hlsl/niagaraparticles.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                      "VSMain", "vs_5_0", 0, 0, &state.VS, &diagnostic)) ||
            FAILED(D3DCompileFromFile(L"shader/hlsl/niagaraparticles.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                      "PSMain", "ps_5_0", 0, 0, &state.PS, &diagnostic)))
        {
            state.Error = "Niagara particle shader compilation failed.";
            if (diagnostic)
            {
                state.Error.assign(static_cast<const char*>(diagnostic->GetBufferPointer()),
                                   diagnostic->GetBufferSize());
            }
            return false;
        }
        CD3DX12_ROOT_PARAMETER parameter;
        parameter.InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
        CD3DX12_ROOT_SIGNATURE_DESC description;
        description.Init(1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> serialized;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostic)) ||
            FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&state.Root))))
        {
            state.Error = "Niagara particle root signature creation failed.";
            return false;
        }
        return true;
    }

    bool CreatePipeline(DXGI_FORMAT format, bool depth, ComPtr<ID3D12PipelineState>& pipeline, bool depthWrite = false)
    {
        if (!InitializeShaders())
        {
            return false;
        }
        const D3D12_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = state.Root.Get();
        description.VS = {state.VS->GetBufferPointer(), state.VS->GetBufferSize()};
        description.PS = {state.PS->GetBufferPointer(), state.PS->GetBufferSize()};
        description.InputLayout = {elements, static_cast<UINT>(std::size(elements))};
        description.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        description.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        auto& blend = description.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        description.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        description.DepthStencilState.DepthEnable = depth;
        description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        if (depthWrite)
        {
            description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        }
        description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        description.DSVFormat = DXGI_FORMAT_UNKNOWN;
        if (depth)
        {
            description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        }
        description.SampleMask = UINT_MAX;
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = format;
        description.SampleDesc.Count = 1;
        if (FAILED(PersistentPsoCache::Graphics(GraphicsDevice::GetDevice(), description, pipeline)))
        {
            state.Error = "Niagara particle pipeline creation failed.";
            return false;
        }
        return true;
    }

    bool InitializePreview()
    {
        if (state.Target)
        {
            return true;
        }
        if (!GraphicsDevice::GetDevice())
        {
            return false;
        }
        if (!state.PreviewPipeline && !CreatePipeline(DXGI_FORMAT_R8G8B8A8_UNORM, true, state.PreviewPipeline))
        {
            return false;
        }
        if (!state.GridPipeline && !CreatePipeline(DXGI_FORMAT_R8G8B8A8_UNORM, true, state.GridPipeline, true))
        {
            return false;
        }
        auto descriptor = EditorDescriptors::Allocate();
        if (!descriptor)
        {
            state.Error = "No editor texture descriptor is available for Niagara preview.";
            return false;
        }
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto target = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, previewSize, previewSize, 1, 1, 1,
                                                         0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0.035f, 0.043f, 0.055f, 1}};
        auto* device = GraphicsDevice::GetDevice();
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                   IID_PPV_ARGS(&state.Target))))
        {
            state.Error = "Niagara preview texture creation failed.";
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
        heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDescription.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.RTV))))
        {
            state.Target.Reset();
            state.Error = "Niagara preview render target descriptor creation failed.";
            return false;
        }
        const auto depth = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, previewSize, previewSize, 1, 1, 1, 0,
                                                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = SceneDepth::ClearValue;
        heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                                   IID_PPV_ARGS(&state.Depth))) ||
            FAILED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&state.DSV))))
        {
            state.Target.Reset();
            state.Depth.Reset();
            state.Error = "Niagara preview depth creation failed.";
            return false;
        }
        device->CreateDepthStencilView(state.Depth.Get(), nullptr, state.DSV->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(state.Target.Get(), nullptr, state.RTV->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = target.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(state.Target.Get(), &srv, descriptor->Cpu);
        state.Descriptor = std::move(descriptor);
        state.Error.clear();
        return true;
    }

    void Quad(std::vector<Vertex>& vertices, const std::array<XMVECTOR, 4>& points, const XMFLOAT4& color, float round)
    {
        constexpr int indices[] = {0, 1, 2, 2, 1, 3};
        constexpr XMFLOAT2 uv[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        for (int index : indices)
        {
            Vertex vertex{};
            XMStoreFloat3(&vertex.Position, points[index]);
            vertex.Color = color;
            vertex.UV = uv[index];
            vertex.Round = round;
            vertices.push_back(vertex);
        }
    }

    std::vector<Vertex> BuildGrid()
    {
        std::vector<Vertex> vertices;
        constexpr float extent = 10;
        constexpr float halfWidth = 0.008f;
        vertices.reserve(21 * 12);
        for (int line = -10; line <= 10; ++line)
        {
            const float position = static_cast<float>(line);
            XMFLOAT4 colorX{0.16f, 0.18f, 0.21f, 1};
            XMFLOAT4 colorZ = colorX;
            if (line == 0)
            {
                colorX = {0.38f, 0.13f, 0.13f, 1};
                colorZ = {0.13f, 0.22f, 0.38f, 1};
            }
            Quad(vertices,
                 {XMVectorSet(-extent, 0, position - halfWidth, 1), XMVectorSet(extent, 0, position - halfWidth, 1),
                  XMVectorSet(-extent, 0, position + halfWidth, 1), XMVectorSet(extent, 0, position + halfWidth, 1)},
                 colorX, 0);
            Quad(vertices,
                 {XMVectorSet(position - halfWidth, 0, -extent, 1), XMVectorSet(position + halfWidth, 0, -extent, 1),
                  XMVectorSet(position - halfWidth, 0, extent, 1), XMVectorSet(position + halfWidth, 0, extent, 1)},
                 colorZ, 0);
        }
        return vertices;
    }

    std::vector<Vertex> BuildGeometry(const Niagara::System& system, const std::vector<Niagara::Particle>& particles,
                                      FXMMATRIX world, CXMMATRIX view)
    {
        struct Item
        {
            const Niagara::Particle* Particle;
            const Niagara::Emitter* Emitter;
            XMFLOAT3 Position;
            float Depth;
            float Scale;
        };

        std::unordered_map<uint64_t, const Niagara::Emitter*> emitters;
        for (const auto& emitter : system.Emitters)
        {
            emitters.emplace(emitter.Id, &emitter);
        }
        const auto inverseView = XMMatrixInverse(nullptr, view);
        const auto right = XMVector3Normalize(inverseView.r[0]);
        const auto up = XMVector3Normalize(inverseView.r[1]);
        const float worldScale =
            (std::max)({XMVectorGetX(XMVector3Length(world.r[0])), XMVectorGetX(XMVector3Length(world.r[1])),
                        XMVectorGetX(XMVector3Length(world.r[2]))});
        std::vector<Item> items;
        items.reserve(particles.size());
        for (const auto& particle : particles)
        {
            const auto found = emitters.find(particle.EmitterId);
            if (found == emitters.end() || !std::isfinite(particle.Size) || particle.Size <= 0)
            {
                continue;
            }
            auto position = XMLoadFloat3(&particle.Position);
            float scale = 1;
            if (found->second->LocalSpace)
            {
                position = XMVector3TransformCoord(position, world);
                scale = worldScale;
            }
            Item item{&particle, found->second, {}, XMVectorGetZ(XMVector3TransformCoord(position, view)), scale};
            XMStoreFloat3(&item.Position, position);
            if (std::isfinite(item.Depth))
            {
                items.push_back(item);
            }
        }
        struct DrawPrimitive
        {
            const Item* ParticleItem;
            const Item* RibbonStart;
            float Depth;
        };

        std::vector<DrawPrimitive> primitives;
        primitives.reserve(items.size());
        std::unordered_map<uint64_t, std::vector<const Item*>> ribbons;
        for (const auto& item : items)
        {
            if (item.Emitter->Renderer == Niagara::RendererKind::Ribbon)
            {
                ribbons[item.Emitter->Id].push_back(&item);
            }
            else
            {
                primitives.push_back({&item, nullptr, item.Depth});
            }
        }
        for (auto& [emitter, chain] : ribbons)
        {
            // Particle IDs preserve spawn order within each emitter, including interleaved emitters.
            std::sort(chain.begin(), chain.end(), [](const Item* first, const Item* second) {
                return first->Particle->Id < second->Particle->Id;
            });
            for (size_t index = 1; index < chain.size(); ++index)
            {
                const auto* start = chain[index - 1];
                const auto* end = chain[index];
                primitives.push_back({end, start, start->Depth * 0.5f + end->Depth * 0.5f});
            }
        }
        std::sort(primitives.begin(), primitives.end(), [](const DrawPrimitive& first, const DrawPrimitive& second) {
            if (first.Depth != second.Depth)
            {
                return first.Depth > second.Depth;
            }
            return first.ParticleItem->Particle->Id < second.ParticleItem->Particle->Id;
        });

        std::vector<Vertex> vertices;
        vertices.reserve((std::min)(primitives.size() * 6, maxVertices));
        for (const auto& primitive : primitives)
        {
            const auto& item = *primitive.ParticleItem;
            const size_t vertexCount = item.Emitter->Renderer == Niagara::RendererKind::Mesh ? 36 : 6;
            if (vertices.size() + vertexCount > maxVertices)
            {
                state.Error = "Niagara render vertex budget reached; reduce the particle count.";
                break;
            }
            if (primitive.RibbonStart)
            {
                const auto first = XMLoadFloat3(&primitive.RibbonStart->Position);
                const auto second = XMLoadFloat3(&item.Position);
                const auto tangent = second - first;
                if (XMVectorGetX(XMVector3LengthSq(tangent)) < 0.0000001f)
                {
                    continue;
                }
                const auto cameraDirection = inverseView.r[3] - (first + second) * 0.5f;
                auto side = XMVector3Cross(tangent, cameraDirection);
                if (XMVectorGetX(XMVector3LengthSq(side)) < 0.0000001f)
                {
                    side = right;
                }
                side = XMVectorScale(XMVector3Normalize(side), item.Particle->Size * item.Scale * 0.5f);
                Quad(vertices, {first - side, first + side, second - side, second + side}, item.Particle->Color, 0);
                continue;
            }
            const auto center = XMLoadFloat3(&item.Position);
            const float radius = item.Particle->Size * item.Scale * 0.5f;
            const auto horizontal = XMVectorScale(right, radius);
            const auto vertical = XMVectorScale(up, radius);
            if (item.Emitter->Renderer == Niagara::RendererKind::Mesh)
            {
                // Built-in unit cube. Each face receives a fixed light factor to make its volume readable.
                for (int axis = 0; axis < 3; ++axis)
                {
                    for (int side = -1; side <= 1; side += 2)
                    {
                        std::array<XMVECTOR, 4> corners;
                        for (int corner = 0; corner < 4; ++corner)
                        {
                            float xyz[3]{};
                            xyz[axis] = static_cast<float>(side);
                            xyz[(axis + 1) % 3] = -1.0f + 2.0f * (corner & 1);
                            xyz[(axis + 2) % 3] = -1.0f + static_cast<float>(corner & 2);
                            auto offset = XMVectorSet(xyz[0], xyz[1], xyz[2], 0);
                            offset = XMVectorScale(offset, item.Particle->Size * 0.5f);
                            auto local = XMVectorAdd(XMLoadFloat3(&item.Particle->Position), offset);
                            if (item.Emitter->LocalSpace)
                            {
                                local = XMVector3TransformCoord(local, world);
                            }
                            corners[corner] = local;
                        }
                        auto color = item.Particle->Color;
                        const float lighting = 0.55f + axis * 0.15f;
                        color.x *= lighting;
                        color.y *= lighting;
                        color.z *= lighting;
                        Quad(vertices, corners, color, 0);
                    }
                }
            }
            else
            {
                Quad(vertices,
                     {center - horizontal + vertical, center + horizontal + vertical, center - horizontal - vertical,
                      center + horizontal - vertical},
                     item.Particle->Color, 1);
            }
        }
        return vertices;
    }

    void DrawVertices(const std::vector<Vertex>& vertices, FXMMATRIX viewProjection, ID3D12PipelineState* pipeline)
    {
        if (vertices.empty() || !pipeline)
        {
            return;
        }
        const size_t bytes = vertices.size() * sizeof(Vertex);
        const auto completed = GraphicsDevice::GetCompletedFence();
        Upload* upload = nullptr;
        for (auto& candidate : state.Uploads)
        {
            if (candidate.Fence <= completed && candidate.Capacity >= bytes)
            {
                upload = &candidate;
                break;
            }
        }
        if (!upload)
        {
            // Completed allocations can be replaced. Keeping every smaller historical size
            // would otherwise retain an unbounded pool as an emitter ramps up.
            std::erase_if(state.Uploads, [completed](Upload& candidate) {
                if (candidate.Fence > completed)
                {
                    return false;
                }
                if (candidate.Data)
                {
                    candidate.Resource->Unmap(0, nullptr);
                }
                return true;
            });
            Upload created;
            created.Capacity = (bytes + 65535) & ~size_t(65535);
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(created.Capacity);
            if (FAILED(GraphicsDevice::GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                            IID_PPV_ARGS(&created.Resource))))
            {
                state.Error = "Niagara particle vertex upload allocation failed.";
                return;
            }
            const D3D12_RANGE readRange{0, 0};
            if (FAILED(created.Resource->Map(0, &readRange, reinterpret_cast<void**>(&created.Data))))
            {
                state.Error = "Niagara particle vertex upload mapping failed.";
                return;
            }
            state.Uploads.push_back(std::move(created));
            upload = &state.Uploads.back();
        }
        std::memcpy(upload->Data, vertices.data(), bytes);
        upload->Fence = GraphicsDevice::GetPendingSubmissionFence();
        auto* commands = GraphicsDevice::GetCommandList();
        commands->SetGraphicsRootSignature(state.Root.Get());
        commands->SetPipelineState(pipeline);
        XMFLOAT4X4 matrix;
        XMStoreFloat4x4(&matrix, viewProjection);
        commands->SetGraphicsRoot32BitConstants(0, 16, &matrix, 0);
        const D3D12_VERTEX_BUFFER_VIEW view{upload->Resource->GetGPUVirtualAddress(), static_cast<UINT>(bytes),
                                            sizeof(Vertex)};
        commands->IASetVertexBuffers(0, 1, &view);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(static_cast<UINT>(vertices.size()), 1, 0, 0);
    }
}

void NiagaraRenderer::BeginFrame()
{
    state.Visible = false;
    const auto completed = GraphicsDevice::GetCompletedFence();
    std::erase_if(state.RetiredPipelines, [completed](const auto& pipeline) {
        return pipeline.first <= completed;
    });
}

void NiagaraRenderer::SetPreview(const Niagara::System& system, const std::vector<Niagara::Particle>& particles,
                                 float yaw, float pitch, float distance, const XMFLOAT3& targetPosition, bool grid,
                                 const XMFLOAT4& background)
{
    state.Visible = true;
    if (!InitializePreview())
    {
        return;
    }
    distance = (std::max)(0.1f, distance);
    pitch = std::clamp(pitch, -1.5f, 1.5f);
    state.Background = background;
    const auto target = XMLoadFloat3(&targetPosition);
    const auto eye = target + XMVectorSet(sinf(yaw) * cosf(pitch) * distance, sinf(pitch) * distance,
                                          -cosf(yaw) * cosf(pitch) * distance, 0);
    const auto view = XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0));
    const auto projection = SceneDepth::InfiniteReverseZPerspectiveLH(XM_PIDIV4, 1, 0.05f);
    XMStoreFloat4x4(&state.PreviewViewProjection, view * projection);
    state.PreviewVertices = BuildGeometry(system, particles, XMMatrixIdentity(), view);
    if (grid)
    {
        if (state.GridVertices.empty())
        {
            state.GridVertices = BuildGrid();
        }
    }
    else
    {
        state.GridVertices.clear();
    }
}

ImTextureID NiagaraRenderer::PreviewTexture()
{
    if (!state.Descriptor)
    {
        return ImTextureID{};
    }
    return static_cast<ImTextureID>(state.Descriptor->Gpu.ptr);
}

std::shared_ptr<NiagaraThumbnailFrame> NiagaraRenderer::PrepareThumbnail(const std::filesystem::path& path,
                                                                       std::string& error)
{
    auto frame = std::make_shared<NiagaraThumbnailFrame>();
    if (!Niagara::Load(path, frame->System, error))
    {
        return {};
    }
    Niagara::Runtime runtime;
    auto simulation = frame->System;
    for (auto& emitter : simulation.Emitters)
    {
        // Thumbnail preparation runs on a worker. Do not touch the live GPU simulation's shared command state.
        emitter.Simulation = Niagara::Backend::CPU;
    }
    runtime.Reset(simulation);
    // A fixed seed and bounded sampling keep the thumbnail stable across cache rebuilds.
    // Capture the most populated frame, including short bursts near the start.
    const float endTime = (std::min)(frame->System.Duration, 5.0f);
    const int steps = (std::max)(1, static_cast<int>(endTime * 60.0f));
    for (int step = 0; step < steps && !runtime.Failed(); ++step)
    {
        runtime.Advance(1.0f / 60.0f);
        if (runtime.Particles().size() > frame->Particles.size())
        {
            frame->Particles = runtime.Particles();
        }
    }
    if (runtime.Failed())
    {
        error = runtime.Diagnostic();
        return {};
    }
    return frame;
}

std::shared_ptr<ThumbnailGpuImage> NiagaraRenderer::RenderThumbnail(
    const NiagaraThumbnailFrame& frame, UINT size, std::shared_ptr<EditorDescriptorLease> descriptor)
{
    auto* device = GraphicsDevice::GetDevice();
    auto* commands = GraphicsDevice::GetCommandList();
    if (!device || !commands || !descriptor || size == 0)
    {
        return {};
    }
    if (!state.ThumbnailPipeline &&
        !CreatePipeline(DXGI_FORMAT_R8G8B8A8_UNORM, false, state.ThumbnailPipeline))
    {
        return {};
    }
    const auto view = XMMatrixLookAtLH(XMVectorSet(4, 3, -6, 1), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    const auto vertices = BuildGeometry(frame.System, frame.Particles, XMMatrixIdentity(), view);
    XMFLOAT3 minimum{FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 maximum{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& vertex : vertices)
    {
        XMFLOAT3 position;
        XMStoreFloat3(&position, XMVector3TransformCoord(XMLoadFloat3(&vertex.Position), view));
        minimum.x = (std::min)(minimum.x, position.x);
        minimum.y = (std::min)(minimum.y, position.y);
        minimum.z = (std::min)(minimum.z, position.z);
        maximum.x = (std::max)(maximum.x, position.x);
        maximum.y = (std::max)(maximum.y, position.y);
        maximum.z = (std::max)(maximum.z, position.z);
    }
    auto projection = XMMatrixIdentity();
    if (!vertices.empty())
    {
        // Crop around the effect itself, with a small border and no editor grid or material sphere.
        float halfSize = (std::max)({maximum.x - minimum.x, maximum.y - minimum.y, 0.01f}) * 0.55f;
        float centerX = (minimum.x + maximum.x) * 0.5f;
        float centerY = (minimum.y + maximum.y) * 0.5f;
        const Niagara::Particle* focalParticle = nullptr;
        float focalSize = 0;
        for (const auto& particle : frame.Particles)
        {
            if (std::isfinite(particle.Size) && particle.Color.w > 0.1f && particle.Size > focalSize)
            {
                focalParticle = &particle;
                focalSize = particle.Size;
            }
        }
        // Long, sparse trails can shrink every particle below one pixel. In that case,
        // crop a visible part of the effect instead of producing an apparently empty texture.
        const float croppedHalfSize = focalSize * static_cast<float>(size) / 6.0f;
        if (focalParticle && croppedHalfSize > 0 && croppedHalfSize < halfSize)
        {
            XMFLOAT3 focus;
            XMStoreFloat3(&focus, XMVector3TransformCoord(XMLoadFloat3(&focalParticle->Position), view));
            centerX = focus.x;
            centerY = focus.y;
            halfSize = croppedHalfSize;
        }
        projection = XMMatrixOrthographicOffCenterLH(centerX - halfSize, centerX + halfSize,
                                                     centerY - halfSize, centerY + halfSize,
                                                     minimum.z - 1.0f, maximum.z + 1.0f);
    }
    auto image = std::make_shared<ThumbnailGpuImage>();
    image->Descriptor = std::move(descriptor);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto target = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, 1, 1, 0,
                                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0.025f, 0.028f, 0.035f, 1}};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                IID_PPV_ARGS(&image->Resource))))
    {
        return {};
    }
    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    views.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&image->RenderTargetViews))))
    {
        return {};
    }
    const auto rtv = image->RenderTargetViews->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(image->Resource.Get(), nullptr, rtv);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = target.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(image->Resource.Get(), &srv, image->Descriptor->Cpu);
    commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commands->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
    const CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size));
    const CD3DX12_RECT scissor(0, 0, size, size);
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    DrawVertices(vertices, view * projection, state.ThumbnailPipeline.Get());
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(image->Resource.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &barrier);
    image->Width = image->Height = size;
    image->Bytes = device->GetResourceAllocationInfo(0, 1, &target).SizeInBytes;
    image->SubmissionFence = GraphicsDevice::GetPendingSubmissionFence();
    return image;
}

void NiagaraRenderer::RenderPreview()
{
    if (!state.Visible || !state.Target || !GraphicsDevice::GetCommandList())
    {
        return;
    }
    auto* commands = GraphicsDevice::GetCommandList();
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(state.Target.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands->ResourceBarrier(1, &barrier);
    const auto rtv = state.RTV->GetCPUDescriptorHandleForHeapStart();
    commands->ClearRenderTargetView(rtv, &state.Background.x, 0, nullptr);
    const auto dsv = state.DSV->GetCPUDescriptorHandleForHeapStart();
    commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, SceneDepth::ClearValue, 0, 0, nullptr);
    commands->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(previewSize), static_cast<float>(previewSize));
    const CD3DX12_RECT scissor(0, 0, previewSize, previewSize);
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    const auto viewProjection = XMLoadFloat4x4(&state.PreviewViewProjection);
    DrawVertices(state.GridVertices, viewProjection, state.GridPipeline.Get());
    DrawVertices(state.PreviewVertices, viewProjection, state.PreviewPipeline.Get());
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(state.Target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &barrier);
}

void NiagaraRenderer::DrawScene(const Niagara::System& system, const std::vector<Niagara::Particle>& particles,
                                const XMMATRIX& world)
{
    if (particles.empty() || !GraphicsDevice::GetCommandList())
    {
        return;
    }
    auto* target = RenderTargets::GetEditorSceneResource();
    if (!target)
    {
        return;
    }
    const auto format = target->GetDesc().Format;
    if (!state.ScenePipeline || state.SceneFormat != format)
    {
        if (state.ScenePipeline)
        {
            state.RetiredPipelines.emplace_back(GraphicsDevice::GetPendingSubmissionFence(),
                                                std::move(state.ScenePipeline));
        }
        if (!CreatePipeline(format, true, state.ScenePipeline))
        {
            return;
        }
        state.SceneFormat = format;
    }
    XMMATRIX view;
    XMMATRIX projection;
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    const auto vertices = BuildGeometry(system, particles, world, view);
    DrawVertices(vertices, view * projection, state.ScenePipeline.Get());
}

void NiagaraRenderer::Shutdown()
{
    if (GraphicsDevice::GetDevice() && !GraphicsDevice::WaitForGpuIdle())
    {
        if (SUCCEEDED(GraphicsDevice::GetDevice()->GetDeviceRemovedReason()))
        {
            state.Error = "Niagara renderer shutdown deferred: GPU idle wait failed; in-flight resources retained.";
            Debug::Log("%s\n", state.Error.c_str());
            return;
        }
        // Device removal guarantees that its pending commands cannot use these resources again.
        Debug::Log("Niagara renderer released resources after device removal.\n");
    }
    for (auto& upload : state.Uploads)
    {
        if (upload.Data)
        {
            upload.Resource->Unmap(0, nullptr);
        }
    }
    state = State{};
}

const std::string& NiagaraRenderer::LastError()
{
    return state.Error;
}
