#include "pch.h"
#include "persistentpsocache.h"
#include "transparencyeffects.h"
#include "psomanager.h"
#include "shaderpipelines.h"
#include "graphicsdevice.h"
#include "shaderpaths.h"
#include "texturemanager.h"
#include "renderersettings.h"
#include "visibilitybuffer.h"
#include "shaderdescription.h"

bool PsoManager::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const char* debugName,
                                             ComPtr<ID3D12PipelineState>& outPso)
{
    outPso.Reset();
    HRESULT hr = PersistentPsoCache::Graphics(m_Device.Get(), desc, outPso);
    if (FAILED(hr))
    {
        HRESULT removedReason;
        if (m_Device)
        {
            removedReason = m_Device->GetDeviceRemovedReason();
        }
        else
        {
            removedReason = E_POINTER;
        }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || FAILED(removedReason))
        {
            GraphicsDevice::CheckDeviceHealth(hr, "Create graphics pipeline state");
            return false;
        }
        const char* pipelineName;
        if (debugName)
        {
            pipelineName = debugName;
        }
        else
        {
            pipelineName = "unnamed";
        }
        Debug::Log("ERROR: Failed to create %s graphics PSO. HRESULT: 0x%08X\n", pipelineName, hr);
        FILE* log = nullptr;
        fopen_s(&log, "init_log.txt", "a");
        if (log)
        {
            const char* pipelineNameValue;
            if (debugName)
            {
                pipelineNameValue = debugName;
            }
            else
            {
                pipelineNameValue = "unnamed";
            }
            fprintf(log, "PSO ERROR: %s HRESULT=0x%08X\n", pipelineNameValue, hr);
            ComPtr<ID3D12InfoQueue> infoQueue;
            if (SUCCEEDED(m_Device.As(&infoQueue)))
            {
                const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
                UINT64 firstMessage;
                if (messageCount > 8)
                {
                    firstMessage = messageCount - 8;
                }
                else
                {
                    firstMessage = 0;
                }
                for (UINT64 index = firstMessage; index < messageCount; ++index)
                {
                    SIZE_T messageSize = 0;
                    infoQueue->GetMessage(index, nullptr, &messageSize);
                    vector<uint8_t> storage(messageSize);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if (SUCCEEDED(infoQueue->GetMessage(index, message, &messageSize)))
                    {
                        fprintf(log, "D3D12: %s\n", message->pDescription);
                    }
                }
            }
            fclose(log);
        }

        return false;
    }
    ++s_GraphicsPipelineCreations;
    return true;
}

bool PsoManager::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, const char* debugName,
                                            ComPtr<ID3D12PipelineState>& outPso)
{
    outPso.Reset();
    HRESULT hr = PersistentPsoCache::Compute(m_Device.Get(), desc, outPso);
    if (FAILED(hr))
    {
        HRESULT removedReason;
        if (m_Device)
        {
            removedReason = m_Device->GetDeviceRemovedReason();
        }
        else
        {
            removedReason = E_POINTER;
        }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || FAILED(removedReason))
        {
            GraphicsDevice::CheckDeviceHealth(hr, "Create compute pipeline state");
            return false;
        }
        const char* pipelineName3;
        if (debugName)
        {
            pipelineName3 = debugName;
        }
        else
        {
            pipelineName3 = "unnamed";
        }
        Debug::Log("ERROR: Failed to create %s compute PSO. HRESULT: 0x%08X\n", pipelineName3, hr);
        return false;
    }
    return true;
}

namespace
{
// 負荷削減：描画ごとの PSO 検索で毎回文字列キーを組み立てないための前段キャッシュ
// （入力と、結果を左右する状態だけからハッシュを作る。m_PsoCache が空になったら捨てる）
struct FastPsoKey
{
    uint64_t Vs = 0, Ps = 0;
    uint32_t Flags = 0, Format = 0;

