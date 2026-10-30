#include "pch.h"
#include "rendering/scenedepth.h"
#include "materialpreview.h"
#include "animationpreviewcontext.h"
#include "animationmodel.h"
#include "thumbnailpreviewcontext.h"
#include "assets/thumbnailgeometry.h"
#include "materialmanager.h"
#include "materialbindings.h"
#include "frameconstants.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "shaderdescription.h"
#include "scenepasses.h"
#include "texturemanager.h"
#include "staticmodel.h"
#include "imguimanager.h"
#include "materialvalidation.h"
#include "rendererstate.h"
#include <cmath>
#include <mutex>

namespace
{
    constexpr UINT previewSize = 512;
    constexpr UINT constantBytes = 32768; // 本番のライトバッファの予約サイズに合わせる。
    std::mutex thumbnailPipelineMutex;
    std::array<ComPtr<ID3D12PipelineState>, 12> thumbnailPipelines;
    DXGI_FORMAT thumbnailPipelineFormat = DXGI_FORMAT_UNKNOWN;

    struct PreviewState : RendererState
    {
        UINT Size = previewSize;
        UINT SrvIndex = g_kMATERIAL_PREVIEW_SRV_INDEX;
        UINT TextureBase = g_kTHUMBNAIL_DESCRIPTOR_START;
        ComPtr<ID3D12Resource> PreviewConstants;
        XMFLOAT3 Center{};
        float Near = 0.05f;
        float Far = 100.0f;
        bool Visible = false;
        AssetHandle Material = 0;
        int Shape = 0;
        float Yaw = 0.45f;
        float Pitch = 0.2f;
        float Distance = 4.5f;
        float LightIntensity = 2.0f;
        bool SceneEnvironment = false;
        string Error;
        ComPtr<ID3D12Resource> Target;
        ComPtr<ID3D12Resource> Depth;
        ComPtr<ID3D12DescriptorHeap> RTVHeap;
        ComPtr<ID3D12DescriptorHeap> DSVHeap;
        ComPtr<ID3D12Resource> Lights;
        ComPtr<ID3D12Resource> OpticsGlobals;
        uint8_t* MappedOpticsGlobals = nullptr;
        uint8_t* MappedLights = nullptr;
        array<ComPtr<ID3D12Resource>, 3> Vertices;
        array<D3D12_VERTEX_BUFFER_VIEW, 3> Views{};
        array<UINT, 3> Counts{};
        array<ComPtr<ID3D12PipelineState>, 24> Pipelines;

        D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv() const
        {
            return CD3DX12_CPU_DESCRIPTOR_HANDLE(FrameConstants::GetCbvHeap()->GetCPUDescriptorHandleForHeapStart(),
                                                 SrvIndex, FrameConstants::GetCbvIncrementSize());
        }

        D3D12_GPU_DESCRIPTOR_HANDLE GpuSrv() const
        {
            return CD3DX12_GPU_DESCRIPTOR_HANDLE(FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart(),
                                                 SrvIndex, FrameConstants::GetCbvIncrementSize());
        }

        bool Initialize();
        ID3D12PipelineState* Pipeline(const MaterialComponent& material);
    };

    PreviewState preview;

    StaticModelVertex Vertex(float x, float y, float z, float nx, float ny, float nz, float u, float v)
    {
        return {{x, y, z}, {nx, ny, nz}, {u, v}, {1, 1, 1, 11}};
    }

    void Triangle(vector<StaticModelVertex>& vertices, StaticModelVertex first, StaticModelVertex second,
                  StaticModelVertex third)
    {
        const auto edge1 = XMVectorSubtract(XMLoadFloat3(&second.Position), XMLoadFloat3(&first.Position));
        const auto edge2 = XMVectorSubtract(XMLoadFloat3(&third.Position), XMLoadFloat3(&first.Position));
        if (XMVectorGetX(XMVector3Dot(XMVector3Cross(edge1, edge2), XMLoadFloat3(&first.Normal))) < 0)
        {
            swap(second, third);
        }
        vertices.push_back(first);
        vertices.push_back(second);
        vertices.push_back(third);
    }

