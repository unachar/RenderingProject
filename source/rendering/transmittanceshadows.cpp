#include "pch.h"
#include "transmittanceshadows.h"
#include "graphicsdevice.h"
#include "lightingresources.h"
#include "frameconstants.h"
#include "shaderpipelines.h"
#include "materialmanager.h"
#include "materialsystem.h"
#include "componentmanager.h"
#include "world.h"

namespace
{
ComPtr<ID3D12Resource> g_Color, g_Depth, g_Constants;
ComPtr<ID3D12DescriptorHeap> g_Rtv, g_Dsv;
ComPtr<ID3D12PipelineState> g_Pso;
UINT8* g_Mapped = nullptr;
bool g_HasCasters = false, g_Active = false;
UINT g_Slice = 0, g_PreviousDescriptor = 0, g_ConstantOffset = 0;
struct ShadowTransmissionConstants
{
    XMMATRIX ViewProjection;
    XMFLOAT4 MapParams, FilterParams, PeelParams, LightPositionType, LightDirection;
};
static_assert(sizeof(ShadowTransmissionConstants) <= 256);
}

void TransmittanceShadows::PrepareFrame()
{
    g_HasCasters = false;
    for (EntityID entity : World::GetView<MaterialComponent>())
    {
        if (ComponentManager::HasComponent<LightComponent>(entity)) continue;
        if (ComponentManager::HasComponent<MeshComponent>(entity) &&
            ComponentManager::GetComponentUnchecked<MeshComponent>(entity).IsEnvironment) continue;
        if (ComponentManager::HasComponent<SpriteComponent>(entity) &&
            !ComponentManager::GetComponentUnchecked<SpriteComponent>(entity).Is3D) continue;
        if (!ComponentManager::HasComponent<TransformComponent>(entity) ||
            !(ComponentManager::HasComponent<MeshComponent>(entity) ||
              ComponentManager::HasComponent<SpriteComponent>(entity) ||
              ComponentManager::HasComponent<StaticModelComponent>(entity) ||
              ComponentManager::HasComponent<AnimationModelComponent>(entity))) continue;
        const auto& source = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        const UINT count = static_cast<UINT>(source.MaterialSlots.size());
        for (UINT slot = 0; slot <= count; ++slot)
        {
            const auto& material = slot == count ? MaterialManager::Resolve(source) : MaterialManager::Resolve(source, slot);
            if (MaterialSystem::CastsShadowInPass(material, true)) { g_HasCasters = true; break; }
        }
        if (g_HasCasters) break;
    }
    if (g_HasCasters && !CreateResources())
    {
        g_HasCasters = false;
        Debug::Log("ERROR: Transmittance shadow resources unavailable\n");
    }
}

bool TransmittanceShadows::HasCasters() { return g_HasCasters; }
bool TransmittanceShadows::IsActive() { return g_Active; }
ID3D12PipelineState* TransmittanceShadows::GetPipelineState() { return g_Pso.Get(); }
ID3D12Resource* TransmittanceShadows::GetColorResource() { return g_Color.Get(); }
D3D12_GPU_VIRTUAL_ADDRESS TransmittanceShadows::GetCurrentConstantBufferAddress()
{
    return g_Constants ? g_Constants->GetGPUVirtualAddress() + g_ConstantOffset : 0;
}

bool TransmittanceShadows::CreateResources()
{
    if (g_Pso) return true;
    auto* device = m_Device.Get();
    constexpr UINT slices = g_kMAX_SHADOW_LIGHTS * g_kTRANSMITTANCE_SHADOW_LAYERS;
    auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto colorDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
        g_kTRANSMITTANCE_SHADOW_SIZE, g_kTRANSMITTANCE_SHADOW_SIZE, slices, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE colorClear{DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 1, 1, 1}};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &colorDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &colorClear, IID_PPV_ARGS(&g_Color)))) return false;
    auto depthDesc = colorDesc;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &depthClear, IID_PPV_ARGS(&g_Depth)))) return false;
    g_Color->SetName(L"Peeled RGB shadow transmission");
    g_Depth->SetName(L"Peeled transparent shadow depth");
    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.NumDescriptors = slices;
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&g_Rtv)))) return false;
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&g_Dsv)))) return false;
    const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    auto srvHandle = [&](UINT index) { return CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), index, m_CbvIncrementSize); };
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.ArraySize = slices;
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(g_Color.Get(), &srv, srvHandle(g_kTRANSMITTANCE_SHADOW_SRV_START));
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView(g_Depth.Get(), &srv, srvHandle(g_kTRANSMITTANCE_SHADOW_SRV_START + 1));
    for (UINT slice = 0; slice < slices; ++slice)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv.Texture2DArray.FirstArraySlice = slice;
        rtv.Texture2DArray.ArraySize = 1;
        device->CreateRenderTargetView(g_Color.Get(), &rtv, CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Rtv->GetCPUDescriptorHandleForHeapStart(), slice, rtvSize));
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = slice;
        dsv.Texture2DArray.ArraySize = 1;
        device->CreateDepthStencilView(g_Depth.Get(), &dsv, CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Dsv->GetCPUDescriptorHandleForHeapStart(), slice, dsvSize));
        srv.Texture2DArray.ArraySize = 1;
        srv.Texture2DArray.FirstArraySlice = slice;
        device->CreateShaderResourceView(g_Depth.Get(), &srv, srvHandle(g_kTRANSMITTANCE_SHADOW_SRV_START + 2 + slice));
    }
    heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(g_kFRAME_COUNT * g_kMAX_SHADOW_PASSES * g_kTRANSMITTANCE_SHADOW_LAYERS * 256);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_Constants)))) return false;
    CD3DX12_RANGE readRange(0, 0);
    if (FAILED(g_Constants->Map(0, &readRange, reinterpret_cast<void**>(&g_Mapped)))) return false;
    ComPtr<ID3DBlob> vs, ps;
    ShaderDescription shader{};
    shader.csoPath = "shader/hlsl/build/ShadowMapVS.cso";
    shader.ppBlob = vs.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(shader)) return false;
    shader.csoPath = "shader/hlsl/build/ShadowTransmissionPS.cso";
    shader.ppBlob = ps.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(shader)) return false;
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = ShaderPipelines::GetModelRootSignature();
    pso.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
    pso.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
    pso.InputLayout = {layout, _countof(layout)};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    return SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_Pso)));
}

