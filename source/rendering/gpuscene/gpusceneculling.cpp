#include "pch.h"
#include "gpusceneculling.h"
#include "shaderpipelines.h"
#include "componentmanager.h"
#include "camera.h"
#include "renderprofiler.h"
#include "visibility/gpuvisibility.h"

void GpuSceneCulling::Prepare(ID3D12Device* device, uint32_t slots, uint32_t frame)
{
    Visibility.BeginFrame(frame);
    Visibility.Ensure(device, slots, sizeof(uint32_t), frame, L"GPU Scene Visibility", true);
    if (m_Pipeline)
    {
        return;
    }
    CD3DX12_ROOT_PARAMETER parameters[5];
    parameters[0].InitAsShaderResourceView(0);
    parameters[1].InitAsShaderResourceView(1);
    parameters[2].InitAsShaderResourceView(2);
    parameters[3].InitAsUnorderedAccessView(0);
    parameters[4].InitAsConstants(20, 0);
    CD3DX12_ROOT_SIGNATURE_DESC description;
    description.Init(_countof(parameters), parameters);
    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        throw runtime_error("GPU scene culling root signature failed");
    }
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&m_Root))))
    {
        throw runtime_error("GPU scene culling root creation failed");
    }
    ComPtr<ID3DBlob> shader;
    ShaderDescription shaderDescription{};
    shaderDescription.csoPath = "shader/hlsl/build/GpuSceneCullCS.cso";
    shaderDescription.ppBlob = shader.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(shaderDescription))
    {
        throw runtime_error("GPU scene culling shader is missing");
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = m_Root.Get();
    pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_Pipeline))))
    {
        throw runtime_error("GPU scene culling pipeline failed");
    }
}

void GpuSceneCulling::ExecuteCulling(ID3D12GraphicsCommandList* commands, ID3D12Resource* instances,
                                     ID3D12Resource* meshes, ID3D12Resource* materials, uint32_t slots,
                                     uint32_t meshCount, uint32_t materialCount)
{
    if (!slots)
    {
        return;
    }

    struct Constants
    {
        XMFLOAT4X4 ViewProjection;
        uint32_t Slots, Meshes, Materials, EnableFrustum;
    } constants{};

    static_assert(sizeof(constants) == 80);
    XMStoreFloat4x4(&constants.ViewProjection, XMMatrixIdentity());
    const EntityID camera = Camera::GetCameraEntity();
    if (camera != g_kINVALID_ENTITY && ComponentManager::HasComponent<CameraComponent>(camera))
    {
        const auto& data = ComponentManager::GetComponentUnchecked<CameraComponent>(camera);
        XMStoreFloat4x4(&constants.ViewProjection,
                        XMLoadFloat4x4(&data.ViewMatrix) * XMLoadFloat4x4(&data.ProjectionMatrix));
        if (GPUVisibility::GetSettings().FreezeCamera)
        {
            constants.ViewProjection = GPUVisibility::CullingViewProjection();
        }
        constants.EnableFrustum = GPUVisibility::GetSettings().Frustum ? 1 : 0;
    }
    constants.Slots = slots;
    constants.Meshes = meshCount;
    constants.Materials = materialCount;
    RenderProfiler::ScopedEvent profile("GPU Scene Culling", commands);
    commands->SetComputeRootSignature(m_Root.Get());
    commands->SetPipelineState(m_Pipeline.Get());
    commands->SetComputeRootShaderResourceView(0, instances->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(1, meshes->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(2, materials->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(3, Visibility.Resource()->GetGPUVirtualAddress());
    commands->SetComputeRoot32BitConstants(4, 20, &constants, 0);
    commands->Dispatch((slots + 63) / 64, 1, 1);
}