    vector<StaticModelVertex> MakeGeometry(int shape)
    {
        vector<StaticModelVertex> vertices;
        if (shape == 0)
        {
            auto sphereVertex = [](int latitude, int longitude) {
                const float u = longitude / 32.0f;
                const float v = latitude / 16.0f;
                const float angle = u * XM_2PI;
                const float polar = v * XM_PI;
                const float x = sinf(polar) * cosf(angle);
                const float y = cosf(polar);
                const float z = sinf(polar) * sinf(angle);
                return Vertex(x, y, z, x, y, z, u, v);
            };
            for (int latitude = 0; latitude < 16; ++latitude)
            {
                for (int longitude = 0; longitude < 32; ++longitude)
                {
                    const auto first = sphereVertex(latitude, longitude);
                    const auto second = sphereVertex(latitude + 1, longitude);
                    const auto third = sphereVertex(latitude + 1, longitude + 1);
                    const auto fourth = sphereVertex(latitude, longitude + 1);
                    Triangle(vertices, first, second, third);
                    Triangle(vertices, first, third, fourth);
                }
            }
        }
        else
        {
            const int faces = 6;
            for (int face = 0; face < faces; ++face)
            {
                if (shape == 2 && face != 4)
                {
                    continue;
                }
                const int axis = face / 2;
                float direction = -1.0f;
                if ((face & 1) != 0)
                {
                    direction = 1.0f;
                }
                array<StaticModelVertex, 4> corners;
                for (int corner = 0; corner < 4; ++corner)
                {
                    float position[3]{};
                    float normal[3]{};
                    position[axis] = direction;
                    normal[axis] = direction;
                    position[(axis + 1) % 3] = ((corner & 1) != 0) ? 1.0f : -1.0f;
                    position[(axis + 2) % 3] = ((corner & 2) != 0) ? 1.0f : -1.0f;
                    if (shape == 2)
                    {
                        position[axis] = 0;
                    }
                    // Image UVs start at the top left when looking at the outside of a face.
                    // Derive them from a face-local right/up frame, not the corner bit order.
                    float horizontal;
                    float vertical;
                    if (axis == 1)
                    {
                        horizontal = position[0];
                        vertical = direction * position[2];
                    }
                    else
                    {
                        horizontal = -normal[2] * position[0] + normal[0] * position[2];
                        vertical = position[1];
                    }
                    const float u = (horizontal + 1.0f) * 0.5f;
                    const float v = (1.0f - vertical) * 0.5f;
                    corners[corner] = Vertex(position[0], position[1], position[2], normal[0], normal[1], normal[2],
                                             u, v);
                }
                Triangle(vertices, corners[0], corners[1], corners[2]);
                Triangle(vertices, corners[1], corners[3], corners[2]);
            }
        }
        return vertices;
    }