    bool operator==(const FastPsoKey& other) const
    {
        return Vs == other.Vs && Ps == other.Ps && Flags == other.Flags && Format == other.Format;
    }
};

struct FastPsoKeyHash
{
    size_t operator()(const FastPsoKey& key) const noexcept
    {
        return static_cast<size_t>(key.Vs * 0x9e3779b97f4a7c15ull ^ (key.Ps + 0x632be59bd9b4e019ull) ^
                                   (static_cast<uint64_t>(key.Flags) << 32) ^ key.Format);
    }
};

uint64_t HashPath(const char* path)
{
    uint64_t hash = 1469598103934665603ull;
    if (path)
    {
        for (const char* c = path; *c; ++c)
        {
            hash ^= static_cast<unsigned char>(*c);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

unordered_map<FastPsoKey, ID3D12PipelineState*, FastPsoKeyHash>& FastPsoCache()
{
    static unordered_map<FastPsoKey, ID3D12PipelineState*, FastPsoKeyHash> cache;
    return cache;
}
} // namespace

ID3D12PipelineState* PsoManager::GetOrCreateGraphicsPso(const ShaderDescription& resource)
{
    const bool requestDeferredScene = (m_RenderMode == RenderMode::DEFERRED && m_IsDeferredGeometryPass);
    const bool visibilityPass =
        requestDeferredScene && RendererSettings::GetComputeGBufferEnabled() && VisibilityBuffer::IsAvailable();
    auto& fastCache = FastPsoCache();
    if (m_PsoCache.empty())
    {
        fastCache.clear();
    }
    FastPsoKey fastKey;
    fastKey.Vs = HashPath(resource.vsPath);
    fastKey.Ps = HashPath(resource.psPath);
    fastKey.Flags = (resource.isModel ? 1u : 0u) | (resource.frontCounterClockwise ? 2u : 0u) |
                    (resource.enableAlphaBlend ? 4u : 0u) | (resource.depthWrite ? 8u : 0u) |
                    (requestDeferredScene ? 16u : 0u) | (visibilityPass ? 32u : 0u) |
                    (TransparencyEffects::IsOitPassActive() ? 64u : 0u) | (static_cast<uint32_t>(m_RenderMode) << 8) |
                    (static_cast<uint32_t>(resource.cullMode) << 16);
    fastKey.Format = static_cast<uint32_t>(GetForwardRtvFormat());
    if (const auto fast = fastCache.find(fastKey); fast != fastCache.end())
    {
        return fast->second;
    }
    ID3D12PipelineState* created = GetOrCreateGraphicsPsoSlow(resource, requestDeferredScene, visibilityPass);
    if (created)
    {
        fastCache[fastKey] = created;
    }
    return created;
}

ID3D12PipelineState* PsoManager::GetOrCreateGraphicsPsoSlow(const ShaderDescription& resource,
                                                            bool requestDeferredScene, bool visibilityPass)
{
    string resolvedPsPath;
    if (visibilityPass)
    {
        const char* modelKind;
        if (resource.isModel)
        {
            modelKind = "shader/hlsl/build/VisibilityPS.cso";
        }
        else
        {
            modelKind = "shader/hlsl/build/Visibility2DPS.cso";
        }
        resolvedPsPath = (modelKind);
    }
    else
    {
        string pixelShaderPath;
        if (requestDeferredScene)
        {
            pixelShaderPath = ResolvePixelShaderPathForRenderMode(resource.psPath, RenderMode::DEFERRED);
        }
        else
        {
            pixelShaderPath = ResolvePixelShaderPathForRenderMode(resource.psPath, RenderMode::FORWARD);
        }
        resolvedPsPath = (pixelShaderPath);
    }
    const bool useDeferredMrt = requestDeferredScene || RendererPathEndsWith(resolvedPsPath, "_MRT.cso") ||
                                RendererPathEndsWith(resolvedPsPath, "GeometryPS.cso");
    const bool enableAlphaBlend = resource.enableAlphaBlend && !useDeferredMrt;
    DXGI_FORMAT forwardRtvFormat;
    if (useDeferredMrt)
    {
        forwardRtvFormat = DXGI_FORMAT_UNKNOWN;
    }
    else
    {
        forwardRtvFormat = GetForwardRtvFormat();
    }
    const UINT forwardRtvFormatVal = static_cast<UINT>(forwardRtvFormat);
    const char* geometryKind;
    if (resource.isModel)
    {
        geometryKind = "M";
    }
    else
    {
        geometryKind = "2D";
    }
    const char* passKind;
    if (visibilityPass)
    {
        passKind = "VISIBILITY";
    }
    else if (useDeferredMrt)
    {
        passKind = "DEFERRED";
    }
    else
    {
        passKind = "FORWARD";
    }
    const char* windingKind;
    if (resource.frontCounterClockwise)
    {
        windingKind = "FRONT_CCW";
    }
    else
    {
        windingKind = "FRONT_CW";
    }
    const char* blendKind;
    if (enableAlphaBlend)
    {
        blendKind = "ALPHA";
    }
    else
    {
        blendKind = "OPAQUE";
    }
    string key = resource.vsPath + resolvedPsPath + "|" + (geometryKind) + "|" + (passKind) + "|" +
                 to_string(forwardRtvFormatVal) + "|" + (windingKind) + "|" + (blendKind) + "|" +
                 to_string(resource.cullMode) + "|" + to_string(resource.depthWrite);
    // Weighted blended OIT: the transparent pass binds (accumulation, revealage) instead of the scene.
    const bool oitPass = TransparencyEffects::IsOitPassActive() && !useDeferredMrt && !visibilityPass;
    if (oitPass)
    {
        key += "|OIT";
    }

    auto it = m_PsoCache.find(key);
    if (it != m_PsoCache.end())
    {
        return it->second.Get();
    }

    const char* passKind3;
    if (useDeferredMrt)
    {
        passKind3 = "DEFERRED";
    }
    else
    {
        passKind3 = "FORWARD";
    }
    UINT renderTargetCount;
    if (visibilityPass)
    {
        renderTargetCount = 1;
    }
    else
    {
        UINT renderTargetCountValue;
        if (useDeferredMrt)
        {
            renderTargetCountValue = g_kGEOMETRY_GBUFFER_COUNT;
        }
        else
        {
            renderTargetCountValue = 1;
        }
        renderTargetCount = (renderTargetCountValue);
    }
    Debug::Log("Creating PSO: VS=%s PS=%s Mode=%s RTV=%u\n", resource.vsPath, resolvedPsPath.c_str(), passKind3,
               renderTargetCount);

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;

    ShaderDescription vsResource = resource;
    vsResource.csoPath = resource.vsPath;
    vsResource.ppBlob = vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(vsResource))
    {
        return nullptr;
    }

    ShaderDescription psResource = resource;
    psResource.csoPath = resolvedPsPath.c_str();
    psResource.ppBlob = psBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(psResource))
    {
        return nullptr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.FrontCounterClockwise = resource.frontCounterClockwise;
    psoDesc.RasterizerState.CullMode = resource.cullMode;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (visibilityPass)
    {
        psoDesc.NumRenderTargets = 1;
    }
    else
    {
        UINT renderTargetCount3;
        if (useDeferredMrt)
        {
            renderTargetCount3 = g_kGEOMETRY_GBUFFER_COUNT;
        }
        else
        {
            renderTargetCount3 = 1;
        }
        psoDesc.NumRenderTargets = (renderTargetCount3);
    }
    if (visibilityPass)
    {
        psoDesc.RTVFormats[0] = GetGBufferFormat(GBufferType::VISIBILITY);
    }
    else if (useDeferredMrt)
    {
        for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
        {
            psoDesc.RTVFormats[i] = m_kDeferredRtvFormats[i];
        }
    }
    else
    {
        psoDesc.RTVFormats[0] = GetForwardRtvFormat();
    }
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    if (resource.isModel)
    {
        static D3D12_INPUT_ELEMENT_DESC modelLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        psoDesc.InputLayout = {modelLayout, _countof(modelLayout)};
        psoDesc.pRootSignature = m_ModelRootSignature.Get();
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
    }
    else
    {
        static D3D12_INPUT_ELEMENT_DESC layout2D[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        psoDesc.InputLayout = {layout2D, _countof(layout2D)};
        psoDesc.pRootSignature = m_RootSignature.Get();
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
    }

    if (enableAlphaBlend)
    {
        auto& rtBlend = psoDesc.BlendState.RenderTarget[0];
        rtBlend.BlendEnable = TRUE;
        rtBlend.SrcBlend = D3D12_BLEND_ONE;
        rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    }

    if (!resource.depthWrite)
    {
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    }
    if (oitPass)
    {
        // The transparent pass binds (scene, OIT accumulation, OIT revealage).
        // modelshaderPS writes the weighted blended OIT outputs (SV_Target1/2) and leaves the
        // scene untouched; every other shader keeps blending into the scene as before.
        const bool oitShader = resolvedPsPath.find("modelshaderPS.cso") != string::npos;
        psoDesc.NumRenderTargets = 3;
        psoDesc.RTVFormats[0] = GetForwardRtvFormat();
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[2] = TransparencyEffects::GetRevealFormat();
        psoDesc.BlendState.IndependentBlendEnable = TRUE;
        auto& accum = psoDesc.BlendState.RenderTarget[1];
        auto& reveal = psoDesc.BlendState.RenderTarget[2];
        accum = CD3DX12_BLEND_DESC(D3D12_DEFAULT).RenderTarget[0];
        reveal = CD3DX12_BLEND_DESC(D3D12_DEFAULT).RenderTarget[0];
        if (oitShader)
        {
            psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
            // sum(premultiplied colour * w, alpha * w)
            accum.BlendEnable = TRUE;
            accum.SrcBlend = accum.DestBlend = D3D12_BLEND_ONE;
            accum.SrcBlendAlpha = accum.DestBlendAlpha = D3D12_BLEND_ONE;
            accum.BlendOp = accum.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            // prod(1 - alpha)
            reveal.BlendEnable = TRUE;
            reveal.SrcBlend = D3D12_BLEND_ZERO;
            reveal.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
            reveal.SrcBlendAlpha = D3D12_BLEND_ZERO;
            reveal.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            reveal.BlendOp = reveal.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
        else
        {
            accum.RenderTargetWriteMask = 0;
            reveal.RenderTargetWriteMask = 0;
        }
    }

    ComPtr<ID3D12PipelineState> newPso{};
    if (!CreateGraphicsPipelineState(psoDesc, "renderer", newPso))
    {
        return nullptr;
    }

    m_PsoCache[key] = newPso;
    return newPso.Get();
}

ID3D12PipelineState* PsoManager::GetOrCreateToonOutlinePso(bool enableAlphaBlend)
{
    const bool useDeferredMrt = (m_RenderMode == RenderMode::DEFERRED && m_IsDeferredGeometryPass);
    const bool visibilityPass =
        useDeferredMrt && RendererSettings::GetComputeGBufferEnabled() && VisibilityBuffer::IsAvailable();
    UINT forwardRtvFmt;
    if (useDeferredMrt)
    {
        forwardRtvFmt = 0;
    }
    else
    {
        forwardRtvFmt = static_cast<UINT>(GetForwardRtvFormat());
    }
    const bool useAlphaBlend = enableAlphaBlend && !useDeferredMrt;
    const char* outlinePassKind;
    if (visibilityPass)
    {
        outlinePassKind = "VISIBILITY";
    }
    else if (useDeferredMrt)
    {
        outlinePassKind = "DEFERRED";
    }
    else
    {
        outlinePassKind = "FORWARD";
    }
    const char* outlineBlendKind;
    if (useAlphaBlend)
    {
        outlineBlendKind = "ALPHA";
    }
    else
    {
        outlineBlendKind = "OPAQUE";
    }
    const bool outlineOit = TransparencyEffects::IsOitPassActive() && !useDeferredMrt && !visibilityPass;
    const string key = string("TOON_OUTLINE|") + (outlinePassKind) + "|" + to_string(forwardRtvFmt) + "|" +
                       (outlineBlendKind) + (outlineOit ? "|OIT" : "");
    auto it = m_PsoCache.find(key);
    if (it != m_PsoCache.end())
    {
        return it->second.Get();
    }

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;

    ShaderDescription vsResource{};
    vsResource.csoPath = "shader\\hlsl\\build\\toonOutlineVS.cso";
    vsResource.ppBlob = vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(vsResource))
    {
        return nullptr;
    }

    ShaderDescription psResource{};
    if (visibilityPass)
    {
        psResource.csoPath = "shader\\hlsl\\build\\VisibilityOutlinePS.cso";
    }
    else
    {
        if (useDeferredMrt)
        {
            psResource.csoPath = "shader\\hlsl\\build\\toonOutlinePS_MRT.cso";
        }
        else
        {
            psResource.csoPath = "shader\\hlsl\\build\\toonOutlinePS.cso";
        }
    }
    psResource.ppBlob = psBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(psResource))
    {
        return nullptr;
    }

    static D3D12_INPUT_ELEMENT_DESC modelLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
    psoDesc.InputLayout = {modelLayout, _countof(modelLayout)};
    psoDesc.pRootSignature = m_ModelRootSignature.Get();
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    if (useAlphaBlend)
    {
        auto& rtBlend = psoDesc.BlendState.RenderTarget[0];
        rtBlend.BlendEnable = TRUE;
        rtBlend.SrcBlend = D3D12_BLEND_ONE;
        rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (visibilityPass)
    {
        psoDesc.NumRenderTargets = 1;
    }
    else
    {
        UINT renderTargetCount4;
        if (useDeferredMrt)
        {
            renderTargetCount4 = g_kGEOMETRY_GBUFFER_COUNT;
        }
        else
        {
            renderTargetCount4 = 1;
        }
        psoDesc.NumRenderTargets = (renderTargetCount4);
    }
    if (visibilityPass)
    {
        psoDesc.RTVFormats[0] = GetGBufferFormat(GBufferType::VISIBILITY);
    }
    else if (useDeferredMrt)
    {
        for (UINT i = 0; i < g_kGEOMETRY_GBUFFER_COUNT; ++i)
        {
            psoDesc.RTVFormats[i] = m_kDeferredRtvFormats[i];
        }
    }
    else
    {
        psoDesc.RTVFormats[0] = GetForwardRtvFormat();
    }
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    if (outlineOit)
    {
        // Outlines keep blending into the scene target; the OIT targets are not written.
        psoDesc.NumRenderTargets = 3;
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[2] = TransparencyEffects::GetRevealFormat();
        psoDesc.BlendState.IndependentBlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[1] = CD3DX12_BLEND_DESC(D3D12_DEFAULT).RenderTarget[0];
        psoDesc.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
        psoDesc.BlendState.RenderTarget[2] = psoDesc.BlendState.RenderTarget[1];
    }

    ComPtr<ID3D12PipelineState> pso;
    if (!CreateGraphicsPipelineState(psoDesc, "toon outline", pso))
    {
        return nullptr;
    }

    m_PsoCache[key] = pso;
    return pso.Get();
}

ID3D12PipelineState* PsoManager::GetOrCreateShadowMapPso()
{
    if (m_ShadowMapPso)
    {
        return m_ShadowMapPso.Get();
    }

    ShaderDescription resource{};
    resource.vsPath = "shader/hlsl/build/ShadowMapVS.cso";
    resource.csoPath = resource.vsPath;
    ComPtr<ID3DBlob> vsBlob;
    resource.ppBlob = vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(resource))
    {
        return nullptr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
    ComPtr<ID3DBlob> psBlob;
    resource.csoPath = "shader/hlsl/build/ShadowMapPS.cso";
    resource.ppBlob = psBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(resource))
    {
        return nullptr;
    }
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.pRootSignature = m_ModelRootSignature.Get();

    static D3D12_INPUT_ELEMENT_DESC modelLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    psoDesc.InputLayout = {modelLayout, _countof(modelLayout)};

    if (!CreateGraphicsPipelineState(psoDesc, "shadow map", m_ShadowMapPso))
    {
        return nullptr;
    }

    return m_ShadowMapPso.Get();
}

ID3D12PipelineState* PsoManager::GetOrCreateShadowMapInstancedPso()
{
    constexpr const char* key = "shadow_map_instanced";
    auto cached = m_PsoCache.find(key);
    if (cached != m_PsoCache.end())
    {
        return cached->second.Get();
    }

    ShaderDescription resource{};
    resource.vsPath = "shader/hlsl/build/ShadowMapInstancedVS.cso";
    resource.csoPath = resource.vsPath;
    ComPtr<ID3DBlob> vsBlob;
    resource.ppBlob = vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(resource))
    {
        return nullptr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.pRootSignature = m_ModelRootSignature.Get();

    static D3D12_INPUT_ELEMENT_DESC modelLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    psoDesc.InputLayout = {modelLayout, _countof(modelLayout)};

    ComPtr<ID3D12PipelineState> pso;
    if (!CreateGraphicsPipelineState(psoDesc, key, pso))
    {
        return nullptr;
    }
    m_PsoCache.emplace(key, pso);
    return pso.Get();
}

bool PsoManager::CreateSkinningPso()
{
    ShaderDescription resource{};
    resource.csoPath = "shader\\hlsl\\build\\skinning_cs.cso";
    resource.ppBlob = resource.csBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(resource))
    {
        Debug::Log("ERROR: Failed to load skinning_cs.cso. Skipping GPU skinning.\n");
        return true;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_SkinningRootSignature.Get();
    ID3DBlob* shaderBlob;
    if (resource.ppBlob)
    {
        shaderBlob = *resource.ppBlob;
    }
    else
    {
        shaderBlob = nullptr;
    }
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(shaderBlob);

    if (!CreateComputePipelineState(psoDesc, "skinning", m_SkinningPso))
    {
        return false;
    }

    return true;
}

bool PsoManager::CreatePostProcessPipelines()
{
    auto CreatePPPSO = [&](const char* psPath, PostProcessType type, DXGI_FORMAT targetFormat = DXGI_FORMAT_UNKNOWN,
                           ComPtr<ID3D12PipelineState>* output = nullptr) {
        ShaderDescription resource{};
        resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
        resource.psPath = psPath;
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        ShaderDescription vsResource = resource;
        vsResource.csoPath = resource.vsPath;
        vsResource.ppBlob = vsBlob.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(vsResource))
        {
            return false;
        }
        ShaderDescription psResource = resource;
        psResource.csoPath = resource.psPath;
        psResource.ppBlob = psBlob.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(psResource))
        {
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
        psoDesc.pRootSignature = m_PostProcessRootSignature.Get();
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = targetFormat == DXGI_FORMAT_UNKNOWN ? m_SceneColorFormat : targetFormat;
        psoDesc.SampleDesc.Count = 1;

        ComPtr<ID3D12PipelineState> pso;
        if (CreateGraphicsPipelineState(psoDesc, "post process", pso))
        {
            if (output)
            {
                *output = pso;
            }
            else
            {
                m_PostProcessPsoMap[type] = pso;
            }
            return true;
        }
        return false;
    };

    CreatePPPSO("shader\\hlsl\\build\\postProcessNonePS.cso", PostProcessType::NONE);
    CreatePPPSO("shader\\hlsl\\build\\postProcessBlurPS.cso", PostProcessType::BLUR);
    CreatePPPSO("shader\\hlsl\\build\\postProcessSepiaPS.cso", PostProcessType::SEPIA);
    CreatePPPSO("shader\\hlsl\\build\\postProcessGrayPS.cso", PostProcessType::GRAYSCALE);
    CreatePPPSO("shader\\hlsl\\build\\postProcessInvertPS.cso", PostProcessType::INVERT);
    CreatePPPSO("shader\\hlsl\\build\\postProcessBloomPS.cso", PostProcessType::BLOOM);
    if (!CreatePPPSO("shader\\hlsl\\build\\postProcessBloomPS.cso", PostProcessType::BLOOM,
                     GetGBufferFormat(GBufferType::BLOOM), &m_BloomExtractPso))
    {
        return false;
    }
    return true;
}

bool PsoManager::CreateDeferredLightingPipeline()
{
    ShaderDescription resource{};
    resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
    resource.psPath = "shader\\hlsl\\build\\DeferredLightingPS.cso";

    ShaderDescription vsResource = resource;
    vsResource.csoPath = resource.vsPath;
    vsResource.ppBlob = resource.vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(vsResource))
    {
        return false;
    }
    ShaderDescription psResource = resource;
    psResource.csoPath = resource.psPath;
    psResource.ppBlob = resource.psBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(psResource))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
    psoDesc.pRootSignature = m_PostProcessRootSignature.Get();
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_SceneColorFormat;
    psoDesc.SampleDesc.Count = 1;

    return CreateGraphicsPipelineState(psoDesc, "deferred lighting", m_DeferredLightingPso);
}

