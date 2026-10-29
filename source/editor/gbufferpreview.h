#pragma once

#include "gbufferpreviewshader.h"
#include "imguishadercompiler.h"

// An ImGui draw callback keeps data textures out of the regular alpha-blended image shader.
namespace GBufferPreview
{
    inline ComPtr<ID3D12RootSignature> RootSignature;
    inline ComPtr<ID3D12PipelineState> Pipeline;
    inline D3D12_CPU_DESCRIPTOR_HANDLE BloomCpu{};
    inline D3D12_GPU_DESCRIPTOR_HANDLE BloomGpu{};
    inline ComPtr<ID3D12Resource> BloomResource;

    inline void Shutdown()
    {
        Pipeline.Reset();
        RootSignature.Reset();
        BloomCpu = {};
        BloomGpu = {};
        BloomResource.Reset();
    }

    inline bool Initialize(ID3D12Device* device, DXGI_FORMAT targetFormat, DXGI_FORMAT depthFormat)
    {
        CD3DX12_DESCRIPTOR_RANGE textureRange;
        textureRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_ROOT_PARAMETER parameters[3];
        parameters[0].InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
        parameters[1].InitAsDescriptorTable(1, &textureRange, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[2].InitAsConstants(1, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_ROOT_SIGNATURE_DESC rootDesc;
        rootDesc.Init(3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> serialized, errors;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
            FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&RootSignature))))
        {
            return false;
        }
        ComPtr<ID3DBlob> vertex, pixel;
        if (FAILED(ImGuiShaderCompiler::Compile(GBufferPreviewShader::Vertex, L"vs_6_0", &vertex)) ||
            FAILED(ImGuiShaderCompiler::Compile(GBufferPreviewShader::Pixel, L"ps_6_0", &pixel)))
        {
            Shutdown();
            return false;
        }
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(ImDrawVert, pos)),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(ImDrawVert, uv)),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, UINT(offsetof(ImDrawVert, col)),
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = RootSignature.Get();
        desc.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
        desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        desc.InputLayout = {layout, UINT(std::size(layout))};
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = false;
        desc.DepthStencilState.StencilEnable = false;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = targetFormat;
        desc.DSVFormat = depthFormat;
        desc.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&Pipeline))))
        {
            Shutdown();
            return false;
        }
        return true;
    }

    inline void SetRenderState(const ImDrawList*, const ImDrawCmd* command)
    {
        auto* state = static_cast<ImGui_ImplDX12_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
        auto* commands = state->CommandList;
        const ImDrawData* drawData = ImGui::GetDrawData();
        const float left = drawData->DisplayPos.x;
        const float right = left + drawData->DisplaySize.x;
        const float top = drawData->DisplayPos.y;
        const float bottom = top + drawData->DisplaySize.y;
        const float projection[4][4] = {
            {2.0f / (right - left), 0, 0, 0},
            {0, 2.0f / (top - bottom), 0, 0},
            {0, 0, 0.5f, 0},
            {(right + left) / (left - right), (top + bottom) / (bottom - top), 0.5f, 1},
        };
        commands->SetPipelineState(Pipeline.Get());
        commands->SetGraphicsRootSignature(RootSignature.Get());
        commands->SetGraphicsRoot32BitConstants(0, 16, projection, 0);
        commands->SetGraphicsRoot32BitConstants(2, 1, command->UserCallbackData, 0);
    }

    inline void Image(D3D12_GPU_DESCRIPTOR_HANDLE texture, ImVec2 size, UINT type)
    {
        if (!Pipeline || !texture.ptr)
        {
            ImGui::TextUnformatted("プレビューを利用できません");
            return;
        }
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddCallback(SetRenderState, &type, sizeof(type));
        ImGui::Image(ImTextureID(texture.ptr), size);
        drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    inline D3D12_GPU_DESCRIPTOR_HANDLE BloomTexture(ID3D12Device* device, ID3D12Resource* resource,
                                                    D3D12_GPU_DESCRIPTOR_HANDLE fallback)
    {
        if (!resource || !BloomCpu.ptr)
        {
            return fallback;
        }
        // Bloom textures are recreated with the scene targets after the GPU has been drained.
        if (resource != BloomResource.Get())
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = resource->GetDesc().Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resource, &view, BloomCpu);
            BloomResource = resource;
        }
        return BloomGpu;
    }
}