    bool PreviewState::Initialize()
    {
        if (Target)
        {
            return true;
        }
        auto* device = GraphicsDevice::GetDevice();
        if (!device)
        {
            return false;
        }
        const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto target = CD3DX12_RESOURCE_DESC::Tex2D(GetSceneColorFormat(), Size, Size, 1, 1, 1, 0,
                                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_CLEAR_VALUE clear{};
        clear.Format = target.Format;
        clear.Color[0] = 0.12f;
        clear.Color[1] = 0.13f;
        clear.Color[2] = 0.16f;
        clear.Color[3] = 1;
        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &target,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                   IID_PPV_ARGS(&Target))))
        {
            Error = "Failed to create material preview target";
            return false;
        }
        target.Format = DXGI_FORMAT_D32_FLOAT;
        target.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clear.Format = target.Format;
        clear.DepthStencil.Depth = 0.0f;
        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &target,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&Depth))))
        {
            Error = "Failed to create preview depth";
            Target.Reset();
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.NumDescriptors = 1;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&RTVHeap))))
        {
            Target.Reset();
            return false;
        }
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&DSVHeap))))
        {
            Target.Reset();
            return false;
        }
        device->CreateRenderTargetView(Target.Get(), nullptr, RTVHeap->GetCPUDescriptorHandleForHeapStart());
        device->CreateDepthStencilView(Depth.Get(), nullptr, DSVHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = Target->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(Target.Get(), &srv, CpuSrv());
        const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto lightDescription = CD3DX12_RESOURCE_DESC::Buffer(constantBytes * g_kFRAME_COUNT);
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &lightDescription,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&Lights))) ||
            FAILED(Lights->Map(0, nullptr, reinterpret_cast<void**>(&MappedLights))))
        {
            Error = "Failed to create preview lighting constants";
            Target.Reset();
            return false;
        }
        memset(MappedLights, 0, constantBytes * g_kFRAME_COUNT);
        // b5 はフラグ、パラメーター、160個のライトフラグを含む。2592バイトを2816バイトに整列する。
        const auto opticsDescription = CD3DX12_RESOURCE_DESC::Buffer(2816 * g_kFRAME_COUNT);
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &opticsDescription,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&OpticsGlobals))) ||
            FAILED(OpticsGlobals->Map(0, nullptr, reinterpret_cast<void**>(&MappedOpticsGlobals))))
        {
            Error = "Failed to create material optics preview constants";
            Target.Reset();
            return false;
        }
        memset(MappedOpticsGlobals, 0, 2816 * g_kFRAME_COUNT);
        const int shapeCount = SrvIndex == g_kMATERIAL_PREVIEW_SRV_INDEX ? 3 : 1;
        for (int shape = 0; shape < shapeCount; ++shape)
        {
            const auto vertices = MakeGeometry(shape);
            const UINT bytes = static_cast<UINT>(vertices.size() * sizeof(StaticModelVertex));
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&Vertices[shape]))))
            {
                Error = "Failed to create preview mesh";
                Target.Reset();
                return false;
            }
            void* mapped = nullptr;
            if (FAILED(Vertices[shape]->Map(0, nullptr, &mapped)))
            {
                Target.Reset();
                return false;
            }
            memcpy(mapped, vertices.data(), bytes);
            Vertices[shape]->Unmap(0, nullptr);
            Counts[shape] = static_cast<UINT>(vertices.size());
            Views[shape] = {Vertices[shape]->GetGPUVirtualAddress(), bytes, sizeof(StaticModelVertex)};
        }
        return true;
    }

    ID3D12PipelineState* PreviewState::Pipeline(const MaterialComponent& material)
    {
        size_t index = static_cast<size_t>(material.AssetCull);
        if (!material.AssetDepthWrite || material.IsTransparent)
        {
            index += 3;
        }
        if (material.IsTransparent)
        {
            index += 6;
        }
        if (SrvIndex == g_kMATERIAL_PREVIEW_SRV_INDEX && material.Transmission > 0.0f)
        {
            index += 12;
        }
        if (Pipelines[index])
        {
            return Pipelines[index].Get();
        }
        // Thumbnail targets have independent constants, but share identical pipeline states.
        const bool thumbnail = SrvIndex != g_kMATERIAL_PREVIEW_SRV_INDEX;
        std::unique_lock cacheLock(thumbnailPipelineMutex, std::defer_lock);
        if (thumbnail)
        {
            cacheLock.lock();
            const auto format = Target->GetDesc().Format;
            if (thumbnailPipelineFormat != format)
            {
                thumbnailPipelines = {};
                thumbnailPipelineFormat = format;
            }
            if (thumbnailPipelines[index])
            {
                Pipelines[index] = thumbnailPipelines[index];
                return Pipelines[index].Get();
            }
        }
        ComPtr<ID3DBlob> vertex;
        ComPtr<ID3DBlob> pixel;
        ShaderDescription shader;
        shader.csoPath = "shader/hlsl/build/modelshaderVS.cso";
        shader.ppBlob = vertex.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(shader))
        {
            Error = "Preview failed to load production vertex shader";
            return nullptr;
        }
        shader.csoPath = "shader/hlsl/build/modelshaderPS.cso";
        shader.ppBlob = pixel.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(shader))
        {
            Error = "Preview failed to load production PBR shader";
            return nullptr;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = ShaderPipelines::GetModelRootSignature();
        description.VS = CD3DX12_SHADER_BYTECODE(vertex.Get());
        description.PS = CD3DX12_SHADER_BYTECODE(pixel.Get());
        description.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        if (material.AssetCull == MaterialCull::None)
        {
            description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        }
        else if (material.AssetCull == MaterialCull::Front)
        {
            description.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        }
        description.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
        if (!material.AssetDepthWrite || material.IsTransparent)
        {
            description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
        if (SrvIndex == g_kMATERIAL_PREVIEW_SRV_INDEX && material.Transmission > 0.0f)
        {
            description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        }
        description.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        if (material.IsTransparent)
        {
            auto& blend = description.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        }
        description.SampleMask = UINT_MAX;
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = Target->GetDesc().Format;
        description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        description.SampleDesc.Count = 1;
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        description.InputLayout = {layout, 4};
        if (FAILED(GraphicsDevice::GetDevice()->CreateGraphicsPipelineState(&description,
                                                                            IID_PPV_ARGS(&Pipelines[index]))))
        {
            Error = "Failed to create production preview PSO";
            return nullptr;
        }
        if (thumbnail)
        {
            thumbnailPipelines[index] = Pipelines[index];
        }
        return Pipelines[index].Get();
    }
} // 名前空間