bool PsoManager::CreateAtmospherePso()
{
    ShaderDescription resource{};
    resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
    resource.psPath = "shader\\hlsl\\build\\AtmosphereGBufferPS.cso";

    ShaderDescription vsResource = resource;
    vsResource.csoPath = resource.vsPath;
    vsResource.ppBlob = resource.vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(vsResource))
    {
        return false;
    }
    ShaderDescription psResource = resource;
    psResource.csoPath = resource.psPath;
    psResource.ppBlob = resource.psBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(psResource))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
    psoDesc.pRootSignature = m_PostProcessRootSignature.Get();
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    return CreateGraphicsPipelineState(psoDesc, "atmosphere gbuffer", m_AtmospherePso);
}

bool PsoManager::CreateUpscalePso()
{
    const char* psPath = "shader\\hlsl\\build\\UpscaleBilateralPS.cso";

    ShaderDescription resource{};
    resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
    resource.psPath = psPath;

    ShaderDescription vsResource = resource;
    vsResource.csoPath = resource.vsPath;
    vsResource.ppBlob = resource.vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(vsResource))
    {
        return false;
    }

    ShaderDescription psResource = resource;
    psResource.csoPath = resource.psPath;
    psResource.ppBlob = resource.psBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(psResource))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
    psoDesc.pRootSignature = m_UpscaleRootSignature.Get();
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_SceneColorFormat;
    psoDesc.SampleDesc.Count = 1;

    return CreateGraphicsPipelineState(psoDesc, "UpscaleBilateralPso", m_UpscaleBilateralPso);
}