bool TransmittanceShadows::BeginPass(UINT shadowIndex, UINT peel)
{
    if (!g_HasCasters || peel >= g_kTRANSMITTANCE_SHADOW_LAYERS) return false;
    UINT lightLayer;
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    bool clearLayer;
    if (!LightingResources::GetShadowPassInfo(shadowIndex, lightLayer, viewport, scissor, clearLayer)) return false;
    LightingResources::SetCurrentShadowPassIndex(shadowIndex);
    g_Slice = lightLayer * g_kTRANSMITTANCE_SHADOW_LAYERS + peel;
    g_PreviousDescriptor = peel == 0 ? g_kSHADOW_SRV_INDEX : g_kTRANSMITTANCE_SHADOW_SRV_START + 2 + g_Slice - 1;
    g_ConstantOffset = ((m_FrameIndex * g_kMAX_SHADOW_PASSES + shadowIndex) * g_kTRANSMITTANCE_SHADOW_LAYERS + peel) * 256;
    ShadowTransmissionConstants constants{};
    constants.ViewProjection = XMMatrixTranspose(LightingResources::GetCurrentShadowViewProjection());
    constants.PeelParams = {static_cast<float>(peel), 1.0f / g_kTRANSMITTANCE_SHADOW_SIZE, 0.000001f, static_cast<float>(lightLayer)};
    LightingResources::GetCurrentShadowLight(constants.LightPositionType, constants.LightDirection);
    memcpy(g_Mapped + g_ConstantOffset, &constants, sizeof(constants));
    const float scale = static_cast<float>(g_kTRANSMITTANCE_SHADOW_SIZE) / g_kSHADOW_MAP_SIZE;
    viewport.TopLeftX *= scale; viewport.TopLeftY *= scale;
    viewport.Width *= scale; viewport.Height *= scale;
    scissor.left = static_cast<LONG>(scissor.left * scale); scissor.top = static_cast<LONG>(scissor.top * scale);
    scissor.right = static_cast<LONG>(scissor.right * scale); scissor.bottom = static_cast<LONG>(scissor.bottom * scale);
    D3D12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(g_Color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, g_Slice),
        CD3DX12_RESOURCE_BARRIER::Transition(g_Depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE, g_Slice)};
    m_CommandList->ResourceBarrier(_countof(barriers), barriers);
    const auto rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Rtv->GetCPUDescriptorHandleForHeapStart(), g_Slice,
        m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    const auto dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(g_Dsv->GetCPUDescriptorHandleForHeapStart(), g_Slice,
        m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV));
    const float white[4] = {1, 1, 1, 1};
    const UINT rectCount = clearLayer ? 0 : 1;
    m_CommandList->ClearRenderTargetView(rtv, white, rectCount, clearLayer ? nullptr : &scissor);
    m_CommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, rectCount, clearLayer ? nullptr : &scissor);
    m_CommandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    m_CommandList->RSSetViewports(1, &viewport);
    m_CommandList->RSSetScissorRects(1, &scissor);
    m_CommandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
    m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_Active = true;
    BindCurrentPass(m_CommandList.Get());
    return true;
}

void TransmittanceShadows::BindCurrentPass(ID3D12GraphicsCommandList* commands)
{
    if (!g_Active) return;
    commands->SetGraphicsRootConstantBufferView(5, GetCurrentConstantBufferAddress());
    commands->SetGraphicsRootDescriptorTable(33, CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), g_PreviousDescriptor, m_CbvIncrementSize));
}

void TransmittanceShadows::EndPass()
{
    D3D12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(g_Color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, g_Slice),
        CD3DX12_RESOURCE_BARRIER::Transition(g_Depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, g_Slice)};
    m_CommandList->ResourceBarrier(_countof(barriers), barriers);
    g_Active = false;
}

void TransmittanceShadows::BindLighting(ID3D12GraphicsCommandList* commands, UINT colorRoot, UINT depthRoot)
{
    const UINT start = g_Color ? g_kTRANSMITTANCE_SHADOW_SRV_START : g_kSHADOW_SRV_INDEX;
    commands->SetGraphicsRootDescriptorTable(colorRoot, CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), start, m_CbvIncrementSize));
    commands->SetGraphicsRootDescriptorTable(depthRoot, CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), g_Depth ? start + 1 : start, m_CbvIncrementSize));
}

void TransmittanceShadows::Shutdown()
{
    if (g_Mapped && g_Constants) g_Constants->Unmap(0, nullptr);
    g_Mapped = nullptr;
    g_Constants.Reset(); g_Color.Reset(); g_Depth.Reset(); g_Rtv.Reset(); g_Dsv.Reset(); g_Pso.Reset();
    g_Active = false; g_HasCasters = false;
}