void MaterialPreview::SetVisible(bool visible)
{
    preview.Visible = visible;
}

void MaterialPreview::DrawControls()
{
    ImGui::SetNextItemWidth(95);
    ImGui::Combo("##PreviewMesh", &preview.Shape, "Sphere\0Cube\0Plane\0");
    ImGui::SameLine();
    if (ImGui::Button("Reset Camera"))
    {
        preview.Yaw = 0.45f;
        preview.Pitch = 0.2f;
        preview.Distance = 4.5f;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95);
    ImGui::SliderFloat("Light", &preview.LightIntensity, 0, 5);
    ImGui::SameLine();
    const bool glassMaterial =
        preview.Material != 0 && MaterialManager::GetRuntime(preview.Material).Binding.Transmission > 0.0f;
    ImGui::BeginDisabled(glassMaterial);
    ImGui::Checkbox("Scene IBL", &preview.SceneEnvironment);
    ImGui::EndDisabled();
    if (glassMaterial)
    {
        ImGui::SetItemTooltip(
            "IBL is disabled for transmissive glass. Direct lighting and refracted background remain enabled.");
    }
}

void MaterialPreview::DrawImage(AssetHandle material)
{
    preview.Material = material;
    preview.Visible = true;
    if (!preview.Initialize())
    {
        ImGui::TextWrapped("%s", preview.Error.c_str());
        return;
    }
    const auto available = ImGui::GetContentRegionAvail();
    const float side = min(available.x, available.y);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::ImageButton("##PreviewOrbit", reinterpret_cast<ImTextureData*>(preview.GpuSrv().ptr), ImVec2(side, side),
                       ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0));
    ImGui::PopStyleVar();
    const auto imageMinimum = ImGui::GetItemRectMin();
    const auto imageMaximum = ImGui::GetItemRectMax();
    MaterialValidation::CaptureDropTarget("Preview", imageMinimum.x, imageMinimum.y, imageMaximum.x, imageMaximum.y);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        const auto& input = ImGui::GetIO();
        preview.Yaw += input.MouseDelta.x * 0.01f;
        preview.Pitch = clamp(preview.Pitch + input.MouseDelta.y * 0.01f, -1.4f, 1.4f);
    }
    if (ImGui::IsItemHovered())
    {
        auto& input = ImGui::GetIO();
        preview.Distance = clamp(preview.Distance - input.MouseWheel * 0.25f, 1.5f, 12.0f);
    }
    if (!preview.Error.empty())
    {
        ImGui::TextWrapped("%s", preview.Error.c_str());
    }
}