bool PsoManager::CreateUpscaleDepthPso()
{
    ShaderDescription resource{};
    resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
    resource.psPath = "shader\\hlsl\\build\\UpscaleDepthPS.cso";

    ShaderDescription vsResource = resource;
    vsResource.csoPath = resource.vsPath;
    vsResource.ppBlob = resource.vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(vsResource))
    {
        return false;
    }

    ShaderDescription psResource = resource;
    psResource.csoPath = resource.psPath;
    psResource.ppBlob = resource.psBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(psResource))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
    psoDesc.pRootSignature = m_UpscaleRootSignature.Get();
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    return CreateGraphicsPipelineState(psoDesc, "UpscaleDepthPso", m_UpscaleDepthPso);
}

bool PsoManager::CreateVelocityPso()
{
    ShaderDescription resource{};
    resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
    resource.psPath = "shader\\hlsl\\build\\VelocityPS.cso";

    ShaderDescription vsResource = resource;
    vsResource.csoPath = resource.vsPath;
    vsResource.ppBlob = resource.vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(vsResource))
    {
        return false;
    }

    ShaderDescription psResource = resource;
    psResource.csoPath = resource.psPath;
    psResource.ppBlob = resource.psBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(psResource))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
    psoDesc.pRootSignature = m_AaRootSignature.Get();
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = GetGBufferFormat(GBufferType::VELOCITY);
    psoDesc.SampleDesc.Count = 1;

    return CreateGraphicsPipelineState(psoDesc, "VelocityPso", m_VelocityPso);
}