namespace
{
    bool RenderPreview(PreviewState& preview, const MaterialComponent& material, UINT firstVertex = 0,
                       UINT vertexCount = 0, UINT partIndex = 0, UINT partCount = 1, const MeshData* mesh = nullptr)
    {
        auto* pipeline = preview.Pipeline(material);
        if (!pipeline)
        {
            return false;
        }
        auto* commands = GraphicsDevice::GetCommandList();
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            preview.Target.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (partIndex == 0)
        {
            commands->ResourceBarrier(1, &barrier);
        }
        const auto rtv = preview.RTVHeap->GetCPUDescriptorHandleForHeapStart();
        const auto dsv = preview.DSVHeap->GetCPUDescriptorHandleForHeapStart();
        const float background[4] = {0.12f, 0.13f, 0.16f, 1};
        if (partIndex == 0)
        {
            commands->ClearRenderTargetView(rtv, background, 0, nullptr);
            commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }
        commands->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(preview.Size), static_cast<float>(preview.Size),
                                         0, 1};
        const D3D12_RECT scissor = {0, 0, static_cast<LONG>(preview.Size), static_cast<LONG>(preview.Size)};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        ScenePasses::BeginPass(ShaderPipelines::GetModelRootSignature(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->SetPipelineState(pipeline);
        auto* lights =
            reinterpret_cast<XMFLOAT4*>(preview.MappedLights + GraphicsDevice::GetFrameIndex() * constantBytes);
        memset(lights, 0, constantBytes);
        lights[0] = {-0.5f, 0.7f, -0.8f, 0};
        lights[1] = {1.0f, 0.96f, 0.9f, preview.LightIntensity};
        lights[4] = {1, 0, 0, 0};
        lights[5] = lights[0];
        lights[5 + RendererState::g_kMAX_SHADER_LIGHTS] = lights[1];
        // common.hlsl の LightParams と同じく、4つのベクトル配列の後に行列配列を置く。
        constexpr size_t shadowDataOffset = 5 + 8 * RendererState::g_kMAX_SHADER_LIGHTS;
        constexpr size_t flagsOffset = shadowDataOffset + RendererState::g_kMAX_SHADER_LIGHTS;
        lights[shadowDataOffset] = {-1, 0, 0, 0}; // スタジオライトにはシーンのシャドウマップがない。
        lights[flagsOffset] = {1, 1, 0, 0};       // 不透明パスとフォワードパスで物理ライティングを有効にする。
        commands->SetGraphicsRootConstantBufferView(2, preview.Lights->GetGPUVirtualAddress() +
                                                           GraphicsDevice::GetFrameIndex() * constantBytes);
        // 独立したプレビューシーンのため、影とレイのフラグをゼロにする。
        const auto zeroAddress = preview.Lights->GetGPUVirtualAddress() +
                                 GraphicsDevice::GetFrameIndex() * constantBytes + constantBytes - 256;
        commands->SetGraphicsRootConstantBufferView(5, zeroAddress);
        auto* optics =
            reinterpret_cast<XMFLOAT4*>(preview.MappedOpticsGlobals + GraphicsDevice::GetFrameIndex() * 2816);
        memset(optics, 0, 2816);
        if (preview.SrvIndex == RendererState::g_kMATERIAL_PREVIEW_SRV_INDEX)
        {
            optics[1] = {preview.SceneEnvironment && material.Transmission <= 0.0f ? 1.0f : 0.0f,
                         preview.LightIntensity, static_cast<float>(preview.Shape + 1), 1.0f};
        }
        commands->SetGraphicsRootConstantBufferView(17, preview.OpticsGlobals->GetGPUVirtualAddress() +
                                                            GraphicsDevice::GetFrameIndex() * 2816);
        commands->SetGraphicsRootConstantBufferView(26, zeroAddress);
        commands->SetGraphicsRootConstantBufferView(27, zeroAddress);
        const XMFLOAT3 eye = {sinf(preview.Yaw) * cosf(preview.Pitch) * preview.Distance + preview.Center.x,
                              sinf(preview.Pitch) * preview.Distance + preview.Center.y,
                              -cosf(preview.Yaw) * cosf(preview.Pitch) * preview.Distance + preview.Center.z};
        ConstantBuffer3D constants;
        constants.World = XMMatrixIdentity();
        constants.View = XMMatrixTranspose(
            XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&preview.Center), XMVectorSet(0, 1, 0, 0)));
        constants.Projection = XMMatrixTranspose(SceneDepth::InfiniteReverseZPerspectiveLH(XM_PIDIV4, 1, preview.Near));
        constants.CameraPos = eye;
        constants.UseTexture = material.UseTexture;
        constants.UseNormalMap = material.NormalMapID >= 0;
        constants.MaterialMode = 1;
        constants.ShaderClass = static_cast<int>(ShaderClass::PBR);
        constants.MaterialMetallic = material.Metallic;
        constants.MaterialRoughness = material.Roughness;
        constants.MaterialFresnel = material.Fresnel;
        constants.MaterialAlpha = material.Alpha;
        constants.ViewportSize = {static_cast<float>(preview.Size), static_cast<float>(preview.Size)};
        constants.MaterialIsTransparent = material.IsTransparent;
        const auto drawConstants = FrameConstants::AllocateTransientConstantBuffer(constants);
        if (!drawConstants.ptr)
        {
            preview.Error = "Preview frame constant budget exhausted";
            const auto restore = CD3DX12_RESOURCE_BARRIER::Transition(
                preview.Target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commands->ResourceBarrier(1, &restore);
            return false;
        }
        commands->SetGraphicsRootDescriptorTable(0, drawConstants);
        if (preview.PreviewConstants)
        {
            if (!MaterialBindings::SetPreviewMaterial(material, preview.PreviewConstants.Get(), preview.TextureBase,
                                                      partIndex, partCount))
            {
                const auto restore =
                    CD3DX12_RESOURCE_BARRIER::Transition(preview.Target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                commands->ResourceBarrier(1, &restore);
                return false;
            }
        }
        else
        {
            MaterialBindings::SetMaterial(g_kINVALID_ENTITY, material);
        }
        const auto heap = FrameConstants::GetCbvHeap()->GetGPUDescriptorHandleForHeapStart();
        const auto increment = FrameConstants::GetCbvIncrementSize();
        int albedo = TextureManager::GetDefaultTextureIndex();
        int normal = albedo;
        if (material.TextureID >= 0)
        {
            albedo = material.TextureID;
        }
        if (material.NormalMapID >= 0)
        {
            normal = material.NormalMapID;
        }
        commands->SetGraphicsRootDescriptorTable(1, CD3DX12_GPU_DESCRIPTOR_HANDLE(heap, albedo, increment));
        commands->SetGraphicsRootDescriptorTable(6, CD3DX12_GPU_DESCRIPTOR_HANDLE(heap, normal, increment));
        if (!preview.SceneEnvironment || material.Transmission > 0.0f)
        {
            commands->SetGraphicsRootDescriptorTable(
                7, CD3DX12_GPU_DESCRIPTOR_HANDLE(heap, TextureManager::GetDefaultTextureIndex(), increment));
        }
        if (mesh)
        {
            commands->IASetVertexBuffers(0, 1, &mesh->VertexBufferView);
            commands->IASetIndexBuffer(&mesh->IndexBufferView);
            commands->DrawIndexedInstanced(mesh->IndexCount, 1, 0, 0, 0);
        }
        else
        {
            commands->IASetVertexBuffers(0, 1, &preview.Views[preview.Shape]);
            commands->IASetIndexBuffer(nullptr);
            commands->DrawInstanced(vertexCount ? vertexCount : preview.Counts[preview.Shape], 1, firstVertex, 0);
        }
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(preview.Target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (partIndex + 1 == partCount)
        {
            commands->ResourceBarrier(1, &barrier);
        }
        return true;
    }

} // 名前空間

void MaterialPreview::Render()
{
    if (!preview.Visible || !preview.Material || !preview.Initialize())
    {
        return;
    }
    auto& runtime = MaterialManager::GetRuntime(preview.Material);
    for (int texture : runtime.TextureDescriptors)
    {
        if (texture >= 0)
        {
            TextureManager::TouchTexture(texture);
        }
    }
    RenderPreview(preview, runtime.Binding);
}

void MaterialPreview::Shutdown()
{
    preview = PreviewState{};
    std::lock_guard lock(thumbnailPipelineMutex);
    thumbnailPipelines = {};
    thumbnailPipelineFormat = DXGI_FORMAT_UNKNOWN;
}

ID3D12Resource* MaterialPreview::GetTarget()
{
    return preview.Target.Get();
}

void MaterialPreview::SetMesh(Mesh mesh)
{
    preview.Shape = static_cast<int>(mesh);
}

void MaterialPreview::SetLightIntensity(float intensity)
{
    preview.LightIntensity = clamp(intensity, 0.0f, 5.0f);
}

struct ThumbnailPreviewContext::Impl
{
    PreviewState State;
    bool GeometryUploaded = false;
    UINT MaterialSlots = 1;
};

struct AnimationPreviewContext::Impl
{
    PreviewState State;
    UINT MaterialCount = 1;
    float Radius = 1.0f;
};

AnimationPreviewContext::AnimationPreviewContext() : m_Impl(make_unique<Impl>()) {}

AnimationPreviewContext::~AnimationPreviewContext() = default;

bool AnimationPreviewContext::Initialize(UINT size, UINT descriptorIndex, UINT materialCount)
{
    auto& state = m_Impl->State;
    state.Size = size;
    state.SrvIndex = descriptorIndex;
    state.TextureBase = FrameConstants::g_kTEXTURE_SRV_START_INDEX;
    if (!state.Initialize())
    {
        return false;
    }
    m_Impl->MaterialCount = max(1u, materialCount);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto description =
        CD3DX12_RESOURCE_DESC::Buffer(RendererState::g_kPBR_CB_ALIGNED_SIZE * RendererState::g_kFRAME_COUNT *
                                      static_cast<UINT64>(m_Impl->MaterialCount));
    if (FAILED(GraphicsDevice::GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                    IID_PPV_ARGS(&state.PreviewConstants))))
    {
        state.Error = "アニメーションプレビューのマテリアルバッファを作成できません";
        return false;
    }
    return true;
}

void AnimationPreviewContext::FrameModel(const XMFLOAT3& center, const XMFLOAT3& extents)
{
    auto& state = m_Impl->State;
    state.Center = center;
    m_Impl->Radius = max(0.01f, sqrtf(extents.x * extents.x + extents.y * extents.y + extents.z * extents.z));
    const auto fit = FitThumbnailCamera(m_Impl->Radius);
    state.Distance = fit.Distance;
    state.Near = fit.Near;
    state.Far = fit.Far;
    state.Yaw = 0.0f;
    state.Pitch = 0.12f;
}

void AnimationPreviewContext::Orbit(float yaw, float pitch, float zoom)
{
    auto& state = m_Impl->State;
    state.Yaw += yaw;
    state.Pitch = clamp(state.Pitch + pitch, -1.4f, 1.4f);
    state.Distance = clamp(state.Distance * expf(-zoom * 0.15f), m_Impl->Radius * 0.25f, m_Impl->Radius * 30.0f);
}

const string& AnimationPreviewContext::Error() const
{
    return m_Impl->State.Error;
}

bool AnimationPreviewContext::Render(AnimationModelResource& model, const vector<MaterialComponent>& materials)
{
    auto& state = m_Impl->State;
    if (!state.Target || materials.size() != model.GetMeshCount() || materials.size() > m_Impl->MaterialCount)
    {
        return false;
    }
    vector<UINT> meshes;
    for (UINT index = 0; index < model.GetMeshCount(); ++index)
    {
        const auto& mesh = model.GetMeshData(index);
        if (mesh.IndexCount == 0 || !mesh.VertexBuffer || !mesh.IndexBuffer)
        {
            continue;
        }
        if (!state.Pipeline(materials[index]))
        {
            return false;
        }
        meshes.push_back(index);
    }
    if (meshes.empty())
    {
        state.Error = "プレビューできるメッシュがありません";
        return false;
    }
    auto* commands = GraphicsDevice::GetCommandList();
    model.DispatchGpuSkinning(commands);
    for (UINT part = 0; part < meshes.size(); ++part)
    {
        const UINT index = meshes[part];
        const auto& mesh = model.GetMeshData(index);
        const auto& material = materials[index];
        for (int texture : {material.TextureID, material.NormalMapID, material.MetallicRoughnessID,
                            material.AOTextureID, material.EmissiveTextureID, material.HeightTextureID})
        {
            if (texture >= 0)
            {
                TextureManager::TouchTexture(texture);
            }
        }
        auto transition =
            CD3DX12_RESOURCE_BARRIER::Transition(mesh.VertexBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        commands->ResourceBarrier(1, &transition);
        const bool rendered = RenderPreview(state, material, 0, 0, part, static_cast<UINT>(meshes.size()), &mesh);
        transition = CD3DX12_RESOURCE_BARRIER::Transition(mesh.VertexBuffer.Get(),
                                                          D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->ResourceBarrier(1, &transition);
        if (!rendered)
        {
            return false;
        }
    }
    return true;
}

ThumbnailPreviewContext::ThumbnailPreviewContext() : m_Impl(std::make_unique<Impl>()) {}

ThumbnailPreviewContext::~ThumbnailPreviewContext() = default;

bool ThumbnailPreviewContext::Initialize(UINT size, UINT descriptorIndex)
{
    auto& state = m_Impl->State;
    state.Size = size;
    state.SrvIndex = descriptorIndex;
    state.SceneEnvironment = false;
    const auto sphereFit = FitThumbnailCamera(1.0f);
    state.Distance = sphereFit.Distance;
    state.Near = sphereFit.Near;
    state.Far = sphereFit.Far;
    if (!state.Initialize())
    {
        return false;
    }
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto description =
        CD3DX12_RESOURCE_DESC::Buffer(RendererState::g_kPBR_CB_ALIGNED_SIZE * RendererState::g_kFRAME_COUNT);
    return SUCCEEDED(GraphicsDevice::GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                          IID_PPV_ARGS(&state.PreviewConstants)));
}

bool ThumbnailPreviewContext::PreparePipelines(const MaterialComponent& material,
                                               const std::vector<ThumbnailPreviewPart>& parts)
{
    auto& state = m_Impl->State;
    if (parts.empty())
    {
        return state.Pipeline(material) != nullptr;
    }
    for (const auto& part : parts)
    {
        if (!state.Pipeline(part.Material))
        {
            return false;
        }
    }
    return true;
}

bool ThumbnailPreviewContext::Render(const MaterialComponent& material, const std::vector<StaticModelVertex>& geometry,
                                     const std::vector<ThumbnailPreviewPart>& parts)
{
    auto& state = m_Impl->State;
    if (!geometry.empty() && !m_Impl->GeometryUploaded)
    {
        XMFLOAT3 low = geometry.front().Position;
        XMFLOAT3 high = low;
        for (const auto& vertex : geometry)
        {
            const auto p = vertex.Position;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            {
                return false;
            }
            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
        state.Center = {(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f};
        const float x = (high.x - low.x) * 0.5f, y = (high.y - low.y) * 0.5f, z = (high.z - low.z) * 0.5f;
        const auto fit = FitThumbnailCamera(std::sqrt(x * x + y * y + z * z));
        if (!fit.Valid || geometry.size() > UINT_MAX / sizeof(StaticModelVertex))
        {
            return false;
        }
        state.Distance = fit.Distance;
        state.Near = fit.Near;
        state.Far = fit.Far;
        const UINT bytes = static_cast<UINT>(geometry.size() * sizeof(StaticModelVertex));
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        state.Vertices[0].Reset();
        if (FAILED(GraphicsDevice::GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&state.Vertices[0]))))
        {
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(state.Vertices[0]->Map(0, nullptr, &mapped)))
        {
            return false;
        }
        memcpy(mapped, geometry.data(), bytes);
        state.Vertices[0]->Unmap(0, nullptr);
        state.Views[0] = {state.Vertices[0]->GetGPUVirtualAddress(), bytes, sizeof(StaticModelVertex)};
        state.Counts[0] = static_cast<UINT>(geometry.size());
        m_Impl->GeometryUploaded = true;
    }
    if (parts.empty())
    {
        return RenderPreview(state, material);
    }
    const UINT count = static_cast<UINT>(parts.size());
    if (m_Impl->MaterialSlots != count)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(
            RendererState::g_kPBR_CB_ALIGNED_SIZE * RendererState::g_kFRAME_COUNT * static_cast<UINT64>(count));
        state.PreviewConstants.Reset();
        if (FAILED(GraphicsDevice::GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&state.PreviewConstants))))
        {
            return false;
        }
        m_Impl->MaterialSlots = count;
    }
    // パス記録前にパイプラインを解決し、マテリアル失敗時にターゲットが描画途中で残るのを防ぐ。
    for (const auto& part : parts)
    {
        if (!state.Pipeline(part.Material))
        {
            return false;
        }
    }
    for (UINT index = 0; index < count; ++index)
    {
        const auto& part = parts[index];
        if (!RenderPreview(state, part.Material, part.FirstVertex, part.VertexCount, index, count))
        {
            return false;
        }
    }
    return true;
}

ID3D12Resource* ThumbnailPreviewContext::GetTarget() const
{
    return m_Impl->State.Target.Get();
}

uint64_t ThumbnailPreviewContext::MemoryBytes() const
{
    const auto& state = m_Impl->State;
    uint64_t bytes = 0;
    const auto add = [&bytes](const ComPtr<ID3D12Resource>& resource) {
        if (!resource)
        {
            return;
        }
        const auto description = resource->GetDesc();
        bytes += GraphicsDevice::GetDevice()->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    };
    add(state.Target);
    add(state.Depth);
    add(state.Lights);
    add(state.OpticsGlobals);
    add(state.PreviewConstants);
    for (const auto& buffer : state.Vertices)
    {
        add(buffer);
    }
    return bytes;
}