bool PsoManager::CreateVelocityGeometryPso()
{
    ShaderDescription resource{};
    resource.vsPath = "shader\\hlsl\\build\\VelocityGeometryVS.cso";
    resource.psPath = "shader\\hlsl\\build\\VelocityGeometryPS.cso";
    ShaderDescription vsResource = resource;
    vsResource.csoPath = resource.vsPath;
    vsResource.ppBlob = resource.vsBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(vsResource))
    {
        return false;
    }
    ShaderDescription psResource = resource;
    psResource.csoPath = resource.psPath;
    psResource.ppBlob = resource.psBlob.GetAddressOf();
    if (!ShaderPipelines::LoadShaderBlob(psResource))
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"PREVPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
    desc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
    desc.InputLayout = {layout, _countof(layout)};
    desc.pRootSignature = m_ModelRootSignature.Get();
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = GetGBufferFormat(GBufferType::VELOCITY);
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    return CreateGraphicsPipelineState(desc, "VelocityGeometryPso", m_VelocityGeometryPso);
}

bool PsoManager::CreateAaPsos()
{
    auto CreateAAPSO = [&](const char* psPath, ComPtr<ID3D12PipelineState>& outPso, const char* debugName) {
        ShaderDescription resource{};
        resource.vsPath = "shader\\hlsl\\build\\postProcessVS.cso";
        resource.psPath = psPath;

        ShaderDescription vsResource = resource;
        vsResource.csoPath = resource.vsPath;
        vsResource.ppBlob = resource.vsBlob.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(vsResource))
        {
            return false;
        }
        ShaderDescription psResource = resource;
        psResource.csoPath = resource.psPath;
        psResource.ppBlob = resource.psBlob.GetAddressOf();
        if (!ShaderPipelines::LoadShaderBlob(psResource))
        {
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(resource.vsBlob.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(resource.psBlob.Get());
        psoDesc.pRootSignature = m_AaRootSignature.Get();
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = m_SceneColorFormat;
        psoDesc.SampleDesc.Count = 1;

        return CreateGraphicsPipelineState(psoDesc, debugName, outPso);
    };

    if (!CreateAAPSO("shader\\hlsl\\build\\FXAA_PS.cso", m_FxaaPso, "FxaaPso"))
    {
        return false;
    }
    if (!CreateAAPSO("shader\\hlsl\\build\\TAA_BlendPS.cso", m_TaaBlendPso, "TaaBlendPso"))
    {
        return false;
    }

    return true;
}

ID3D12PipelineState* PsoManager::GetPostProcessPso(PostProcessType type)
{
    auto psoIt = m_PostProcessPsoMap.find(type);
    if (psoIt == m_PostProcessPsoMap.end() || !psoIt->second)
    {
        psoIt = m_PostProcessPsoMap.find(PostProcessType::NONE);
    }
    if (psoIt == m_PostProcessPsoMap.end() || !psoIt->second)
    {
        return nullptr;
    }
    return psoIt->second.Get();
}
